#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <sstream>
#include "parser.h"
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

auto    get_formatted_price = [](char sign, uint64_t price, int decimal_places) {
    std::string s_price = std::to_string(price);
    
    if (s_price.length() <= (size_t)decimal_places) {
        s_price.insert(0, decimal_places - s_price.length() + 1, '0');
    }

    if (decimal_places > 0) {
        s_price.insert(s_price.length() - decimal_places, ".");
    }

    return (sign == '-' ? "-" : "") + s_price;
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


    as << "First Deal: Price = " << get_formatted_price(pkt.first_price_sign, pkt.first_price, pkt.first_price_decimal)
       << ", Quantity = " << pkt.first_quantity << "\n";


    if (multi_match_count > 0) {
        for (const auto& m : pkt.consecutive_matches) {
            as << "Next Deal: " 
               << get_formatted_price(m.price_sign, m.price, m.decimal_locator) 
               << " Qty: " << m.quantity << "\n";
        }
    }

    as << "Trial Match Note   : " << (pkt.calculated_flag == '1' ? "Trial (Remaining Order Book)" : "Actual Match") << "\n";
    as << "========================";

    Logger::getInstance().log(as.str());
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
           << " | Price: " << get_formatted_price(entry.price_sign, entry.price, entry.decimal_locator)
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
                else price_display = get_formatted_price(entry.price_sign, entry.price, entry.decimal_locator);
            } else {
                price_display = get_formatted_price(entry.price_sign, entry.price, entry.decimal_locator);
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

int main(int argc, char* argv[]) {
    // Register signal for graceful exit
    std::signal(SIGINT, signal_handler);

    // Initialize the singleton Logger
    Logger::getInstance().init("taifex_parser.log");

    TaifexParser parser;
    int port = 14000; // Default TAIFEX UDP port
    std::string multicast_group;
    std::string interface_ip;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "-multicast" && i + 1 < argc) {
            multicast_group = argv[++i];
        } else if (arg == "-iface" && i + 1 < argc) {
            interface_ip = argv[++i];
        }
    }

    if (!multicast_group.empty() && !interface_ip.empty()) {
        parser.set_multicast(multicast_group, interface_ip);
    }

    parser.start_loop(
        port, 
        on_trade_match,  // I024 Callback
        on_incremental,  // I081 Callback
        on_snapshot      // I083 Callback
    );

    std::cout << "TAIFEX Parser Service started on port " << port << std::endl;
    if (!multicast_group.empty() && !interface_ip.empty()) {
        std::cout << "Multicast: group=" << multicast_group
                  << " iface=" << interface_ip << std::endl;
    }
    std::cout << "Logs are being written to taifex_parser.log. Press Ctrl+C to exit." << std::endl;

    // Main execution thread stays here until interrupted
    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Cleanup and join threads
    std::cout << "\nShutting down parser..." << std::endl;
    parser.end_loop();

    return 0;
}