#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <sstream>
#include "parser.h"
#include "taifex_channels.h"
#include "order_book.h"
#include "logger.h"

// Atomic flag to control the main loop and handle graceful shutdown
std::atomic<bool> keep_running(true);

void signal_handler(int signal) {
    keep_running = false;
}

auto get_full_price = [](char sign, uint64_t price) {
    std::string s = (sign == '-' ? "-" : "");
    return s + std::to_string(price);
};

std::string to_hex_str(uint32_t val) {
    std::stringstream ss;
    ss << "0x" << std::hex << std::uppercase << val;
    return ss.str();
}

// --- TAIFEX Callback Handlers ---

void on_trade_match(const I024_Packet& pkt) {
    std::stringstream ss;

    ss << "Received Packet:\n"
       << "----------------------------------------\n"
       << "Transmission Code  : " << pkt.header.transmission_code << "\n"
       << "Message Kind       : " << pkt.header.message_kind << " (I024 - Trade)\n"
       << "Information Time   : " << pkt.header.info_time << "\n"
       << "Channel ID         : " << pkt.header.channel_id << "\n"
       << "Channel Seq        : " << pkt.header.channel_seq << "\n"
       << "----------------------------------------\n"
       << "Prod ID            : " << pkt.prod_id << "\n"
       << "Prod Msg Seq       : " << pkt.prod_msg_seq << "\n"
       << "Calculated Flag    : " << pkt.calculated_flag << " (" << (pkt.calculated_flag == '1' ? "Trial" : "Actual") << ")\n"
       << "Match Time         : " << pkt.match_time << "\n"
       << "Display Item (Raw) : " << (int)pkt.display_item << " (0x" << std::hex << (int)pkt.display_item << std::dec << ")\n"
       << "Cumulative Volume  : " << pkt.total_qty << "\n"
       << "Cumulative Buy Cnt : " << pkt.buy_cnt << "\n"
       << "Cumulative Sell Cnt: " << pkt.sell_cnt << "\n";
    
    Logger::getInstance().log(ss.str());

    // --- Analyzed Packet Section ---
    std::stringstream as;
    as << "=== Analyzed Packet ===\n";
    

    int multi_match_count = pkt.display_item & 0x7F;
    bool is_first_packet = (pkt.display_item & 0x80) != 0;

    as << "Is First Packet    : " << (is_first_packet ? "Yes" : "No") << "\n"
       << "Extra Match Levels : " << multi_match_count << " levels\n";


    as << "First Deal: Price = " << get_full_price(pkt.first_price_sign, pkt.first_price)
       << ", Quantity = " << pkt.first_quantity << "\n";


    if (multi_match_count > 0) {
        for (const auto& m : pkt.consecutive_matches) {
            as << "Next Deal: " 
               << get_full_price(m.price_sign, m.price)
               << " Qty: " << m.quantity << "\n";
        }
    }

    as << "Trial Match Note   : " << (pkt.calculated_flag == '1' ? "Trial (Remaining Order Book)" : "Actual Match") << "\n";
    as << "========================";

    Logger::getInstance().log(as.str());
}

void on_day_high_low(const I025_Packet& pkt) {
    std::stringstream ss;
    ss << "Received Packet (I025 - Day High/Low):\n"
       << "----------------------------------------\n"
       << "Information Time   : " << pkt.header.info_time << "\n"
       << "Channel Seq        : " << pkt.header.channel_seq << "\n"
       << "Prod ID            : " << pkt.prod_id << "\n"
       << "Prod Msg Seq       : " << pkt.prod_msg_seq << "\n"
       << "Day High           : " << get_full_price(pkt.day_high_price_sign, pkt.day_high_price) << "\n"
       << "Day Low            : " << get_full_price(pkt.day_low_price_sign, pkt.day_low_price) << "\n"
       << "Show Time          : " << pkt.show_time << "\n";
    Logger::getInstance().log(ss.str());
}

void on_incremental(const I081_Packet& pkt) {
    std::stringstream ss;
    ss << "Received Packet (I081 - Incremental):\n"
       << "----------------------------------------\n"
       << "Transmission Code  : " << pkt.header.transmission_code << "\n"
       << "Message Kind       : " << pkt.header.message_kind << " (I081 - Increment)\n"
       << "Information Time   : " << pkt.header.info_time << "\n"
       << "Channel ID         : " << pkt.header.channel_id << "\n"
       << "Channel Seq        : " << pkt.header.channel_seq << "\n"
       << "----------------------------------------\n"
       << "Prod ID            : " << pkt.prod_id << "\n"
       << "Prod Msg Seq       : " << pkt.prod_msg_seq << "\n"
       << "No. MD Entries     : " << (int)pkt.no_md_entries << "\n";

    Logger::getInstance().log(ss.str());

    std::stringstream as;
    as << "=== Analyzed Incremental Update ===\n";

       for (const auto& entry : pkt.entries) {
        std::string act;
        switch(entry.update_action) {
            case '0': act = "NEW"; break;
            case '1': act = "CHG"; break;
            case '2': act = "DEL"; break;
            case '5': act = "Ove"; break;
            default:  act = "UNK";
        }

        std::string side;
        switch(entry.entry_type) {
            case '0': side = "BID"; break;
            case '1': side = "ASK"; break;
            case 'E': side = "Implied BID"; break;
            case 'F': side = "Implied ASK"; break;
            default:  side = "Unknown";
        }

        as << "[" << act << "][" << side << "] "
           << "Lvl " << (int)entry.price_level 
           << " | Price: " << get_full_price(entry.price_sign, entry.price)
           << " | Qty: " << entry.quantity << "\n";
    }
    
    as << "====================================";
    Logger::getInstance().log(as.str());
}
/**
 * Handle I083: Snapshot (Full Refresh)
 * Usually received at startup or during recovery to synchronize the Order Book.
 */
void on_snapshot(const I083_Packet& pkt) {
    std::stringstream ss;
    ss << "Received Packet (I083 - Snapshot):\n"
       << "----------------------------------------\n"
       << "Transmission Code  : " << pkt.header.transmission_code << "\n"
       << "Message Kind       : " << pkt.header.message_kind << " (I083 - Snapshot)\n"
       << "Information Time   : " << pkt.header.info_time << "\n"
       << "Channel ID         : " << pkt.header.channel_id << "\n"
       << "Channel Seq        : " << pkt.header.channel_seq << "\n"
       << "----------------------------------------\n"
       << "Prod ID            : " << pkt.prod_id << "\n"
       << "Prod Msg Seq       : " << pkt.prod_msg_seq << "\n"
       << "Calculated Flag    : " << pkt.calculated_flag 
       << " (" << (pkt.calculated_flag == '1' ? "Trial" : "Actual") << ")\n"
       << "No. MD Entries     : " << (int)pkt.no_md_entries << (pkt.no_md_entries == 0 ? " (Empty Book)" : "") << "\n";

    Logger::getInstance().log(ss.str());

    if (pkt.no_md_entries > 0) {
        std::stringstream as;
        as << "=== Analyzed Snapshot Content ===\n";
            for (const auto& entry : pkt.entries) {
                // Entry Type
            std::string type_str;
            switch(entry.entry_type) {
                case '0': type_str = "BID"; break;
                case '1': type_str = "ASK"; break;
                case 'E': type_str = "Implied BID"; break;
                case 'F': type_str = "Implied ASK"; break;
                default:  type_str = "Unknown";
            }

            std::string price_display;
            if (pkt.calculated_flag == '1') {
                if (entry.price == 999999999) price_display = "Market";
                else if (entry.price == 999999999 && entry.price_sign == '-') price_display = "Market";
                else price_display = get_full_price(entry.price_sign, entry.price);
            } else {
                price_display = get_full_price(entry.price_sign, entry.price);
            }
                as << "[" << type_str << "] "
                   << "Level " << (int)entry.price_level 
                   << " | Price: " << price_display  
                   << " | Qty: " << entry.quantity << "\n";
            }
        
        as << "==================================";
        Logger::getInstance().log(as.str());
    }
}

void on_refresh(const I084_Packet& pkt) {
    std::stringstream ss;
    ss << "Received Packet (I084 - Snapshot Refresh):\n"
       << "----------------------------------------\n"
       << "Message Kind       : " << pkt.header.message_kind << " (I084)\n"
       << "Information Time   : " << pkt.header.info_time << "\n"
       << "Channel Seq        : " << pkt.header.channel_seq << "\n"
       << "Message Type       : " << pkt.message_type << "\n";

    switch (pkt.message_type) {
        case 'A': ss << "Type               : Refresh Begin, LAST-SEQ=" << pkt.last_seq << "\n"; break;
        case 'Z': ss << "Type               : Refresh Complete, LAST-SEQ=" << pkt.last_seq << "\n"; break;
        case 'O': ss << "Type               : Order Data, NO-ENTRIES=" << (int)pkt.no_entries << "\n"; break;
        default:  ss << "Type               : " << pkt.message_type << " (unparsed)\n"; break;
    }
    Logger::getInstance().log(ss.str());

    if (pkt.message_type == 'O') {
        std::stringstream as;
        as << "=== Analyzed Snapshot Refresh ===\n";
        for (const auto& prod : pkt.products) {
            as << "Prod ID            : " << prod.prod_id
               << " | LAST-PROD-MSG-SEQ: " << prod.last_prod_msg_seq
               << " | NO-MD-ENTRIES: " << (int)prod.no_md_entries
               << (prod.no_md_entries == 0 ? " (Empty Book)" : "") << "\n";
            for (const auto& entry : prod.entries) {
                std::string type_str;
                switch (entry.entry_type) {
                    case '0': type_str = "BID"; break;
                    case '1': type_str = "ASK"; break;
                    case 'E': type_str = "Implied BID"; break;
                    case 'F': type_str = "Implied ASK"; break;
                    default:  type_str = "Unknown";
                }
                as << "  [" << type_str << "] Level " << (int)entry.price_level
                   << " | Price: " << get_full_price(entry.price_sign, entry.price)
                   << " | Qty: " << entry.quantity << "\n";
            }
        }
        as << "==================================";
        Logger::getInstance().log(as.str());
    }
}

/**
 * Wrapped handle_ mode: fired by the OrderBookManager whenever a maintained
 * book changes (including stale-flag flips). The reference is a snapshot that
 * is self-consistent but TRANSIENT — valid only for the duration of this
 * call. Copy the OrderBook explicitly if you need to keep it. (This handler
 * only reads it during the call, which is the intended pattern.)
 */
void handle_futopt_order_book(const OrderBook& book) {
    std::stringstream ss;
    ss << "=== Order Book [" << (book.is_stale ? "STALE" : "FRESH") << "] ===\n"
       << "Prod ID            : " << book.prod_id << "\n"
       << "Last Prod Msg Seq  : " << book.last_prod_msg_seq << "\n"
       << "Has Snapshot Base  : " << (book.has_snapshot ? "Yes" : "No") << "\n"
       << "Information Time   : " << book.info_time << "\n";

    auto print_side = [&](const char* name,
                          const std::array<OrderBookLevel, TAIFEX_BOOK_DEPTH>& side) {
        for (int i = 0; i < TAIFEX_BOOK_DEPTH; ++i) {
            if (!side[i].valid) continue;
            ss << "  [" << name << "] Level " << (i + 1)
               << " | Price: " << get_full_price(side[i].price_sign, side[i].price)
               << " | Qty: " << side[i].quantity << "\n";
        }
    };
    print_side("BID", book.bids);
    print_side("ASK", book.asks);
    print_side("Implied BID", book.derived_bids);
    print_side("Implied ASK", book.derived_asks);
    ss << "==============================";
    Logger::getInstance().log(ss.str());

    // Also echo a compact line to stdout so book transitions are easy to see.
    std::cout << "[BOOK][" << (book.is_stale ? "STALE" : "FRESH") << "] "
              << book.prod_id << " seq=" << book.last_prod_msg_seq << std::endl;
}

int main(int argc, char* argv[]) {
    // Register signal for graceful exit
    std::signal(SIGINT, signal_handler);

    // Initialize the singleton Logger
    Logger::getInstance().init("taifex_parser.log");

    TaifexParser parser;           // realtime: I024/I025/I081/I083
    TaifexParser snapshot_parser;  // I084 snapshot carousel (separate port)
    OrderBookManager book_mgr;
    int port = 14000;          // Default TAIFEX realtime UDP port
    int snapshot_port = 14700; // Default TAIFEX snapshot (I084) UDP port
    // PROD-ID short code: symbol + month letter (A=Jan..L=Dec) + year digit.
    // TXFG6 = TXF July 2026; update as contracts roll (or pass -prod).
    std::string prod = "TXFG6";
    // Live mode is selected with -mode; endpoints always come from the
    // manual's channel table (constants/taifex_channels.h), never from a
    // typed address. Without -mode the parsers bind the wildcard address —
    // the local unicast-replay form.
    std::string mode;
    std::string interface_ip;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "-prod" && i + 1 < argc) {
            prod = argv[++i];
        } else if (arg == "-iface" && i + 1 < argc) {
            interface_ip = argv[++i];
        }
    }

    if (!mode.empty()) {
        namespace tc = taifex::constants;
        tc::Session session;
        tc::Product product;
        if      (mode == "FUTURES_DAY")   { session = tc::Session::Day;   product = tc::Product::Futures; }
        else if (mode == "FUTURES_NIGHT") { session = tc::Session::Night; product = tc::Product::Futures; }
        else if (mode == "OPTIONS_DAY")   { session = tc::Session::Day;   product = tc::Product::Options; }
        else if (mode == "OPTIONS_NIGHT") { session = tc::Session::Night; product = tc::Product::Options; }
        else {
            std::cerr << "[ERROR] unknown -mode '" << mode << "'. Supported: "
                         "FUTURES_DAY, FUTURES_NIGHT, OPTIONS_DAY, OPTIONS_NIGHT"
                      << std::endl;
            return 1;
        }
        if (interface_ip.empty()) {
            std::cerr << "[ERROR] -mode requires -iface (the feed NIC's local "
                         "IP address, used for the multicast join)" << std::endl;
            return 1;
        }
        // A mode is a matched realtime + snapshot channel pair, so day and
        // night endpoints can never be mixed.
        const tc::Channel* rt   = tc::find(session, product, tc::Service::Realtime);
        const tc::Channel* snap = tc::find(session, product, tc::Service::SnapshotRefresh);
        port          = rt->port;
        snapshot_port = snap->port;
        parser.set_multicast(std::string(rt->group), interface_ip);
        snapshot_parser.set_multicast(std::string(snap->group), interface_ip);
    }

    // Wrapped handle_ mode: both parsers feed one shared manager, which
    // fires handle_futopt_order_book whenever this instrument's book changes.
    book_mgr.register_callback(prod, handle_futopt_order_book);
    // A wildcard registration (every instrument) is available too:
    //   book_mgr.register_callback_all(handle_futopt_order_book);
    parser.set_order_book_manager(&book_mgr);
    snapshot_parser.set_order_book_manager(&book_mgr);

    // Native on_ mode keeps working alongside (raw packet callbacks).
    if (!parser.start_loop(
            port,
            on_trade_match,   // I024 Callback
            on_day_high_low,  // I025 Callback
            on_incremental,   // I081 Callback
            on_snapshot,      // I083 Callback
            nullptr)) {       // I084 never arrives on the realtime port
        std::cerr << "[ERROR] realtime socket setup failed" << std::endl;
        return 1;
    }
    if (!snapshot_parser.start_loop(
            snapshot_port,
            nullptr,          // realtime kinds never arrive on the snapshot port...
            nullptr,
            nullptr,
            nullptr,
            on_refresh)) {    // ...but I084 logging is kept for visibility
        std::cerr << "[ERROR] snapshot socket setup failed" << std::endl;
        parser.end_loop();
        return 1;
    }

    std::cout << "TAIFEX Parser Service started on port " << port
              << " (snapshot port " << snapshot_port
              << ", book for " << prod << ")" << std::endl;
    if (!mode.empty()) {
        std::cout << "Mode: " << mode << " iface=" << interface_ip << std::endl;
    }
    std::cout << "Logs are being written to taifex_parser.log. Press Ctrl+C to exit." << std::endl;

    // Main execution thread stays here until interrupted
    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Cleanup and join threads
    std::cout << "\nShutting down parser..." << std::endl;
    parser.end_loop();
    snapshot_parser.end_loop();

    return 0;
}