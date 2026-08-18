#include "order_book.h"

#include <algorithm>
#include <cstring>

// The delivered stale flag is derived, never stored: every snapshot handed to
// callbacks (or returned by get_book) is stamped from the synced/suspect pair
// at copy time. Each delivery event copies the book exactly once; callbacks
// receive a const reference to that copy, valid only during the call.

std::string OrderBookManager::trim_prod_id(const char* prod_id) {
    size_t len = 0;
    while (len < 20 && prod_id[len] != '\0') ++len;
    while (len > 0 && prod_id[len - 1] == ' ') --len;
    return std::string(prod_id, len);
}

std::array<OrderBookLevel, TAIFEX_BOOK_DEPTH>* OrderBookManager::side_ladder(
    OrderBook& book, char entry_type) {
    switch (entry_type) {
        case '0': return &book.bids;
        case '1': return &book.asks;
        case 'E': return &book.derived_bids;
        case 'F': return &book.derived_asks;
        default:  return nullptr;
    }
}

// I081 MD-UPDATE-ACTION semantics per the TAIFEX manual's worked examples:
// New inserts at the level and shifts lower levels down (level 5 falls off
// silently); Delete removes the level and shifts the rest up; Change and
// Overlay rewrite the level in place. Overlay with price=0/qty=0 is how the
// exchange announces a cleared derived quote.
void OrderBookManager::apply_entry(OrderBook& book, const MDEntry& entry) {
    auto* ladder = side_ladder(book, entry.entry_type);
    if (!ladder) return;
    if (entry.price_level < 1 || entry.price_level > TAIFEX_BOOK_DEPTH) return;
    int idx = entry.price_level - 1;

    switch (entry.update_action) {
        case '0':  // New
            for (int i = TAIFEX_BOOK_DEPTH - 1; i > idx; --i)
                (*ladder)[i] = (*ladder)[i - 1];
            (*ladder)[idx] = {true, entry.price_sign, entry.price, entry.quantity};
            break;
        case '1':  // Change
            (*ladder)[idx] = {true, entry.price_sign, entry.price, entry.quantity};
            break;
        case '2':  // Delete
            for (int i = idx; i < TAIFEX_BOOK_DEPTH - 1; ++i)
                (*ladder)[i] = (*ladder)[i + 1];
            (*ladder)[TAIFEX_BOOK_DEPTH - 1] = OrderBookLevel{};
            break;
        case '5':  // Overlay
            if (entry.price == 0 && entry.quantity == 0)
                (*ladder)[idx] = OrderBookLevel{};
            else
                (*ladder)[idx] = {true, entry.price_sign, entry.price, entry.quantity};
            break;
        default:
            break;  // unknown action: skip entry
    }
}

OrderBookManager::ProductState& OrderBookManager::get_or_create_locked(
    const char* prod_id) {
    std::string key = trim_prod_id(prod_id);
    auto it = products_.find(key);
    if (it == products_.end()) {
        it = products_.emplace(key, ProductState{}).first;
        std::memcpy(it->second.book.prod_id, prod_id, 20);
        it->second.book.prod_id[20] = '\0';
    }
    return it->second;
}

bool OrderBookManager::track_seq_locked(ProductState& st, uint32_t seq) {
    OrderBook& book = st.book;
    if (book.last_prod_msg_seq == 0) {
        // First message ever seen for this product. The serial starts at 1
        // each session (and after an I002 reset), so seq == 1 proves a
        // complete chain from an empty book; anything else means we joined
        // mid-stream with an unknown base.
        st.synced = (seq == 1);
        st.suspect = false;
        book.last_prod_msg_seq = seq;
        return true;
    }
    if (seq <= book.last_prod_msg_seq) return false;  // duplicate / retransmit
    if (seq == book.last_prod_msg_seq + 1) {
        // Contiguity proof: whatever was lost on the channel (if anything)
        // did not concern this product.
        st.suspect = false;
    } else {
        // A message for THIS product was lost; the book chain is broken.
        st.synced = false;
        st.suspect = false;  // no longer merely suspected — it is known
    }
    book.last_prod_msg_seq = seq;
    return true;
}

void OrderBookManager::adopt_snapshot_locked(ProductState& st, const char* prod_id,
                                             uint32_t seq,
                                             const std::vector<SnapshotEntry>& entries,
                                             const char* info_time) {
    OrderBook& book = st.book;
    book.bids.fill(OrderBookLevel{});
    book.asks.fill(OrderBookLevel{});
    book.derived_bids.fill(OrderBookLevel{});
    book.derived_asks.fill(OrderBookLevel{});
    for (const SnapshotEntry& entry : entries) {
        auto* ladder = side_ladder(book, entry.entry_type);
        if (!ladder) continue;
        if (entry.price_level < 1 || entry.price_level > TAIFEX_BOOK_DEPTH) continue;
        (*ladder)[entry.price_level - 1] =
            {true, entry.price_sign, entry.price, entry.quantity};
    }
    std::memcpy(book.prod_id, prod_id, 20);
    book.prod_id[20] = '\0';
    std::memcpy(book.info_time, info_time, sizeof(book.info_time));
    book.last_prod_msg_seq = seq;
    book.has_snapshot = true;
    st.synced = true;
    st.suspect = false;
}

void OrderBookManager::collect_reset_locked(std::vector<Delivery>& out) {
    // The reset invalidates every book; tell subscribers whose last delivered
    // snapshot claimed to be trusted (mirrors the channel-gap stale-flip
    // sweep).
    for (auto& kv : products_) {
        ProductState& st = kv.second;
        if (st.suspect || !st.synced) continue;  // already delivered as stale
        collect_delivery_locked(kv.first, st.book, /*is_stale=*/true, out);
    }
    products_.clear();
    channels_.clear();
}

void OrderBookManager::collect_delivery_locked(
    const std::string& key, const OrderBook& live_book, bool is_stale,
    std::vector<Delivery>& out) const {
    auto it = callbacks_.find(key);
    bool has_product_cbs = (it != callbacks_.end() && !it->second.empty());
    if (!has_product_cbs && wildcard_callbacks_.empty()) return;  // no copy

    Delivery d;
    d.book = live_book;  // the single snapshot copy for this event
    d.book.is_stale = is_stale;
    if (has_product_cbs)
        d.callbacks.insert(d.callbacks.end(), it->second.begin(),
                           it->second.end());
    d.callbacks.insert(d.callbacks.end(), wildcard_callbacks_.begin(),
                       wildcard_callbacks_.end());
    out.push_back(std::move(d));
}

void OrderBookManager::fire(std::vector<Delivery>& deliveries) {
    for (Delivery& d : deliveries)
        for (const CallbackPtr& cb : d.callbacks) (*cb)(d.book);
}

// --- Feed methods ---

void OrderBookManager::on_header(const Header& header) {
    std::vector<Delivery> deliveries;
    {
        std::lock_guard<std::mutex> lock(mtx_);

        if (header.message_kind == '2') {
            // I002 sequence reset: the spec mandates clearing the order books
            // and every sequence tracker. Subscribers are told their book is
            // no longer trusted before the state disappears.
            collect_reset_locked(deliveries);
        } else {
            do {
                auto it = channels_.find(header.channel_id);
                if (it == channels_.end()) {
                    ChannelState ch;
                    ch.last_seq = header.channel_seq;
                    ch.is_snapshot_channel = (header.message_kind == 'C');
                    channels_.emplace(header.channel_id, ch);
                    break;  // first message on channel: nothing provable
                }

                ChannelState& ch = it->second;
                if (header.message_kind == 'C') ch.is_snapshot_channel = true;

                uint32_t seq = header.channel_seq;
                bool gap;
                if (header.message_kind == '1') {
                    // I001 heartbeat repeats the channel's last sequence; a
                    // larger value reveals messages lost on an otherwise idle
                    // channel.
                    gap = (seq > ch.last_seq);
                } else {
                    if (seq <= ch.last_seq) break;  // duplicate / reorder
                    gap = (seq != ch.last_seq + 1);
                }
                ch.last_seq = std::max(ch.last_seq, seq);

                // The I084 carousel resets its CHANNEL-SEQ each cycle and
                // losing part of a cycle is harmless (each 'O' is
                // self-contained), so snapshot channel gaps are ignored.
                if (!gap || ch.is_snapshot_channel) break;

                // A packet was lost on a realtime channel: any product's
                // chain may be broken. Mark every synced book suspect until
                // its next contiguous message proves otherwise (or a snapshot
                // re-bases it). Deliver the stale-flag flip to registered
                // callbacks.
                for (auto& kv : products_) {
                    ProductState& st = kv.second;
                    if (st.suspect || !st.synced) continue;  // already stale
                    st.suspect = true;
                    collect_delivery_locked(kv.first, st.book,
                                            /*is_stale=*/true, deliveries);
                }
            } while (false);
        }
    }
    fire(deliveries);
}

void OrderBookManager::on_i024(const I024_Packet& pkt) {
    std::vector<Delivery> deliveries;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ProductState& st = get_or_create_locked(pkt.prod_id);
        bool stale_before = !st.synced || st.suspect;
        if (!track_seq_locked(st, pkt.prod_msg_seq)) return;
        bool stale_after = !st.synced || st.suspect;
        if (stale_after == stale_before) return;  // book content unchanged
        collect_delivery_locked(trim_prod_id(pkt.prod_id), st.book, stale_after,
                                deliveries);
    }
    fire(deliveries);
}

void OrderBookManager::on_i025(const I025_Packet& pkt) {
    std::vector<Delivery> deliveries;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ProductState& st = get_or_create_locked(pkt.prod_id);
        bool stale_before = !st.synced || st.suspect;
        if (!track_seq_locked(st, pkt.prod_msg_seq)) return;
        bool stale_after = !st.synced || st.suspect;
        if (stale_after == stale_before) return;
        collect_delivery_locked(trim_prod_id(pkt.prod_id), st.book, stale_after,
                                deliveries);
    }
    fire(deliveries);
}

void OrderBookManager::on_i081(const I081_Packet& pkt) {
    std::vector<Delivery> deliveries;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ProductState& st = get_or_create_locked(pkt.prod_id);
        if (!track_seq_locked(st, pkt.prod_msg_seq)) return;  // duplicate: drop
        // Apply best-effort even when the chain is broken; the delivered
        // snapshot carries the stale flag.
        for (const MDEntry& entry : pkt.entries) apply_entry(st.book, entry);
        std::memcpy(st.book.info_time, pkt.header.info_time,
                    sizeof(st.book.info_time));
        collect_delivery_locked(trim_prod_id(pkt.prod_id), st.book,
                                !st.synced || st.suspect, deliveries);
    }
    fire(deliveries);
}

void OrderBookManager::on_i083(const I083_Packet& pkt) {
    std::vector<Delivery> deliveries;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ProductState& st = get_or_create_locked(pkt.prod_id);

        if (pkt.calculated_flag == '1') {
            // Trial (pre-open) book: not adopted, but it still consumes a
            // PROD-MSG-SEQ, so it must be tracked.
            bool stale_before = !st.synced || st.suspect;
            if (!track_seq_locked(st, pkt.prod_msg_seq)) return;
            bool stale_after = !st.synced || st.suspect;
            if (stale_after == stale_before) return;
            collect_delivery_locked(trim_prod_id(pkt.prod_id), st.book,
                                    stale_after, deliveries);
        } else {
            if (st.book.last_prod_msg_seq != 0 &&
                pkt.prod_msg_seq < st.book.last_prod_msg_seq)
                return;  // older than the live book: ignore
            adopt_snapshot_locked(st, pkt.prod_id, pkt.prod_msg_seq, pkt.entries,
                                  pkt.header.info_time);
            collect_delivery_locked(trim_prod_id(pkt.prod_id), st.book,
                                    /*is_stale=*/false, deliveries);
        }
    }
    fire(deliveries);
}

void OrderBookManager::on_i084(const I084_Packet& pkt) {
    if (pkt.message_type != 'O') return;  // 'A'/'Z'/'S'/'P' carry no book data
    for (const I084Product& prod : pkt.products) {
        std::vector<Delivery> deliveries;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            ProductState& st = get_or_create_locked(prod.prod_id);
            if (st.book.last_prod_msg_seq != 0 &&
                prod.last_prod_msg_seq < st.book.last_prod_msg_seq)
                continue;  // snapshot older than the live book: ignore
            adopt_snapshot_locked(st, prod.prod_id, prod.last_prod_msg_seq,
                                  prod.entries, pkt.header.info_time);
            collect_delivery_locked(trim_prod_id(prod.prod_id), st.book,
                                    /*is_stale=*/false, deliveries);
        }
        fire(deliveries);
    }
}

// --- Registration / introspection ---
// Wrapping into shared_ptr / letting the last reference die happens OUTSIDE
// the lock: copying or destroying a pybind-wrapped std::function acquires
// the GIL, which must never be done while holding mtx_.

void OrderBookManager::register_callback(const std::string& prod_id,
                                         BookCallback cb) {
    CallbackPtr ptr = std::make_shared<BookCallback>(std::move(cb));
    std::lock_guard<std::mutex> lock(mtx_);
    callbacks_[trim_prod_id(prod_id.c_str())].push_back(std::move(ptr));
}

void OrderBookManager::register_callback_all(BookCallback cb) {
    CallbackPtr ptr = std::make_shared<BookCallback>(std::move(cb));
    std::lock_guard<std::mutex> lock(mtx_);
    wildcard_callbacks_.push_back(std::move(ptr));
}

void OrderBookManager::unregister_callbacks(const std::string& prod_id) {
    std::vector<CallbackPtr> removed;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = callbacks_.find(trim_prod_id(prod_id.c_str()));
        if (it == callbacks_.end()) return;
        removed = std::move(it->second);
        callbacks_.erase(it);
    }
    // `removed` destroys the callbacks here, after unlock.
}

void OrderBookManager::unregister_all_callbacks() {
    std::unordered_map<std::string, std::vector<CallbackPtr>> removed;
    std::vector<CallbackPtr> removed_wildcard;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        removed = std::move(callbacks_);
        callbacks_.clear();
        removed_wildcard = std::move(wildcard_callbacks_);
        wildcard_callbacks_.clear();
    }
    // Moved-out containers destroy the callbacks here, after unlock.
}

std::optional<OrderBook> OrderBookManager::get_book(
    const std::string& prod_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = products_.find(trim_prod_id(prod_id.c_str()));
    if (it == products_.end()) return std::nullopt;
    OrderBook copy = it->second.book;
    copy.is_stale = !it->second.synced || it->second.suspect;
    return copy;
}

std::vector<std::string> OrderBookManager::product_ids() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> ids;
    ids.reserve(products_.size());
    for (const auto& kv : products_) ids.push_back(kv.first);
    return ids;
}

void OrderBookManager::reset() {
    std::vector<Delivery> deliveries;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        collect_reset_locked(deliveries);
    }
    fire(deliveries);
}
