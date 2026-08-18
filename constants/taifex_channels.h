#ifndef TAIFEX_CONSTANTS_CHANNELS_H
#define TAIFEX_CONSTANTS_CHANNELS_H

#include <cstdint>
#include <string_view>

// TAIFEX multicast channel table, transcribed VERBATIM from the exchange
// manual 逐筆行情資訊傳輸作業手冊 V0.9.5 (2019-09-21), 叁/三 「Multicast
// 群組定義」, printed pages 7-8.
//
// Structural facts that shape this table:
//  - CHANNEL-ID and port are IDENTICAL across the day (一般交易時段) and
//    night (盤後交易時段) sessions; only the group differs (225.0.x day,
//    225.10.x night). No in-band header field distinguishes the sessions —
//    binding the socket to the group address is the only separation.
//  - The address gaps are DELIBERATE: .145/port 14500 and .45/port 4500 are
//    skipped (channels 11-14 were renumbered in manual V0.9.5). Never derive
//    an endpoint arithmetically; use these literal rows.
//  - There is no A/B redundant group pair. The exchange's "Dual Feed" is two
//    telecom circuits carrying the SAME groups; duplicates are filtered by
//    CHANNEL-SEQ. The 主/備 split exists only for the TCP retransmission
//    servers (manual printed p.8), which this library does not implement.
//
// verified == true means the row is corroborated by live production captures
// in addition to the manual (channels 1/2/13/14, the ones this library
// decodes). The remaining services come from the manual only.

namespace taifex {
namespace constants {

enum class Session : uint8_t { Day, Night };        // 一般交易時段 / 盤後交易時段
enum class Product : uint8_t { Futures, Options };  // 期貨 / 選擇權
enum class Service : uint8_t {
    Realtime,         // 即時行情 (I024/I025/I081/I083, +I140)
    BasicData,        // 基本資料 (I010/I011/I120/I130)
    TextBulletin,     // 文字公告訊息 (I050)
    Statistics,       // 統計資訊 (I030/I070/I071/I072/I073)
    Quote,            // 詢價訊息 (I100)
    SpotData,         // 現貨資訊 (I060/I064)
    SnapshotRefresh,  // 行情快照更新 (I084)
};

struct Channel {
    Session          session;
    Product          product;
    Service          service;
    uint16_t         channel_id;  // NOT unique across sessions (see above)
    std::string_view group;       // multicast group, dotted quad
    uint16_t         port;        // UDP destination port
    bool             verified;    // corroborated by live captures, not just the manual
};

inline constexpr Channel kChannels[] = {
    // --- 一般交易時段 (Day) ---
    {Session::Day, Product::Futures, Service::Realtime,         1, "225.0.140.140", 14000, true},
    {Session::Day, Product::Options, Service::Realtime,         2, "225.0.40.40",    4000, true},
    {Session::Day, Product::Futures, Service::BasicData,        3, "225.0.140.141", 14100, false},
    {Session::Day, Product::Options, Service::BasicData,        4, "225.0.40.41",    4100, false},
    {Session::Day, Product::Futures, Service::TextBulletin,     5, "225.0.140.142", 14200, false},
    {Session::Day, Product::Options, Service::TextBulletin,     6, "225.0.40.42",    4200, false},
    {Session::Day, Product::Futures, Service::Statistics,       7, "225.0.140.143", 14300, false},
    {Session::Day, Product::Options, Service::Statistics,       8, "225.0.40.43",    4300, false},
    {Session::Day, Product::Futures, Service::Quote,            9, "225.0.140.144", 14400, false},
    {Session::Day, Product::Options, Service::Quote,           10, "225.0.40.44",    4400, false},
    {Session::Day, Product::Futures, Service::SpotData,        11, "225.0.140.146", 14600, false},
    {Session::Day, Product::Options, Service::SpotData,        12, "225.0.40.46",    4600, false},
    {Session::Day, Product::Futures, Service::SnapshotRefresh, 13, "225.0.140.147", 14700, true},
    {Session::Day, Product::Options, Service::SnapshotRefresh, 14, "225.0.40.47",    4700, true},
    // --- 盤後交易時段 (Night) ---
    {Session::Night, Product::Futures, Service::Realtime,         1, "225.10.140.140", 14000, true},
    {Session::Night, Product::Options, Service::Realtime,         2, "225.10.40.40",    4000, true},
    {Session::Night, Product::Futures, Service::BasicData,        3, "225.10.140.141", 14100, false},
    {Session::Night, Product::Options, Service::BasicData,        4, "225.10.40.41",    4100, false},
    {Session::Night, Product::Futures, Service::TextBulletin,     5, "225.10.140.142", 14200, false},
    {Session::Night, Product::Options, Service::TextBulletin,     6, "225.10.40.42",    4200, false},
    {Session::Night, Product::Futures, Service::Statistics,       7, "225.10.140.143", 14300, false},
    {Session::Night, Product::Options, Service::Statistics,       8, "225.10.40.43",    4300, false},
    {Session::Night, Product::Futures, Service::Quote,            9, "225.10.140.144", 14400, false},
    {Session::Night, Product::Options, Service::Quote,           10, "225.10.40.44",    4400, false},
    {Session::Night, Product::Futures, Service::SpotData,        11, "225.10.140.146", 14600, false},
    {Session::Night, Product::Options, Service::SpotData,        12, "225.10.40.46",    4600, false},
    {Session::Night, Product::Futures, Service::SnapshotRefresh, 13, "225.10.140.147", 14700, true},
    {Session::Night, Product::Options, Service::SnapshotRefresh, 14, "225.10.40.47",    4700, true},
};

// Compile-time lookup; nullptr for a combination the manual does not define.
constexpr const Channel* find(Session session, Product product, Service service) {
    for (const Channel& ch : kChannels) {
        if (ch.session == session && ch.product == product && ch.service == service)
            return &ch;
    }
    return nullptr;
}

static_assert(find(Session::Day, Product::Futures, Service::Realtime)->port == 14000);
static_assert(find(Session::Night, Product::Options, Service::SnapshotRefresh)->channel_id == 14);

}  // namespace constants
}  // namespace taifex

#endif
