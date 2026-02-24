#include "parser.h"
#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <algorithm>

TaifexParser::TaifexParser() : running(false), sockfd(-1) {}

TaifexParser::~TaifexParser() {
    end_loop();
}

uint64_t TaifexParser::bcd_to_uint(const uint8_t* bcd, size_t len) {
    uint64_t result = 0;
    for (size_t i = 0; i < len; ++i) {
        result = result * 100 + ((bcd[i] >> 4) * 10) + (bcd[i] & 0x0F);
    }
    return result;
}

bool TaifexParser::verify_checksum(const uint8_t* data, size_t len) {
    if (len < 4) return false; 
    uint8_t calculated_xor = 0;
    // Checksum 是從 Transmission Code (index 1) 到 Checksum 位元之前 (len-3)
    for (size_t i = 1; i < len - 3; ++i) {
        calculated_xor ^= data[i];
    }
    return calculated_xor == data[len - 3];
}

void TaifexParser::start_loop(int port, I024Callback cb24, I081Callback cb81, I083Callback cb83) {
    if (running) return;
    running = true;
    on_i024 = cb24;
    on_i081 = cb81;
    on_i083 = cb83;
    recv_thread = std::thread(&TaifexParser::receive_loop, this, port);
}

void TaifexParser::end_loop() {
    running = false;
    if (sockfd != -1) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
        sockfd = -1;
    }
    if (recv_thread.joinable()) recv_thread.join();
}

void TaifexParser::receive_loop(int port) {
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) return;

    int reuse = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[ERROR] Bind failed on port " << port << std::endl;
        return;
    }

    std::cerr << ">>> [SERVER LIVE] Listening on UDP " << port << " <<<" << std::endl;

    uint8_t buffer[4096];
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        // 修正：只調用一次接收函數
        ssize_t len = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &addr_len);

        if (len > 0) {
            std::cerr << "[GOT DATA] Length: " << len << " bytes" << std::endl;
            
            size_t start_pos = 0;
            for (size_t i = 0; i < (size_t)len - 1; i++) {
                if (buffer[i] == 0x0D && buffer[i + 1] == 0x0A) {
                    process_raw_data(buffer + start_pos, i + 2 - start_pos);
                    start_pos = i + 2;
                }
            }
        }
    }
}

void TaifexParser::process_raw_data(const uint8_t* data, size_t length) {
    if (length < 20 || data[0] != ESC_CODE) return;
    if (!verify_checksum(data, length)) {
        std::cerr << "[DEBUG] Checksum failed" << std::endl;
        return;
    }

    Header header;
    header.transmission_code = data[1];
    header.message_kind = data[2];
    header.info_time = std::to_string(bcd_to_uint(data + 3, 6));
    header.channel_id = (uint16_t)bcd_to_uint(data + 9, 2);
    header.channel_seq = (uint32_t)bcd_to_uint(data + 11, 5);
    header.version_no = (uint8_t)bcd_to_uint(data + 16, 1);
    header.body_len = (uint16_t)bcd_to_uint(data + 17, 2);

    std::cerr << "[PARSING] MessageKind: " << header.message_kind << std::endl;

    switch (header.message_kind) {
        case 'D': handle_i024(data, header); break; 
        case 'A': handle_i081(data, header); break; 
        case 'B': handle_i083(data, header); break; 
    }
}

// --- Handler: I024 (Trade) ---

bool TaifexParser::handle_i024(const uint8_t* data, const Header& header) {
    I024_Packet pkt;
    pkt.header = header;
    size_t offset = 19; // ESC(1) + Header(18)

    memcpy(pkt.prod_id, data + offset, 20);
    pkt.prod_id[20] = '\0';
    offset += 20;

    pkt.prod_msg_seq = (uint32_t)bcd_to_uint(data + offset, 5);
    offset += 5;

    pkt.calculated_flag = data[offset++];

    uint64_t mt = bcd_to_uint(data + offset, 6);
    snprintf(pkt.match_time, sizeof(pkt.match_time), "%012lu", mt);
    offset += 6;

    pkt.first_price_sign = data[offset++];
    pkt.first_price = bcd_to_uint(data + offset, 5);
    offset += 5;
    pkt.first_quantity = (uint32_t)bcd_to_uint(data + offset, 4); // 4 bytes
    offset += 4;

    pkt.display_item = data[offset++];
    int occurs = pkt.display_item & 0x7F; // Bit 0-6

    for (int i = 0; i < occurs; ++i) {
        MatchData md;
        md.price_sign = data[offset++];
        md.price = bcd_to_uint(data + offset, 5);
        offset += 5;
        md.quantity = (uint16_t)bcd_to_uint(data + offset, 2); // Repeat Qty is 2 bytes!
        offset += 2;
        pkt.consecutive_matches.push_back(md);
    }

    pkt.total_qty = (uint32_t)bcd_to_uint(data + offset, 4); offset += 4;
    pkt.buy_cnt = (uint32_t)bcd_to_uint(data + offset, 4);   offset += 4;
    pkt.sell_cnt = (uint32_t)bcd_to_uint(data + offset, 4);  offset += 4;

    if (on_i024) on_i024(pkt);
    return true;
}

// --- Handler: I081 (Incremental) ---

bool TaifexParser::handle_i081(const uint8_t* data, const Header& header) {
    I081_Packet pkt;
    pkt.header = header;
    size_t offset = 19;

    memcpy(pkt.prod_id, data + offset, 20);
    pkt.prod_id[20] = '\0';
    offset += 20;

    pkt.prod_msg_seq = (uint32_t)bcd_to_uint(data + offset, 5);
    offset += 5;

    pkt.no_md_entries = (uint8_t)bcd_to_uint(data + offset, 1);
    offset += 1;

    for (int i = 0; i < pkt.no_md_entries; ++i) {
        MDEntry entry;
        entry.update_action = data[offset++];
        entry.entry_type = data[offset++];
        entry.price_sign = data[offset++];
        entry.price = bcd_to_uint(data + offset, 5);
        offset += 5;
        entry.quantity = (uint32_t)bcd_to_uint(data + offset, 4);
        offset += 4;
        entry.price_level = (uint8_t)bcd_to_uint(data + offset, 1);
        offset += 1;
        pkt.entries.push_back(entry);
    }

    if (on_i081) on_i081(pkt);
    return true;
}

// --- Handler: I083 (Snapshot) ---

bool TaifexParser::handle_i083(const uint8_t* data, const Header& header) {
    I083_Packet pkt;
    pkt.header = header;
    size_t offset = 19;

    memcpy(pkt.prod_id, data + offset, 20);
    pkt.prod_id[20] = '\0';
    offset += 20;

    pkt.prod_msg_seq = (uint32_t)bcd_to_uint(data + offset, 5);
    offset += 5;

    pkt.calculated_flag = data[offset++]; // I083 special field

    pkt.no_md_entries = (uint8_t)bcd_to_uint(data + offset, 1);
    offset += 1;

    for (int i = 0; i < pkt.no_md_entries; ++i) {
        MDEntry entry;
        entry.update_action = ' '; // Not applicable for snapshot
        entry.entry_type = data[offset++];
        entry.price_sign = data[offset++];
        entry.price = bcd_to_uint(data + offset, 5);
        offset += 5;
        entry.quantity = (uint32_t)bcd_to_uint(data + offset, 4);
        offset += 4;
        entry.price_level = (uint8_t)bcd_to_uint(data + offset, 1);
        offset += 1;
        pkt.entries.push_back(entry);
    }

    if (on_i083) on_i083(pkt);
    return true;
}