// Assert-based unit tests for OrderBookManager (no framework — the repo has
// none). Feeds packet structs directly; no UDP or BCD involved.
//
// Build: cmake target `order_book_test` (TAIFEX_BUILD_TESTS=ON, default).

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "order_book.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define RUN(fn)                       \
    do {                              \
        int before = g_failures;      \
        fn();                         \
        std::printf("[%s] %s\n", g_failures == before ? "PASS" : "FAIL", #fn); \
    } while (0)

// --- Packet builders -------------------------------------------------------

// Real PROD-ID short code: symbol + month letter (A=Jan..L=Dec) + year digit.
static const char* kProd = "TXFG6";
static const uint16_t kRtChannel = 1;
static const uint16_t kSnapChannel = 9;

static void set_prod(char (&dst)[21], const char* prod) {
    std::memset(dst, ' ', 20);
    dst[20] = '\0';
    std::memcpy(dst, prod, std::strlen(prod));
}

static Header make_header(char kind, uint32_t channel_seq,
                          uint16_t channel_id = kRtChannel) {
    Header h{};
    h.transmission_code = '2';
    h.message_kind = kind;
    std::snprintf(h.info_time, sizeof(h.info_time), "09:00:00.000000");
    h.channel_id = channel_id;
    h.channel_seq = channel_seq;
    h.version_no = 1;
    h.body_len = 0;
    return h;
}

static MDEntry make_entry(char action, char type, uint64_t price, uint32_t qty,
                          uint8_t level, char sign = '0') {
    MDEntry e{};
    e.update_action = action;
    e.entry_type = type;
    e.price_sign = sign;
    e.price = price;
    e.quantity = qty;
    e.price_level = level;
    e.decimal_locator = 3;
    return e;
}

static I081_Packet make_i081(uint32_t seq, std::vector<MDEntry> entries,
                             uint32_t channel_seq, const char* prod = kProd) {
    I081_Packet p{};
    p.header = make_header('A', channel_seq);
    set_prod(p.prod_id, prod);
    p.prod_msg_seq = seq;
    p.no_md_entries = (uint8_t)entries.size();
    p.entries = std::move(entries);
    return p;
}

static I083_Packet make_i083(uint32_t seq, std::vector<MDEntry> entries,
                             uint32_t channel_seq, char calc = '0',
                             const char* prod = kProd) {
    I083_Packet p{};
    p.header = make_header('B', channel_seq);
    set_prod(p.prod_id, prod);
    p.prod_msg_seq = seq;
    p.calculated_flag = calc;
    p.no_md_entries = (uint8_t)entries.size();
    p.entries = std::move(entries);
    for (auto& e : p.entries) e.update_action = ' ';
    return p;
}

struct SnapProduct {
    const char* prod;
    uint32_t last_prod_msg_seq;
    std::vector<MDEntry> entries;
};

static I084_Packet make_i084_o_multi(std::vector<SnapProduct> products,
                                     uint32_t channel_seq) {
    I084_Packet p{};
    p.header = make_header('C', channel_seq, kSnapChannel);
    p.message_type = 'O';
    p.no_entries = (uint8_t)products.size();
    for (auto& sp : products) {
        I084Product prod_blk{};
        set_prod(prod_blk.prod_id, sp.prod);
        prod_blk.last_prod_msg_seq = sp.last_prod_msg_seq;
        prod_blk.no_md_entries = (uint8_t)sp.entries.size();
        prod_blk.entries = std::move(sp.entries);
        p.products.push_back(std::move(prod_blk));
    }
    return p;
}

static I084_Packet make_i084_o(uint32_t last_prod_msg_seq,
                               std::vector<MDEntry> entries,
                               uint32_t channel_seq, const char* prod = kProd) {
    return make_i084_o_multi({{prod, last_prod_msg_seq, std::move(entries)}},
                             channel_seq);
}

static I024_Packet make_i024(uint32_t seq, uint32_t channel_seq,
                             const char* prod = kProd) {
    I024_Packet p{};
    p.header = make_header('D', channel_seq);
    set_prod(p.prod_id, prod);
    p.prod_msg_seq = seq;
    p.calculated_flag = '0';
    return p;
}

static I025_Packet make_i025(uint32_t seq, uint32_t channel_seq,
                             const char* prod = kProd) {
    I025_Packet p{};
    p.header = make_header('E', channel_seq);
    set_prod(p.prod_id, prod);
    p.prod_msg_seq = seq;
    return p;
}

// Feed helper mirroring what TaifexParser does with a manager attached:
// header first, then the typed feed.
template <typename Pkt, typename Fn>
static void feed(OrderBookManager& mgr, const Pkt& pkt, Fn typed) {
    mgr.on_header(pkt.header);
    (mgr.*typed)(pkt);
}
static void feed(OrderBookManager& m, const I024_Packet& p) { feed(m, p, &OrderBookManager::on_i024); }
static void feed(OrderBookManager& m, const I025_Packet& p) { feed(m, p, &OrderBookManager::on_i025); }
static void feed(OrderBookManager& m, const I081_Packet& p) { feed(m, p, &OrderBookManager::on_i081); }
static void feed(OrderBookManager& m, const I083_Packet& p) { feed(m, p, &OrderBookManager::on_i083); }
static void feed(OrderBookManager& m, const I084_Packet& p) { feed(m, p, &OrderBookManager::on_i084); }

struct Recorder {
    std::vector<OrderBook> books;
    OrderBookManager::BookCallback cb() {
        return [this](const OrderBook& b) { books.push_back(b); };
    }
    const OrderBook& last() const { return books.back(); }
};

static bool level_is(const OrderBookLevel& l, uint64_t price, uint32_t qty) {
    return l.valid && l.price == price && l.quantity == qty;
}
static bool level_empty(const OrderBookLevel& l) { return !l.valid; }

// The manual's example baseline book (pp. 89-90): 4 bids / 5 asks.
static std::vector<MDEntry> manual_snapshot_entries() {
    return {
        make_entry(' ', '0', 10315, 4, 1), make_entry(' ', '0', 10314, 3, 2),
        make_entry(' ', '0', 10313, 5, 3), make_entry(' ', '0', 10312, 8, 4),
        make_entry(' ', '1', 10317, 6, 1), make_entry(' ', '1', 10318, 7, 2),
        make_entry(' ', '1', 10319, 13, 3), make_entry(' ', '1', 10320, 15, 4),
        make_entry(' ', '1', 10321, 13, 5),
    };
}

// --- Scenarios --------------------------------------------------------------

static void test_initial_build_from_i083() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));

    CHECK(rec.books.size() == 1);
    const OrderBook& b = rec.last();
    CHECK(!b.is_stale);
    CHECK(b.has_snapshot);
    CHECK(b.last_prod_msg_seq == 100);
    CHECK(level_is(b.bids[0], 10315, 4));
    CHECK(level_is(b.bids[3], 10312, 8));
    CHECK(level_empty(b.bids[4]));
    CHECK(level_is(b.asks[0], 10317, 6));
    CHECK(level_is(b.asks[4], 10321, 13));
    CHECK(std::string(b.prod_id).substr(0, 5) == "TXFG6");
}

// Manual example 三 (p.90): New buy at level 5 appends.
static void test_i081_new_append() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i081(101, {make_entry('0', '0', 10311, 10, 5)}, 2));

    const OrderBook& b = rec.last();
    CHECK(!b.is_stale);
    CHECK(b.last_prod_msg_seq == 101);
    CHECK(level_is(b.bids[4], 10311, 10));
    CHECK(level_is(b.bids[0], 10315, 4));  // untouched
}

// Manual example 四 (p.91): New buy at level 1 shifts everything down; the
// old level 5 falls off silently.
static void test_i081_new_shift_down_falloff() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i081(101, {make_entry('0', '0', 10311, 10, 5)}, 2));
    feed(mgr, make_i081(102, {make_entry('0', '0', 10316, 1, 1)}, 3));

    const OrderBook& b = rec.last();
    CHECK(!b.is_stale);
    CHECK(level_is(b.bids[0], 10316, 1));
    CHECK(level_is(b.bids[1], 10315, 4));
    CHECK(level_is(b.bids[2], 10314, 3));
    CHECK(level_is(b.bids[3], 10313, 5));
    CHECK(level_is(b.bids[4], 10312, 8));  // 10311 fell off
}

// Manual example 五 (pp.92-95): mixed New/Delete batch, applied in order.
static void test_i081_delete_shift_up_batch() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i081(101, {make_entry('0', '0', 10311, 10, 5)}, 2));
    feed(mgr, make_i081(102, {make_entry('0', '0', 10316, 1, 1)}, 3));
    feed(mgr, make_i081(103,
                        {
                            make_entry('0', '0', 10318, 2, 1),   // new best bid
                            make_entry('2', '1', 10317, 0, 1),   // del ask L1
                            make_entry('0', '1', 10322, 5, 5),   // new ask L5
                            make_entry('2', '1', 10318, 0, 1),   // del ask L1 again
                            make_entry('0', '1', 10323, 3, 5),   // new ask L5
                        },
                        4));

    const OrderBook& b = rec.last();
    CHECK(!b.is_stale);
    // Final state from manual p.95:
    CHECK(level_is(b.bids[0], 10318, 2));
    CHECK(level_is(b.bids[1], 10316, 1));
    CHECK(level_is(b.bids[2], 10315, 4));
    CHECK(level_is(b.bids[3], 10314, 3));
    CHECK(level_is(b.bids[4], 10313, 5));
    CHECK(level_is(b.asks[0], 10319, 13));
    CHECK(level_is(b.asks[1], 10320, 15));
    CHECK(level_is(b.asks[2], 10321, 13));
    CHECK(level_is(b.asks[3], 10322, 5));
    CHECK(level_is(b.asks[4], 10323, 3));
}

// Delete on its own must leave the vacated level 5 empty (no ghost quote).
static void test_i081_delete_leaves_level5_empty() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i081(101, {make_entry('2', '1', 10317, 0, 1)}, 2));

    const OrderBook& b = rec.last();
    CHECK(level_is(b.asks[0], 10318, 7));
    CHECK(level_is(b.asks[3], 10321, 13));
    CHECK(level_empty(b.asks[4]));  // no ghost duplicate of the old level 5

    // Delete at level 5 directly.
    feed(mgr, make_i081(102, {make_entry('2', '0', 10312, 0, 4)}, 3));
    CHECK(level_empty(rec.last().bids[3]));
}

// Manual example 六 (p.96): Change rewrites a level in place.
static void test_i081_change_in_place() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i081(101, {make_entry('1', '1', 10319, 10, 3)}, 2));

    const OrderBook& b = rec.last();
    CHECK(level_is(b.asks[2], 10319, 10));
    CHECK(level_is(b.asks[1], 10318, 7));  // neighbours untouched
    CHECK(level_is(b.asks[3], 10320, 15));
}

// Manual examples 七/八 (pp.97-99): Overlay writes derived levels in place;
// price=0/qty=0 clears the level.
static void test_i081_overlay_derived() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i081(101,
                        {make_entry('5', 'E', 10317, 5, 1),
                         make_entry('5', 'F', 10320, 5, 1)},
                        2));

    const OrderBook& b1 = rec.last();
    CHECK(level_is(b1.derived_bids[0], 10317, 5));
    CHECK(level_is(b1.derived_asks[0], 10320, 5));
    CHECK(level_is(b1.bids[0], 10315, 4));  // regular ladders untouched

    feed(mgr, make_i081(102, {make_entry('5', 'E', 0, 0, 1)}, 3));
    const OrderBook& b2 = rec.last();
    CHECK(level_empty(b2.derived_bids[0]));
    CHECK(level_is(b2.derived_asks[0], 10320, 5));
}

// I024/I025 consume the same per-product serial; interleaving them must not
// look like a gap.
static void test_i024_i025_interleave_no_false_stale() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i024(101, 2));
    feed(mgr, make_i025(102, 3));
    feed(mgr, make_i081(103, {make_entry('1', '1', 10319, 9, 3)}, 4));

    CHECK(rec.books.size() == 2);  // I024/I025 change nothing -> no callback
    const OrderBook& b = rec.last();
    CHECK(!b.is_stale);
    CHECK(b.last_prod_msg_seq == 103);
    CHECK(level_is(b.asks[2], 10319, 9));
}

static void test_gap_marks_stale_and_applies_best_effort() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    // seq 101 lost (channel_seq 2 lost as well)
    feed(mgr, make_i081(102, {make_entry('1', '1', 10319, 8, 3)}, 3));

    const OrderBook& b = rec.last();
    CHECK(b.is_stale);
    CHECK(b.last_prod_msg_seq == 102);
    CHECK(level_is(b.asks[2], 10319, 8));  // still applied best-effort

    // Still stale on the next contiguous increment (chain already broken).
    feed(mgr, make_i081(103, {make_entry('1', '1', 10319, 7, 3)}, 4));
    CHECK(rec.last().is_stale);
    CHECK(level_is(rec.last().asks[2], 10319, 7));
}

static void test_i084_recovery_then_fresh() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i081(102, {make_entry('1', '1', 10319, 8, 3)}, 3));  // gap
    CHECK(rec.last().is_stale);

    feed(mgr, make_i084_o(105, {make_entry(' ', '0', 10315, 2, 1),
                                make_entry(' ', '1', 10317, 3, 1)},
                          1));
    const OrderBook& b = rec.last();
    CHECK(!b.is_stale);
    CHECK(b.has_snapshot);
    CHECK(b.last_prod_msg_seq == 105);
    CHECK(level_is(b.bids[0], 10315, 2));
    CHECK(level_empty(b.bids[1]));  // adopted wholesale
    CHECK(level_is(b.asks[0], 10317, 3));

    // Contiguous increment after adoption stays fresh.
    feed(mgr, make_i081(106, {make_entry('0', '0', 10314, 1, 2)}, 4));
    CHECK(!rec.last().is_stale);
    CHECK(level_is(rec.last().bids[1], 10314, 1));
}

// An I084 'O' packet normally carries several products; each block must be
// adopted (or ignored) independently.
static void test_i084_multi_product() {
    OrderBookManager mgr;
    Recorder recA, recB, recC;
    mgr.register_callback("AAA", recA.cb());
    mgr.register_callback("BBB", recB.cb());
    mgr.register_callback("CCC", recC.cb());
    // BBB already live and ahead of the snapshot: its block must be ignored.
    feed(mgr, make_i083(50, {make_entry(' ', '0', 200, 2, 1)}, 1, '0', "BBB"));

    feed(mgr, make_i084_o_multi(
                  {{"AAA", 10, {make_entry(' ', '0', 100, 1, 1)}},
                   {"BBB", 40, {make_entry(' ', '0', 999, 9, 1)}},
                   {"CCC", 30, {make_entry(' ', '1', 300, 3, 1)}}},
                  1));

    CHECK(recA.books.size() == 1);
    CHECK(!recA.last().is_stale);
    CHECK(level_is(recA.last().bids[0], 100, 1));
    CHECK(recB.books.size() == 1);  // no delivery for the stale-ignored block
    CHECK(level_is(mgr.get_book("BBB")->bids[0], 200, 2));
    CHECK(mgr.get_book("BBB")->last_prod_msg_seq == 50);
    CHECK(recC.books.size() == 1);
    CHECK(level_is(recC.last().asks[0], 300, 3));
    CHECK(mgr.product_ids().size() == 3);
}

// Corrupt/hostile MD entries (price_level 0 or >5, unknown entry_type or
// update_action) must be skipped without touching the book or crashing.
static void test_malformed_entries_ignored() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    const OrderBook before = rec.last();

    feed(mgr, make_i081(101,
                        {
                            make_entry('0', '0', 1, 1, 0),   // level 0
                            make_entry('0', '0', 2, 2, 6),   // level > depth
                            make_entry('0', 'X', 3, 3, 1),   // unknown side
                            make_entry('9', '0', 4, 4, 1),   // unknown action
                        },
                        2));
    const OrderBook& after = rec.last();
    CHECK(!after.is_stale);  // sequence still advanced normally
    CHECK(after.last_prod_msg_seq == 101);
    for (int i = 0; i < TAIFEX_BOOK_DEPTH; ++i) {
        CHECK(after.bids[i].valid == before.bids[i].valid);
        CHECK(after.bids[i].price == before.bids[i].price);
        CHECK(after.asks[i].valid == before.asks[i].valid);
        CHECK(after.asks[i].price == before.asks[i].price);
    }

    // Same garbage inside a snapshot: adoption skips it, keeps the rest.
    feed(mgr, make_i083(102,
                        {make_entry(' ', '0', 500, 5, 1),
                         make_entry(' ', '1', 600, 6, 9),   // level > depth
                         make_entry(' ', 'Z', 700, 7, 1)},  // unknown side
                        3));
    const OrderBook& b = rec.last();
    CHECK(level_is(b.bids[0], 500, 5));
    for (int i = 0; i < TAIFEX_BOOK_DEPTH; ++i) CHECK(level_empty(b.asks[i]));
}

static void test_i083_recovery_from_stale() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i081(102, {make_entry('1', '1', 10319, 8, 3)}, 3));  // gap
    CHECK(rec.last().is_stale);
    feed(mgr, make_i083(110, {make_entry(' ', '0', 10300, 1, 1)}, 4));
    CHECK(!rec.last().is_stale);
    CHECK(rec.last().last_prod_msg_seq == 110);
    CHECK(level_is(rec.last().bids[0], 10300, 1));
}

// An I083 whose seq equals the stale book's last seq (quiet product since the
// loss — the common recovery case) must still be adopted.
static void test_i083_equal_seq_adoption_while_stale() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i081(102, {make_entry('1', '1', 10319, 8, 3)}, 3));  // gap
    CHECK(rec.last().is_stale);
    feed(mgr, make_i083(102, {make_entry(' ', '0', 10301, 2, 1)}, 4));
    CHECK(!rec.last().is_stale);
    CHECK(rec.last().last_prod_msg_seq == 102);
    CHECK(level_is(rec.last().bids[0], 10301, 2));
}

static void test_noncontiguous_post_snapshot_stale_again() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i084_o(105, {make_entry(' ', '0', 10315, 2, 1)}, 1));
    CHECK(!rec.last().is_stale);
    // First post-snapshot increment skips 106 -> the adoption race hit us.
    feed(mgr, make_i081(107, {make_entry('1', '0', 10315, 1, 1)}, 10));
    CHECK(rec.last().is_stale);
}

static void test_increment_at_or_below_snapshot_seq_dropped() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i084_o(105, {make_entry(' ', '0', 10315, 2, 1)}, 1));
    // Covered by the snapshot: must be dropped, book unchanged.
    feed(mgr, make_i081(104, {make_entry('0', '0', 10399, 9, 1)}, 10));
    feed(mgr, make_i081(105, {make_entry('0', '0', 10399, 9, 1)}, 11));
    CHECK(rec.books.size() == 1);
    auto b = mgr.get_book(kProd);
    CHECK(b && !b->is_stale);
    CHECK(level_is(b->bids[0], 10315, 2));
}

static void test_trial_i083_tracks_seq_but_keeps_book() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i083(101, {make_entry(' ', '0', 999999999, 50, 1)}, 2, '1'));

    CHECK(rec.books.size() == 1);  // trial snapshot: no delivery
    auto b = mgr.get_book(kProd);
    CHECK(b && !b->is_stale);
    CHECK(b->last_prod_msg_seq == 101);       // seq consumed
    CHECK(level_is(b->bids[0], 10315, 4));    // content untouched

    // Chain continues from the trial's seq.
    feed(mgr, make_i081(102, {make_entry('1', '0', 10315, 6, 1)}, 3));
    CHECK(!rec.last().is_stale);
}

static void test_older_snapshot_ignored() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    feed(mgr, make_i081(101, {make_entry('1', '1', 10319, 9, 3)}, 2));
    // Stale carousel data from before the increment: ignore both kinds.
    feed(mgr, make_i084_o(99, {make_entry(' ', '0', 1, 1, 1)}, 1));
    feed(mgr, make_i083(100, {make_entry(' ', '0', 2, 2, 1)}, 3));
    CHECK(rec.books.size() == 2);
    auto b = mgr.get_book(kProd);
    CHECK(b && b->last_prod_msg_seq == 101);
    CHECK(level_is(b->asks[2], 10319, 9));
}

static void test_empty_snapshot_is_valid_empty_book() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i084_o(50, {}, 1));
    const OrderBook& b = rec.last();
    CHECK(!b.is_stale);
    CHECK(b.has_snapshot);
    for (int i = 0; i < TAIFEX_BOOK_DEPTH; ++i) {
        CHECK(level_empty(b.bids[i]));
        CHECK(level_empty(b.asks[i]));
    }
}

static void test_first_i081_without_snapshot_is_stale() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    // Joined mid-session: first thing seen is an increment with seq 500.
    feed(mgr, make_i081(500, {make_entry('0', '0', 10315, 4, 1)}, 100));
    const OrderBook& b = rec.last();
    CHECK(b.is_stale);
    CHECK(!b.has_snapshot);
    CHECK(level_is(b.bids[0], 10315, 4));  // best-effort content
}

static void test_seq_one_from_session_start_is_fresh() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    // Listening from session start: seq 1 proves a complete chain from an
    // empty book even without a snapshot.
    feed(mgr, make_i081(1, {make_entry('0', '0', 10315, 4, 1)}, 1));
    CHECK(!rec.last().is_stale);
    feed(mgr, make_i081(2, {make_entry('0', '1', 10317, 2, 1)}, 2));
    CHECK(!rec.last().is_stale);
}

static void test_channel_gap_suspect_then_contiguity_clears() {
    OrderBookManager mgr;
    Recorder recA, recB;
    mgr.register_callback("AAA", recA.cb());
    mgr.register_callback("BBB", recB.cb());
    feed(mgr, make_i083(10, {make_entry(' ', '0', 100, 1, 1)}, 1, '0', "AAA"));
    feed(mgr, make_i083(20, {make_entry(' ', '0', 200, 2, 1)}, 2, '0', "BBB"));
    CHECK(!recA.last().is_stale);
    CHECK(!recB.last().is_stale);

    // channel_seq jumps 2 -> 5: two messages lost, could concern anyone.
    feed(mgr, make_i024(11, 5, "AAA"));
    // Header gap fires stale flips for both registered products...
    CHECK(recA.books.size() >= 2 && recB.books.size() == 2);
    CHECK(recB.last().is_stale);
    // ...but AAA's own message was contiguous (10 -> 11): proof it was
    // unaffected, so its final delivered state this round is fresh again.
    CHECK(!recA.last().is_stale);
    CHECK(mgr.get_book("AAA")->is_stale == false);
    CHECK(mgr.get_book("BBB")->is_stale == true);

    // BBB's next message is contiguous too (20 -> 21): un-stale without any
    // snapshot.
    feed(mgr, make_i025(21, 6, "BBB"));
    CHECK(!recB.last().is_stale);
    CHECK(mgr.get_book("BBB")->is_stale == false);
}

static void test_channel_gap_with_product_loss_needs_snapshot() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(10, {make_entry(' ', '0', 100, 1, 1)}, 1));
    // Lost packet contained this product's seq 11:
    feed(mgr, make_i081(12, {make_entry('1', '0', 100, 5, 1)}, 3));
    CHECK(rec.last().is_stale);
    // Contiguous follow-ups do NOT clear a broken chain...
    feed(mgr, make_i081(13, {make_entry('1', '0', 100, 6, 1)}, 4));
    CHECK(rec.last().is_stale);
    // ...only a snapshot does.
    feed(mgr, make_i084_o(13, {make_entry(' ', '0', 100, 6, 1)}, 1));
    CHECK(!rec.last().is_stale);
}

static void test_heartbeat_reveals_gap_on_idle_channel() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(10, {make_entry(' ', '0', 100, 1, 1)}, 7));
    CHECK(!rec.last().is_stale);
    // Heartbeat repeating the last seq: fine.
    mgr.on_header(make_header('1', 7));
    CHECK(!mgr.get_book(kProd)->is_stale);
    // Heartbeat claiming a later seq: something was lost while idle.
    mgr.on_header(make_header('1', 9));
    CHECK(mgr.get_book(kProd)->is_stale);
    CHECK(rec.last().is_stale);  // flip was delivered
}

static void test_snapshot_channel_gap_ignored() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(10, {make_entry(' ', '0', 100, 1, 1)}, 1));
    // I084 carousel resets its CHANNEL-SEQ each cycle; jumps there are noise.
    feed(mgr, make_i084_o(10, {make_entry(' ', '0', 100, 1, 1)}, 500));
    feed(mgr, make_i084_o(10, {make_entry(' ', '0', 100, 1, 1)}, 1));
    feed(mgr, make_i084_o(10, {make_entry(' ', '0', 100, 1, 1)}, 900));
    CHECK(!mgr.get_book(kProd)->is_stale);
}

static void test_i002_reset_clears_everything() {
    OrderBookManager mgr;
    Recorder rec;
    mgr.register_callback(kProd, rec.cb());
    feed(mgr, make_i083(100, manual_snapshot_entries(), 1));
    CHECK(rec.books.size() == 1);
    CHECK(!rec.last().is_stale);
    mgr.on_header(make_header('2', 0));
    // The reset invalidates the book: the subscriber must be told, not left
    // holding a fresh-looking copy.
    CHECK(rec.books.size() == 2);
    CHECK(rec.last().is_stale);
    CHECK(!mgr.get_book(kProd).has_value());
    CHECK(mgr.product_ids().empty());
    // A second reset has nothing to deliver.
    mgr.on_header(make_header('2', 0));
    CHECK(rec.books.size() == 2);
    // Rebuild from scratch: serials restart at 1.
    feed(mgr, make_i081(1, {make_entry('0', '0', 10315, 4, 1)}, 1));
    CHECK(!rec.last().is_stale);
}

static void test_callback_routing() {
    OrderBookManager mgr;
    Recorder recA, recA2, recB, recAll;
    mgr.register_callback("TXFG6", recA.cb());       // trimmed form
    mgr.register_callback("TXFG6   ", recA2.cb());   // padded form
    mgr.register_callback("MXFG6", recB.cb());
    mgr.register_callback_all(recAll.cb());

    // All callbacks of one delivery event must receive the SAME snapshot
    // (single copy, passed by reference, valid only during the call).
    std::vector<const OrderBook*> addresses;
    mgr.register_callback("TXFG6", [&](const OrderBook& b) {
        addresses.push_back(&b);
    });
    mgr.register_callback_all([&](const OrderBook& b) {
        addresses.push_back(&b);
    });

    feed(mgr, make_i083(1, {make_entry(' ', '0', 100, 1, 1)}, 1, '0', "TXFG6"));
    CHECK(recA.books.size() == 1);
    CHECK(recA2.books.size() == 1);  // both callbacks for the product fire
    CHECK(recB.books.empty());
    CHECK(recAll.books.size() == 1);
    CHECK(addresses.size() == 2);
    CHECK(addresses[0] == addresses[1]);  // one shared snapshot per event

    feed(mgr, make_i083(1, {make_entry(' ', '0', 200, 1, 1)}, 2, '0', "MXFG6"));
    CHECK(recB.books.size() == 1);
    CHECK(recAll.books.size() == 2);

    mgr.unregister_callbacks("TXFG6");
    feed(mgr, make_i081(2, {make_entry('1', '0', 100, 2, 1)}, 3, "TXFG6"));
    CHECK(recA.books.size() == 1);   // no longer delivered
    CHECK(recAll.books.size() == 3); // wildcard unaffected

    CHECK(!mgr.get_book("NOSUCH").has_value());
    CHECK(mgr.product_ids().size() == 2);
}

static void test_reentrant_callback() {
    OrderBookManager mgr;
    int calls = 0;
    mgr.register_callback(kProd, [&](const OrderBook& b) {
        ++calls;
        // Re-entering the manager from a callback must not deadlock.
        auto again = mgr.get_book(kProd);
        CHECK(again.has_value());
        mgr.register_callback("OTHER", [](const OrderBook&) {});
    });
    feed(mgr, make_i083(1, {make_entry(' ', '0', 100, 1, 1)}, 1));
    CHECK(calls == 1);
}

static void test_two_thread_feed_smoke() {
    OrderBookManager mgr;
    std::atomic<int> deliveries{0};
    mgr.register_callback_all([&](const OrderBook&) { ++deliveries; });

    std::thread rt([&] {
        uint32_t chan_seq = 1;
        feed(mgr, make_i083(1, manual_snapshot_entries(), chan_seq++));
        for (uint32_t s = 2; s <= 5000; ++s)
            feed(mgr, make_i081(s, {make_entry('1', '0', 10315, s % 100 + 1, 1)},
                                chan_seq++));
    });
    std::thread snap([&] {
        for (int i = 0; i < 500; ++i)
            feed(mgr, make_i084_o(1 + i * 10,
                                  {make_entry(' ', '0', 10315, 1, 1)}, 1));
    });
    rt.join();
    snap.join();

    auto b = mgr.get_book(kProd);
    CHECK(b.has_value());
    CHECK(b->last_prod_msg_seq >= 5000 - 1);
    CHECK(deliveries.load() > 0);
}

int main() {
    RUN(test_initial_build_from_i083);
    RUN(test_i081_new_append);
    RUN(test_i081_new_shift_down_falloff);
    RUN(test_i081_delete_shift_up_batch);
    RUN(test_i081_delete_leaves_level5_empty);
    RUN(test_i081_change_in_place);
    RUN(test_i081_overlay_derived);
    RUN(test_i024_i025_interleave_no_false_stale);
    RUN(test_gap_marks_stale_and_applies_best_effort);
    RUN(test_i084_recovery_then_fresh);
    RUN(test_i084_multi_product);
    RUN(test_malformed_entries_ignored);
    RUN(test_i083_recovery_from_stale);
    RUN(test_i083_equal_seq_adoption_while_stale);
    RUN(test_noncontiguous_post_snapshot_stale_again);
    RUN(test_increment_at_or_below_snapshot_seq_dropped);
    RUN(test_trial_i083_tracks_seq_but_keeps_book);
    RUN(test_older_snapshot_ignored);
    RUN(test_empty_snapshot_is_valid_empty_book);
    RUN(test_first_i081_without_snapshot_is_stale);
    RUN(test_seq_one_from_session_start_is_fresh);
    RUN(test_channel_gap_suspect_then_contiguity_clears);
    RUN(test_channel_gap_with_product_loss_needs_snapshot);
    RUN(test_heartbeat_reveals_gap_on_idle_channel);
    RUN(test_snapshot_channel_gap_ignored);
    RUN(test_i002_reset_clears_everything);
    RUN(test_callback_routing);
    RUN(test_reentrant_callback);
    RUN(test_two_thread_feed_smoke);

    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nAll tests passed.\n");
    return 0;
}
