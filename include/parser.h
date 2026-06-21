#ifndef TAIFEX_PARSER_H
#define TAIFEX_PARSER_H

#include <thread>
#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include "logger.h"

// --- 1. Common Structures ---

struct Header {
    char transmission_code;  // X(1)
    char message_kind;       // X(1) - 'A' for I081, 'B' for I083
    char info_time[16];   // 9(12) - BCD 6 bytes (HHMMSSuuuuuu)
    uint16_t channel_id;     // 9(4)  - BCD 2 bytes
    uint32_t channel_seq;    // 9(10) - BCD 5 bytes
    uint8_t version_no;      // 9(2)  - BCD 1 byte
    uint16_t body_len;       // 9(4)  - BCD 2 bytes
};

struct Footer {
    uint8_t checksum;       // X(1) - XOR checksum of the entire packet
    uint16_t terminal_code; // X(2) - Fixed 0x0D0A (\r\n)
};

struct MDEntry {
    // Note: I083 entries might not use update_action, but shared for memory efficiency
    char update_action;      // X(1) - 0:New, 1:Change, 2:Delete, 5:Overlay
    char entry_type;         // X(1) - 0:Buy, 1:Sell, E:Derived Buy, F:Derived Sell
    char price_sign;         // X(1) - '0':Positive, '-':Negative
    uint64_t price;          // 9(9)  - BCD 5 bytes
    uint32_t quantity;       // 9(8)  - BCD 4 bytes
    uint8_t price_level;     // 9(2)  - BCD 1 byte
    uint8_t decimal_locator; // documented decimal point (Note:     I081 and I084 are 3)
};

struct MatchData {
    char price_sign;         // X(1) - '0':Positive, '-':Negative
    uint64_t price;          // 9(9) - BCD 5 bytes
    uint16_t quantity;       // 9(4) - BCD 2 bytes (Note: I024 repeat qty is 2 bytes)
    uint8_t decimal_locator; // documented decimal point (Note: I024 is 2)
};

// --- 2. Specific Message Bodies ---

struct I024_Packet {
    Header header;           // Common Header

    // --- Body Fixed Part ---
    char prod_id[21];        // X(20)
    uint32_t prod_msg_seq;   // 9(10) - BCD 5 bytes
    char calculated_flag;    // X(1)  - 0:Normal, 1:Calculated
    char match_time[16];  // 9(12) - BCD 6 bytes (HHMMSSuuuuuu)
    
    // First Match (Always present in I024)
    char first_price_sign;   // X(1)
    uint64_t first_price;    // 9(9) - BCD 5 bytes
    uint32_t first_quantity; // 9(8) - BCD 4 bytes
    uint8_t first_price_decimal = 2;

    // --- Dynamic Control ---
    uint8_t display_item;    // X(1) - Bit Map (Bit 0-6 defines number of repeats)
    
    // --- Repeated Match Data ---
    std::vector<MatchData> consecutive_matches;

    // --- Aggregate Statistics (Trailing Part) ---
    uint32_t total_qty;      // 9(8) - BCD 4 bytes
    uint32_t buy_cnt;        // 9(8) - BCD 4 bytes
    uint32_t sell_cnt;       // 9(8) - BCD 4 bytes

    Footer footer;           // Checksum and Terminal Code
};

// I081: Incremental Order Book Update
struct I081_Packet {
    Header header;
    char prod_id[21];        // X(20)
    uint32_t prod_msg_seq;   // 9(10) - BCD 5 bytes
    uint8_t no_md_entries;   // 9(2)  - BCD 1 byte
    std::vector<MDEntry> entries;
    Footer footer;
};

// I083: Order Book Snapshot
struct I083_Packet {
    Header header;
    char prod_id[21];        // X(20)
    uint32_t prod_msg_seq;   // 9(10) - BCD 5 bytes
    char calculated_flag;    // X(1)  - 0:Normal, 1:Calculated (Missing in I081)
    uint8_t no_md_entries;   // 9(2)  - BCD 1 byte
    std::vector<MDEntry> entries;
    Footer footer;
};

// I084 'O' (Order Data): one product block, repeated NO-ENTRIES times.
struct I084Product {
    char prod_id[21];           // X(20)
    uint32_t last_prod_msg_seq; // 9(10) - BCD 5 bytes (商品行情訊息末筆處理序號)
    uint8_t no_md_entries;      // 9(2)  - BCD 1 byte (0 => empty book)
    std::vector<MDEntry> entries;
};

// I084: Snapshot Refresh (快照更新訊息), message_kind 'C'. The 1-byte
// MESSAGE-TYPE selects the body: 'A'/'Z' carry LAST-SEQ, 'O' carries the
// repeated per-product order book. 'S'/'P' are left unparsed (out of scope).
struct I084_Packet {
    Header header;
    char message_type;          // X(1) - 'A','O','S','P','Z'
    uint32_t last_seq;          // 9(10) - BCD 5 bytes ('A'/'Z'; 0 otherwise)
    uint8_t no_entries;         // 9(2)  - BCD 1 byte ('O' product count; 0 otherwise)
    std::vector<I084Product> products; // 'O' only
    Footer footer;
};

// --- 3. Parser Class ---

class TaifexParser {
public:
    TaifexParser() ;
    ~TaifexParser() ;

    // Use separate callbacks for different message types
    using I024Callback = std::function<void(const I024_Packet&)>;
    using I081Callback = std::function<void(const I081_Packet&)>;
    using I083Callback = std::function<void(const I083_Packet&)>;
    using I084Callback = std::function<void(const I084_Packet&)>;

    void start_loop(int port, I024Callback cb024, I081Callback cb81, I083Callback cb83, I084Callback cb84) ;
    void end_loop() ;
    void set_multicast(const std::string& group, const std::string& iface_ip);

private:
    void receive_loop(int port) ;
    void process_raw_data(const uint8_t* buffer, size_t length);

    // Parsing logic separated to handle the CALCULATED-FLAG offset
    bool parse_header(const uint8_t* data, Header& header);
    bool handle_i024(const uint8_t* data, const Header& header);
    bool handle_i081(const uint8_t* data, const Header& header);
    bool handle_i083(const uint8_t* data, const Header& header);
    bool handle_i084(const uint8_t* data, const Header& header);

    // Utility
    uint64_t bcd_to_uint(const uint8_t* bcd, size_t len);
    void format_bcd_time_to_char(const uint8_t* bcd, char* out_buf, bool has_micro);
    bool verify_checksum(const uint8_t* data, size_t len);

    std::thread recv_thread;
    std::atomic<bool> running;
    I024Callback on_i024;
    I081Callback on_i081;
    I083Callback on_i083;
    I084Callback on_i084;

    int sockfd = -1;
    bool use_multicast = false;
    std::string multicast_group;
    std::string interface_ip;

    static constexpr uint8_t  ESC_CODE = 0x1B;
    static constexpr size_t   HEADER_SIZE = 18; // Without ESC
};

#endif