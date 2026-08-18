#include "parser.h"
#include "order_book.h"
#include <iostream>
#include <cstring>
#include <cerrno>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <algorithm>

TaifexParser::TaifexParser() : running(false), sockfd(-1) {}

void TaifexParser::set_multicast(const std::string& group, const std::string& iface_ip) {
    use_multicast = true;
    multicast_group = group;
    interface_ip = iface_ip;
}

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

void TaifexParser::format_bcd_time_to_char(const uint8_t* bcd, char* out_buf, bool has_micro) {
    if (has_micro) {
        snprintf(out_buf, 16, "%02x:%02x:%02x.%02x%02x%02x",
                 bcd[0], bcd[1], bcd[2], bcd[3], bcd[4], bcd[5]);
    } else {
        snprintf(out_buf, 16, "%02x:%02x:%02x", bcd[0], bcd[1], bcd[2]);
    }
}

bool TaifexParser::verify_checksum(const uint8_t* data, size_t len) {
    if (len < 4) return false; 
    uint8_t calculated_xor = 0;
    for (size_t i = 1; i < len - 3; ++i) {
        calculated_xor ^= data[i];
    }
    return calculated_xor == data[len - 3];
}

bool TaifexParser::start_loop(int port, I024Callback cb24, I025Callback cb25, I081Callback cb81, I083Callback cb83, I084Callback cb84) {
    if (running) return true;
    set_callbacks(cb24, cb25, cb81, cb83, cb84);
    // Socket setup runs HERE, on the caller's thread, before the receive
    // thread is spawned: every failure must reach the caller, and only
    // end_loop may ever tear the socket down once the thread exists.
    if (!setup_socket(port)) return false;
    running = true;
    recv_thread = std::thread(&TaifexParser::receive_loop, this);
    return true;
}

void TaifexParser::set_callbacks(I024Callback cb24, I025Callback cb25, I081Callback cb81, I083Callback cb83, I084Callback cb84) {
    on_i024 = cb24;
    on_i025 = cb25;
    on_i081 = cb81;
    on_i083 = cb83;
    on_i084 = cb84;
}

void TaifexParser::set_order_book_manager(OrderBookManager* mgr) {
    book_mgr_ = mgr;
}

void TaifexParser::end_loop() {
    // `running = false` MUST precede shutdown(): the shutdown wakes the
    // blocked recvfrom (with a zero-length return), and the loop's re-check
    // of `running` is what lets the thread exit.
    running = false;
    if (sockfd != -1) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
        sockfd = -1;
    }
    if (recv_thread.joinable()) recv_thread.join();
}

namespace {
std::string trim_copy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
}  // namespace

bool TaifexParser::setup_socket(int port) {
    // Resolve group + interface up front so a malformed configuration fails
    // here instead of silently degrading. inet_pton, not inet_addr: the
    // legacy 3-part form does not fail — inet_addr("225.140.140") yields
    // 225.140.0.140, a valid but WRONG group that would join cleanly and
    // subscribe to the wrong feed with no error anywhere.
    in_addr_t group_addr = htonl(INADDR_ANY);
    in_addr_t iface_addr = htonl(INADDR_ANY);
    if (use_multicast) {
        const std::string group = trim_copy(multicast_group);
        const std::string iface = trim_copy(interface_ip);
        if (inet_pton(AF_INET, group.c_str(), &group_addr) != 1 ||
            !IN_MULTICAST(ntohl(group_addr))) {
            std::cerr << "[ERROR] invalid multicast group '" << multicast_group
                      << "' — expected a dotted-quad IPv4 multicast address "
                         "(four octets, no leading zeros)" << std::endl;
            return false;
        }
        if (iface.empty() || inet_pton(AF_INET, iface.c_str(), &iface_addr) != 1) {
            // Required: with imr_interface = INADDR_ANY the kernel resolves
            // the join device by ROUTING THE GROUP ADDRESS. Hosts without a
            // 224.0.0.0/4 route then join on the default-route interface —
            // the join succeeds, the socket receives nothing, silently.
            std::cerr << "[ERROR] interface IP is required with a multicast "
                         "group and must be a local address; got '"
                      << interface_ip << "'" << std::endl;
            return false;
        }
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "[ERROR] socket() failed: " << strerror(errno) << std::endl;
        return false;
    }

    int reuse = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Enlarge the receive queue so brief consumer stalls don't drop packets.
    // SO_RCVBUFFORCE bypasses net.core.rmem_max but requires CAP_NET_ADMIN;
    // fall back to SO_RCVBUF (clamped to rmem_max) when unavailable.
    int rcvbuf = 64 * 1024 * 1024;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) < 0) {
        setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }
    int actual_rcvbuf = 0;
    socklen_t optlen = sizeof(actual_rcvbuf);
    getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf, &optlen);
    std::cerr << "[INFO] recv buffer: " << actual_rcvbuf << " bytes" << std::endl;

    if (use_multicast) {
        // Belt: turn OFF the "deliver every group the HOST joined" default.
        // With IP_MULTICAST_ALL=1 (the kernel default), a socket receives
        // any group on its port that ANY process on the host has joined —
        // measured: a group-bound socket with a failed join still received
        // the feed through a co-tenant's membership. With it off, this
        // socket's own IP_ADD_MEMBERSHIP below is the only thing that
        // admits traffic, which is what makes a failed join a real error.
        int mc_all = 0;
        if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_ALL, &mc_all, sizeof(mc_all)) < 0) {
            std::cerr << "[WARN] IP_MULTICAST_ALL=0 failed: " << strerror(errno)
                      << " — group isolation still enforced by the bind address"
                      << std::endl;
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // Bind to the GROUP, not INADDR_ANY: TAIFEX day and night sessions share
    // every port and differ only by group (225.0.x day vs 225.10.x night),
    // so a wildcard bind lets the other session's feed leak into this
    // listener. Binding the group address makes the kernel demultiplex for
    // us. Legal on Linux without privilege, and legal before the IGMP join.
    addr.sin_addr.s_addr = use_multicast ? group_addr : htonl(INADDR_ANY);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[ERROR] bind failed on "
                  << (use_multicast ? multicast_group : std::string("0.0.0.0"))
                  << ":" << port << ": " << strerror(errno) << std::endl;
        close(sockfd);
        sockfd = -1;
        return false;
    }

    if (use_multicast) {
        struct ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = group_addr;
        mreq.imr_interface.s_addr = iface_addr;  // receive-side iface selector

        std::cerr << "[INFO] Attempting to join multicast group " << multicast_group
                  << " on interface " << interface_ip << std::endl;

        if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            std::cerr << "[WARN] IP_ADD_MEMBERSHIP failed: " << strerror(errno)
                      << " — continuing without IGMP join" << std::endl;
        }

        struct in_addr local_interface{};
        local_interface.s_addr = iface_addr;
        if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_IF, &local_interface, sizeof(local_interface)) < 0) {
            std::cerr << "[WARN] IP_MULTICAST_IF failed: " << strerror(errno)
                      << " — continuing" << std::endl;
        }
    }
    return true;
}

void TaifexParser::receive_loop() {
    uint8_t buffer[4096];
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        ssize_t len = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &addr_len);
        if (len > 0) {
            process_datagram(buffer, static_cast<size_t>(len));
        } else {
            // len == 0 means the socket was shut down (end_loop). shutdown()
            // returns ENOTCONN on an unconnected UDP socket but still wakes
            // this recvfrom, which then returns 0 forever — without this
            // branch the loop would spin at 100% CPU whenever `running` is
            // not already false.
            if (len < 0 && errno != EINTR && errno != EBADF) {
                std::cerr << "[ERROR] recvfrom error: " << strerror(errno) << std::endl;
            }
            break;
        }
    }
}

void TaifexParser::process_datagram(const uint8_t* data, size_t len) {
    if (len < 2) return;
    size_t start_pos = 0;
    for (size_t i = 0; i < len - 1; i++) {
        if (data[i] == 0x0D && data[i + 1] == 0x0A) {
            process_raw_data(data + start_pos, i + 2 - start_pos);
            start_pos = i + 2;
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
    format_bcd_time_to_char(data + 3, header.info_time, true);
    header.channel_id = (uint16_t)bcd_to_uint(data + 9, 2);
    header.channel_seq = (uint32_t)bcd_to_uint(data + 11, 5);
    header.version_no = (uint8_t)bcd_to_uint(data + 16, 1);
    header.body_len = (uint16_t)bcd_to_uint(data + 17, 2);

    // The order book manager tracks CHANNEL-SEQ over every message on the
    // channel (including I001 heartbeats, I002 sequence resets and kinds this
    // parser does not decode), so it is fed each valid header up front.
    if (book_mgr_) book_mgr_->on_header(header);

    switch (header.message_kind) {
        case 'D': handle_i024(data, header); break;
        case 'E': handle_i025(data, length, header); break;
        case 'A': handle_i081(data, header); break;
        case 'B': handle_i083(data, header); break;
        case 'C': handle_i084(data, header); break;
    }
}

// --- Handler: I024 (Trade) ---

bool TaifexParser::handle_i024(const uint8_t* data, const Header& header) {
    I024_Packet pkt;
    pkt.header = header;
    size_t offset = 19; 

    // 1. Prod ID
    memcpy(pkt.prod_id, data + offset, 20);
    pkt.prod_id[20] = '\0';
    offset += 20;

    // 2. Seq & Flag
    pkt.prod_msg_seq = (uint32_t)bcd_to_uint(data + offset, 5);
    offset += 5;
    pkt.calculated_flag = data[offset++];

    // 3. Match Time
    format_bcd_time_to_char(data + offset, pkt.match_time, true);
    offset += 6;

    // 4. First Price 
    pkt.first_price_sign = data[offset++];
    pkt.first_price = bcd_to_uint(data + offset, 5);
    offset += 5;

    // 5. First Qty 
    pkt.first_quantity = (uint32_t)bcd_to_uint(data + offset, 4); 
    offset += 4;

    // 6. Display Item & Occurs
    pkt.display_item = data[offset++];
    int occurs = pkt.display_item & 0x7F; 

    // 7. Repeated Match Data
    for (int i = 0; i < occurs; ++i) {
        MatchData md;
        md.price_sign = data[offset++];
        md.price = bcd_to_uint(data + offset, 5);
        offset += 5;
        md.quantity = (uint16_t)bcd_to_uint(data + offset, 2); 
        offset += 2;
        pkt.consecutive_matches.push_back(md);
    }

    // 8. Cumulative Data 
    pkt.total_qty = (uint32_t)bcd_to_uint(data + offset, 4); offset += 4;
    pkt.buy_cnt = (uint32_t)bcd_to_uint(data + offset, 4);   offset += 4;
    pkt.sell_cnt = (uint32_t)bcd_to_uint(data + offset, 4);  offset += 4;

    if (book_mgr_) book_mgr_->on_i024(pkt);
    if (on_i024) on_i024(pkt);
    return true;
}

// --- Handler: I025 (Intraday Day-High/Low) ---
// Parsed chiefly because I025 consumes the per-product PROD-MSG-SEQ serial
// that the order book manager relies on for loss detection.

bool TaifexParser::handle_i025(const uint8_t* data, size_t length, const Header& header) {
    // Fixed 43-byte body + 19-byte prefix (ESC + header) + 3-byte trailer
    // (checksum + 0D0A). Shorter segments would be read out of bounds.
    if (length < 19 + 43 + 3) return false;

    I025_Packet pkt;
    pkt.header = header;
    size_t offset = 19;

    memcpy(pkt.prod_id, data + offset, 20);
    pkt.prod_id[20] = '\0';
    offset += 20;

    pkt.prod_msg_seq = (uint32_t)bcd_to_uint(data + offset, 5);
    offset += 5;

    pkt.day_high_price_sign = data[offset++];
    pkt.day_high_price = bcd_to_uint(data + offset, 5);
    offset += 5;

    pkt.day_low_price_sign = data[offset++];
    pkt.day_low_price = bcd_to_uint(data + offset, 5);
    offset += 5;

    format_bcd_time_to_char(data + offset, pkt.show_time, true);
    offset += 6;

    if (book_mgr_) book_mgr_->on_i025(pkt);
    if (on_i025) on_i025(pkt);
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

    if (book_mgr_) book_mgr_->on_i081(pkt);
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
        SnapshotEntry entry;
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

    if (book_mgr_) book_mgr_->on_i083(pkt);
    if (on_i083) on_i083(pkt);
    return true;
}

// --- Handler: I084 (Snapshot Refresh) ---
// MESSAGE-TYPE at offset 19 selects the body. We resolve 'A'/'O'/'Z'; 'S'/'P'
// (statistics / product status) are delivered header+type only (out of scope).

bool TaifexParser::handle_i084(const uint8_t* data, const Header& header) {
    I084_Packet pkt;
    pkt.header = header;
    pkt.last_seq = 0;
    pkt.no_entries = 0;
    size_t offset = 19;

    pkt.message_type = data[offset++];

    switch (pkt.message_type) {
        case 'A':   // Refresh Begin
        case 'Z':   // Refresh Complete
            pkt.last_seq = (uint32_t)bcd_to_uint(data + offset, 5);
            offset += 5;
            break;

        case 'O': { // Order Data: NO-ENTRIES products, each a small order book
            pkt.no_entries = (uint8_t)bcd_to_uint(data + offset, 1);
            offset += 1;

            for (int p = 0; p < pkt.no_entries; ++p) {
                I084Product prod;
                memcpy(prod.prod_id, data + offset, 20);
                prod.prod_id[20] = '\0';
                offset += 20;

                prod.last_prod_msg_seq = (uint32_t)bcd_to_uint(data + offset, 5);
                offset += 5;

                prod.no_md_entries = (uint8_t)bcd_to_uint(data + offset, 1);
                offset += 1;

                for (int i = 0; i < prod.no_md_entries; ++i) {
                    SnapshotEntry entry;
                    entry.entry_type = data[offset++];
                    entry.price_sign = data[offset++];
                    entry.price = bcd_to_uint(data + offset, 5);
                    offset += 5;
                    entry.quantity = (uint32_t)bcd_to_uint(data + offset, 4);
                    offset += 4;
                    entry.price_level = (uint8_t)bcd_to_uint(data + offset, 1);
                    offset += 1;
                    prod.entries.push_back(entry);
                }
                pkt.products.push_back(prod);
            }
            break;
        }

        default:
            // 'S' / 'P' — body layout not parsed; deliver header + type marker.
            break;
    }

    if (book_mgr_) book_mgr_->on_i084(pkt);
    if (on_i084) on_i084(pkt);
    return true;
}