#ifndef TAIFEX_ORDER_BOOK_H
#define TAIFEX_ORDER_BOOK_H

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "parser.h"

// TAIFEX discloses at most 5 price levels per side.
inline constexpr int TAIFEX_BOOK_DEPTH = 5;

struct OrderBookLevel {
    bool valid = false;         // level occupied
    char price_sign = '0';      // '0': positive, '-': negative
    uint64_t price = 0;         // raw 9(9) integer price
    uint32_t quantity = 0;
};

// A per-instrument order book snapshot delivered to user callbacks.
// Ladders are indexed by wire MD-PRICE-LEVEL minus 1 (levels are 1-based on
// the wire): bids[0] = best bid (level 1), bids[4] = level 5. Same for all
// sides. entry_type '0' -> bids, '1' -> asks, 'E' -> derived_bids,
// 'F' -> derived_asks.
struct OrderBook {
    char prod_id[21] = {0};          // raw padded wire form
    uint32_t last_prod_msg_seq = 0;  // PROD-MSG-SEQ of last message applied/adopted
    bool is_stale = true;            // true = increment chain broken or unproven
    bool has_snapshot = false;       // ever adopted an I083 / I084-'O' base
    // When the CONTENT last changed: the header INFORMATION-TIME of the last
    // realtime message (I081) or realtime snapshot (I083) applied. An I084
    // carousel adoption does NOT touch it — the carousel re-broadcasts every
    // book on a fixed cycle and its 'O' block carries no time of its own, so
    // the broadcast instant says nothing about when the content changed
    // (measured median gap: ~15 minutes). Empty until the first realtime
    // message for the product.
    char info_time[16] = {0};
    // Broadcast INFORMATION-TIME of the snapshot message (I083 or I084 'O')
    // that last re-based this book; empty if never re-based from a snapshot.
    char snapshot_time[16] = {0};
    std::array<OrderBookLevel, TAIFEX_BOOK_DEPTH> bids{};
    std::array<OrderBookLevel, TAIFEX_BOOK_DEPTH> asks{};
    std::array<OrderBookLevel, TAIFEX_BOOK_DEPTH> derived_bids{};
    std::array<OrderBookLevel, TAIFEX_BOOK_DEPTH> derived_asks{};
};

// Maintains current order books for every product seen on the feed, built
// from I083 snapshots + I081 increments, with the I084 snapshot carousel (or
// a later I083) as loss recovery. Gap/staleness rules:
//
//   - PROD-MSG-SEQ is one serial per product shared across I024/I025/I081/
//     I083, so the manager must also consume I024/I025 to keep per-product
//     continuity (a trade legitimately advances the serial between two I081s).
//   - A per-product sequence gap marks the book stale ("synced" lost);
//     increments keep being applied best-effort and callbacks keep firing
//     with is_stale = true.
//   - A realtime-channel CHANNEL-SEQ gap (fed via on_header, including I001
//     heartbeats that reveal missed messages on idle channels) marks every
//     book "suspect"; a product's next contiguous message clears its suspect
//     flag (proof that the loss did not concern it).
//   - A snapshot (normal I083, or each I084 'O' product entry) with
//     seq >= last is adopted wholesale and clears both flags.
//   - I002 sequence reset (message_kind '2') on a realtime channel clears
//     all books and per-product serials, as mandated by the TAIFEX spec. The
//     spec scopes that wipe to 即時行情 groups, so an I002 arriving on a
//     channel known to carry snapshots (I084) resets only that channel's own
//     sequence tracker. A channel not yet seen carrying I084 is treated as
//     realtime (the identity is learned from traffic).
//
// Delivered is_stale = !synced || suspect.
//
// Thread safety: all feed methods may be called concurrently (e.g. a realtime
// parser thread and a snapshot parser thread). A single mutex guards state.
// User callbacks are never copied, destroyed or invoked while the mutex is
// held: they are stored behind shared_ptr (collection under the lock only
// copies pointers) and fired after unlock. This keeps the critical section
// free of Python/GIL work, so callbacks may safely re-enter the manager and
// pybind GIL acquisition cannot deadlock against the mutex.
//
// Delivery contract: each book-change event makes exactly ONE snapshot copy,
// shared by-reference across every callback registered for the product. The
// reference passed to a callback is valid ONLY for the duration of the call —
// copy the OrderBook explicitly if you need to keep it. (Python callbacks are
// unaffected: pybind converts the const& argument into a new Python object at
// the boundary.)
class OrderBookManager {
public:
    using BookCallback = std::function<void(const OrderBook&)>;

    // --- Feed methods (wire into TaifexParser, any thread) ---
    // Channel tracking; call for EVERY checksum-valid message's header
    // (Parser::set_order_book_manager does this automatically).
    void on_header(const Header& header);
    // Advance per-product sequence only (no book content change).
    void on_i024(const I024_Packet& pkt);
    void on_i025(const I025_Packet& pkt);
    // Book-content feeds.
    void on_i081(const I081_Packet& pkt);
    void on_i083(const I083_Packet& pkt);
    void on_i084(const I084_Packet& pkt);

    // --- Callback registration ---
    // prod_id keys are matched with trailing spaces/NULs trimmed, so
    // register_callback("TXFG6") matches the padded wire form.
    void register_callback(const std::string& prod_id, BookCallback cb);
    void register_callback_all(BookCallback cb);            // every product
    void unregister_callbacks(const std::string& prod_id);  // all for product
    void unregister_all_callbacks();                        // per-product + wildcard

    // --- Introspection (copies under lock) ---
    std::optional<OrderBook> get_book(const std::string& prod_id) const;
    std::vector<std::string> product_ids() const;

    // Clear all books and sequence trackers (I002-equivalent). Like an I002,
    // it first delivers a stale-flagged copy of every currently-trusted book.
    void reset();

private:
    struct ProductState {
        OrderBook book;
        bool synced = false;   // book content is an unbroken chain from a snapshot
        bool suspect = false;  // a realtime channel gap occurred, product unproven since
    };
    struct ChannelState {
        uint32_t last_seq = 0;
        bool is_snapshot_channel = false;  // has carried message_kind 'C' (I084)
    };

    // Callbacks are held behind shared_ptr so that collecting them under the
    // mutex copies only pointers — copying/destroying a pybind-wrapped
    // std::function acquires the GIL, which must never happen under mtx_
    // (a Python thread blocked on mtx_ while holding the GIL would deadlock).
    using CallbackPtr = std::shared_ptr<BookCallback>;
    // One per delivery event: the single snapshot copy for the event plus
    // every callback it goes to. fire() hands each callback a const reference
    // to `book`, valid only for the duration of the call.
    struct Delivery {
        OrderBook book;
        std::vector<CallbackPtr> callbacks;
    };

    static std::string trim_prod_id(const char* prod_id);
    static std::array<OrderBookLevel, TAIFEX_BOOK_DEPTH>* side_ladder(
        OrderBook& book, char entry_type);
    static void apply_entry(OrderBook& book, const MDEntry& entry);

    // All *_locked helpers assume mtx_ is held.
    ProductState& get_or_create_locked(const char* prod_id);
    // Advance per-product seq; returns true when the message is new
    // (seq > last). Handles duplicate drop and gap -> synced=false.
    bool track_seq_locked(ProductState& st, uint32_t seq);
    // Replace the book content wholesale. content_time stamps info_time
    // (pass nullptr to leave it — the I084 carousel case, whose broadcast
    // time is not a content time); snapshot_time is always stamped.
    void adopt_snapshot_locked(ProductState& st, const char* prod_id,
                               uint32_t seq, const std::vector<SnapshotEntry>& entries,
                               const char* content_time, const char* snapshot_time);
    // Deliver is_stale=true for every currently-trusted book, then clear
    // the books, per-product serials and the snapshot quarantine. Channel
    // trackers are NOT touched: an I002 resets only its own group's serial,
    // and reset() layers the full tracker wipe on top.
    void collect_book_wipe_locked(std::vector<Delivery>& out);
    // Full I002-equivalent reset (books + every channel tracker); reset().
    void collect_reset_locked(std::vector<Delivery>& out);
    // Append one Delivery for the product: a single stamped snapshot copy of
    // `live_book` plus the callbacks registered for it (per-product first,
    // then wildcard). Appends nothing — and skips the copy — when no callback
    // is registered for the product.
    void collect_delivery_locked(const std::string& key,
                                 const OrderBook& live_book, bool is_stale,
                                 std::vector<Delivery>& out) const;

    static void fire(std::vector<Delivery>& deliveries);

    mutable std::mutex mtx_;
    // Set by a reset (I002 or reset()): an I084 'O' block that was built
    // BEFORE the reset can still be in flight (the carousel round spans
    // seconds, the socket buffer holds more), and adopting it would re-base
    // a freshly cleared product at the pre-reset serial — after which every
    // post-reset message is dropped as a duplicate, forever, while the book
    // reports fresh. Per the manual's recovery rule (discard snapshot data,
    // wait for the next Refresh Begin), 'O' blocks are ignored while this is
    // set; the next I084 'A' clears it, since everything after that Begin
    // was assembled by the exchange after its own reset.
    bool snapshot_quarantine_ = false;
    std::unordered_map<std::string, ProductState> products_;   // key: trimmed prod_id
    std::unordered_map<uint16_t, ChannelState> channels_;      // key: channel_id
    std::unordered_map<std::string, std::vector<CallbackPtr>> callbacks_;
    std::vector<CallbackPtr> wildcard_callbacks_;
};

#endif
