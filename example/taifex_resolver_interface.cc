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

// --- TAIFEX Callback Handlers ---

void on_trade_match(const I024_Packet& pkt) {
    std::stringstream ss;
    ss << "[I024] Product: " << pkt.prod_id 
       << " | Match Price: " << pkt.first_price 
       << " | Qty: " << pkt.first_quantity
       << " | Cumulative Qty: " << pkt.total_qty;
    
    // Process consecutive matches if present (extracted from display_item bits)
    if (!pkt.consecutive_matches.empty()) {
        ss << " | Multi-Match Count: " << pkt.consecutive_matches.size();
    }
    
    Logger::getInstance().log(ss.str());
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