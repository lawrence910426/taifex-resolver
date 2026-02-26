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

std::string to_hex_str(uint32_t val) {
    std::stringstream ss;
    ss << "0x" << std::hex << std::uppercase << val;
    return ss.str();
}

// --- TAIFEX Callback Handlers ---

void on_trade_match(const I024_Packet& pkt) {
    std::stringstream ss;

    auto get_full_price = [](char sign, uint64_t price) {
        std::string s = (sign == '-' ? "-" : "");
        return s + std::to_string(price);
    };

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
       << " (Sign: '" << pkt.first_price_sign << "')"
       << ", Quantity = " << pkt.first_quantity << "\n";


    if (multi_match_count > 0) {
        for (size_t i = 0; i < pkt.consecutive_matches.size(); ++i) {
            const auto& m = pkt.consecutive_matches[i];
            as << "Next Deal " << std::setw(2) << (i + 1) 
               << ": Price = " << get_full_price(m.price_sign, m.price)
               << " (Sign: '" << m.price_sign << "')"
               << ", Quantity = " << m.quantity << "\n";
        }
    }

    as << "Trial Match Note   : " << (pkt.calculated_flag == '1' ? "Trial (Remaining Order Book)" : "Actual Match") << "\n";
    as << "========================";

    Logger::getInstance().log(as.str());
}

void on_incremental(const I081_Packet& pkt) {
    std::stringstream ss;
    ss << "[I081] Product: " << pkt.prod_id << " | Updates: " << (int)pkt.no_md_entries;
    
    for (const auto& entry : pkt.entries) {
        ss << "\n  -> " << (entry.entry_type == '0' ? "BID" : "ASK")
           << " Level " << (int)entry.price_level 
           << " Action " << entry.update_action
           << ": " << entry.price << " @ " << entry.quantity;
    }
    Logger::getInstance().log(ss.str());
}

/**
 * Handle I083: Snapshot (Full Refresh)
 * Usually received at startup or during recovery to synchronize the Order Book.
 */
void on_snapshot(const I083_Packet& pkt) {
    std::stringstream ss;
    ss << "[I083] Snapshot for " << pkt.prod_id << " | Levels: " << (int)pkt.no_md_entries;
    Logger::getInstance().log(ss.str());
}

int main(int argc, char* argv[]) {
    // Register signal for graceful exit
    std::signal(SIGINT, signal_handler);

    // Initialize the singleton Logger
    Logger::getInstance().init("taifex_parser.log");

    TaifexParser parser;
    int port = 10000; // Default TAIFEX UDP port

    // Simple command-line argument parsing
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        }
    }

    parser.start_loop(
        port, 
        on_trade_match,  // I024 Callback
        on_incremental,  // I081 Callback
        on_snapshot      // I083 Callback
    );

    std::cout << "TAIFEX Parser Service started on port " << port << std::endl;
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