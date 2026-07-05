import argparse
import socket
import time

def calculate_checksum(data):
    """Calculate the XOR checksum of the given data."""
    checksum = 0
    for byte in data:
        checksum ^= byte
    return checksum

def _bcd(value, nbytes):
    """Pack a non-negative int as PACK BCD across `nbytes` (2 digits/byte)."""
    digits = str(value).zfill(nbytes * 2)
    if len(digits) > nbytes * 2:
        raise ValueError(f"{value} does not fit in {nbytes} BCD bytes")
    return bytes((int(digits[i]) << 4) | int(digits[i + 1]) for i in range(0, len(digits), 2))

def create_packet_format_I024_TAIFEX():
    # ESC-CODE (ASCII 27)
    esc_code = bytes([0x1B]) 
    
    # HEADER (Common Quote Header - 18 Bytes)
    # Ref: Transmission Code(1), Message Kind(1), Info Time(6), Channel ID(2), Seq(5), Ver(1), Len(2)
    header = (
        b'\x32' +                      # 1.1 Transmission Code: '2'
        b'\x44' +                      # 1.2 Message Kind: 'D' (Quote)
        b'\x13\x15\x30\x99\x88\x77' +  # 1.3 Info Time: 13:15:30.998877 (BCD)
        b'\x00\x01' +                  # 1.4 Channel ID: 1 (BCD)
        b'\x00\x00\x00\x00\x01' +      # 1.5 Channel Seq: 1 (BCD)
        b'\x01' +                      # 1.6 Version: 1 (BCD)
        b'\x00\x63'                    # 1.7 Body Length: 63 (BCD)
    )
    
    # BODY (I024 Market Data)
    body = (
        b'TXF202603           ' +      # 2.1 Prod-ID: "TXF202603           " (X20)
        b'\x00\x00\x00\x01\x23' +      # 2.2 Prod-Msg-Seq: 123 (BCD)
        b'\x30' +                      # 2.3 Calculated-Flag: '0' (Normal)
        b'\x13\x15\x30\x99\x88\x77' +  # 2.4 Match-Time: 13:15:30.998877 (BCD)
        
        # --- First Match Data ---
        b'\x2D' +                      # 2.5 First-Match Sign: '-' (-)
        b'\x00\x12\x00\x05\x00' +      # 2.6 First-Match Price: 120005.00 (BCD)
        b'\x00\x00\x00\x10' +          # 2.7 First-Match Qty: 10 (BCD)
        
        # --- MATCH-DISPLAY-ITEM & OCCURS ---
        b'\x81' +                      # 2.8 Display Item: 1st Packet(1) + 1 Occur(1) -> 10000001
        
        # --- Match Data (Repeated) ---
        b'\x30' +                      # 2.9 Match Sign: '0' (+)
        b'\x00\x12\x00\x04\x50' +      # 2.10 Match Price: 120004.50 (BCD)
        b'\x00\x05' +                  # 2.11 Match Qty: 5 (BCD)
        
        # --- Aggregated Statistics ---
        b'\x00\x00\x00\x15' +          # 2.12 Total Qty: 15 (BCD)
        b'\x00\x00\x00\x02' +          # 2.13 Buy Count: 2 (BCD)
        b'\x00\x00\x00\x02'            # 2.14 Sell Count: 2 (BCD)
    )
    
    # Calculate checksum (XOR from Header[0] to Body[last])
    # The range: TRANSMISSION-CODE to MATCH-SELL-CNT
    checksum_val = 0
    for byte in (header + body):
        checksum_val ^= byte
    checksum = bytes([checksum_val]) # 3.1 Checksum: X(1)
    
    # TERMINAL-CODE
    terminal_code = b'\x0D\x0A'      # 4.1 Terminal: 0x0D 0x0A
    
    return esc_code + header + body + checksum + terminal_code

def create_packet_format_I081_TAIFEX():
    # ESC-CODE (ASCII 27)
    esc_code = bytes([0x1B]) 
    
    # --- 1. COMMON HEADER (18 Bytes) ---
    header = (
        b'\x32' +                      # 1.1 TRANSMISSION-CODE: '2' (Futures Real-time)
        b'\x41' +                      # 1.2 MESSAGE-KIND: 'A' (I081)
        b'\x13\x15\x35\x12\x34\x56' +  # 1.3 INFORMATION-TIME: 13:15:35.123456 (BCD)
        b'\x00\x01' +                  # 1.4 CHANNEL-ID: 0001 (BCD)
        b'\x00\x00\x00\x00\x02' +      # 1.5 CHANNEL-SEQ: 2 (BCD)
        b'\x01' +                      # 1.6 VERSION-NO: 1 (BCD, As per 2020 table)
        b'\x00\x52'                    # 1.7 BODY-LENGTH: 52 (BCD, Calculated)
    )
    
    # --- 2. BODY (I081 Book Entries) ---
    prod_id = b'TXF202603           '        # 2.1 PROD-ID (X20)
    prod_msg_seq = b'\x00\x00\x00\x02\x46'   # 2.2 PROD-MSG-SEQ: 246 (BCD)
    
    # NO-MD-ENTRIES: Number of price/qty updates (e.g., 2 updates)
    no_md_entries = b'\x02'                  # 2.3 NO-MD-ENTRIES: 2 groups (BCD 1 byte)

    # --- MD-ENTRY Loop Group 1: New Buy Order ---
    entry_1 = (
        b'\x30' +                            # 2.4 MD-UPDATE-ACTION: '0' (New)
        b'\x30' +                            # 2.5 MD-ENTRY-TYPE: '0' (Buy)
        b'\x30' +                            # 2.6 SIGN: '0' (+)
        b'\x00\x01\x20\x00\x50' +            # 2.7 MD-ENTRY-PX: 12000.50 (BCD)
        b'\x00\x00\x00\x10' +                # 2.8 MD-ENTRY-SIZE: 10 (BCD)
        b'\x01'                              # 2.9 MD-PRICE-LEVEL: 1 (BCD 1 byte)
    )

    # --- MD-ENTRY Loop Group 2: Change Sell Order ---
    entry_2 = (
        b'\x31' +                            # 2.10 MD-UPDATE-ACTION: '1' (Change)
        b'\x31' +                            # 2.11 MD-ENTRY-TYPE: '1' (Sell)
        b'\x30' +                            # 2.12 SIGN: '0' (+)
        b'\x00\x01\x20\x00\x60' +            # 2.13 MD-ENTRY-PX: 12000.60 (BCD)
        b'\x00\x00\x00\x21' +                # 2.14 MD-ENTRY-SIZE: 21 (BCD)
        b'\x01'                              # 2.15 MD-PRICE-LEVEL: 1 (BCD 1 byte)
    )

    body = prod_id + prod_msg_seq + no_md_entries + entry_1 + entry_2
    
    # --- 3. CHECK-SUM (LRC XOR) ---
    content_to_check = header + body
    lrc_val = 0
    for byte in content_to_check:
        lrc_val ^= byte
    checksum = bytes([lrc_val])        # 3.1 CHECK-SUM: X(1)
    
    # --- 4. TERMINAL ---
    terminal_code = b'\x0D\x0A'        # 4.1 TERMINAL-CODE: HEX 0D0A
    
    return esc_code + content_to_check + checksum + terminal_code

def create_packet_format_I083_TAIFEX():
    # ESC-CODE (ASCII 27)
    esc_code = bytes([0x1B]) 
    
    # --- 1. COMMON HEADER (18 Bytes) ---
    # I083 Message Kind is 'B' (0x42)
    header = (
        b'\x32' +                      # 1.1 TRANSMISSION-CODE: '2' (Futures Real-time)
        b'\x42' +                      # 1.2 MESSAGE-KIND: 'B' (I083 Snapshot)
        b'\x13\x15\x40\x00\x00\x00' +  # 1.3 INFORMATION-TIME: 13:15:40.000000 (BCD)
        b'\x00\x01' +                  # 1.4 CHANNEL-ID: 0001 (BCD)
        b'\x00\x00\x00\x00\x03' +      # 1.5 CHANNEL-SEQ: 3 (BCD)
        b'\x01' +                      # 1.6 VERSION-NO: 1 (BCD)
        b'\x00\x75'                    # 1.7 BODY-LENGTH: 75 (BCD, Calculated Below)
    )
    
    # --- 2. BODY (I083 Snapshot Data) ---
    prod_id = b'TXF202603           '        # 2.1 PROD-ID (X20)
    prod_msg_seq = b'\x00\x00\x00\x02\x47'   # 2.2 PROD-MSG-SEQ: 247 (BCD)
    calc_flag = b'\x30'                      # 2.3 CALCULATED-FLAG: '0' (Normal Order Book)
    
    # NO-MD-ENTRIES: Let's simulate 4 entries (Best Bid/Ask + Derivative Bid/Ask)
    no_md_entries = b'\x04'                  # 2.4 NO-MD-ENTRIES: 4 groups (BCD 1 byte)

    # --- MD-ENTRY Loop Group 1: Best Bid (Level 1) ---
    entry_1 = (
        b'\x30' +                            # 2.5 MD-ENTRY-TYPE: '0' (Buy)
        b'\x30' +                            # 2.6 SIGN: '0' (+)
        b'\x00\x01\x20\x00\x50' +            # 2.7 MD-ENTRY-PX: 12000.50 (BCD)
        b'\x00\x00\x00\x10' +                # 2.8 MD-ENTRY-SIZE: 10 (BCD)
        b'\x01'                              # 2.9 MD-PRICE-LEVEL: 1 (BCD)
    )

    # --- MD-ENTRY Loop Group 2: Best Ask (Level 1) ---
    entry_2 = (
        b'\x31' +                            # 2.10 MD-ENTRY-TYPE: '1' (Sell)
        b'\x30' +                            # 2.11 SIGN: '0' (+)
        b'\x00\x01\x20\x00\x60' +            # 2.12 MD-ENTRY-PX: 12000.60 (BCD)
        b'\x00\x00\x00\x08' +                # 2.13 MD-ENTRY-SIZE: 8 (BCD)
        b'\x01'                              # 2.14 MD-PRICE-LEVEL: 1 (BCD)
    )

    # --- MD-ENTRY Loop Group 3: Derived Bid (Type E) ---
    entry_3 = (
        b'\x45' +                            # 2.15 MD-ENTRY-TYPE: 'E' (Derived Buy)
        b'\x30' +                            # 2.16 SIGN: '0' (+)
        b'\x00\x01\x20\x00\x45' +            # 2.17 MD-ENTRY-PX: 12000.45 (BCD)
        b'\x00\x00\x00\x02' +                # 2.18 MD-ENTRY-SIZE: 2 (BCD)
        b'\x01'                              # 2.19 MD-PRICE-LEVEL: 1 (BCD)
    )

    # --- MD-ENTRY Loop Group 4: Derived Ask (Type F) ---
    entry_4 = (
        b'\x46' +                            # 2.20 MD-ENTRY-TYPE: 'F' (Derived Sell)
        b'\x30' +                            # 2.21 SIGN: '0' (+)
        b'\x00\x01\x20\x00\x65' +            # 2.22 MD-ENTRY-PX: 12000.65 (BCD)
        b'\x00\x00\x00\x03' +                # 2.23 MD-ENTRY-SIZE: 3 (BCD)
        b'\x01'                              # 2.24 MD-PRICE-LEVEL: 1 (BCD)
    )

    body = prod_id + prod_msg_seq + calc_flag + no_md_entries + \
           entry_1 + entry_2 + entry_3 + entry_4
    
    # --- 3. CHECK-SUM ---
    content_to_check = header + body
    lrc_val = 0
    for byte in content_to_check:
        lrc_val ^= byte
    checksum = bytes([lrc_val])
    
    # --- 4. TERMINAL ---
    terminal_code = b'\x0D\x0A'
    
    return esc_code + content_to_check + checksum + terminal_code

def _build_packet(transmission_code, message_kind, channel_seq, body,
                  channel_id=1, info_time=b'\x13\x15\x40\x00\x00\x00'):
    """Wrap a message body with the 18-byte common header, XOR checksum and
    terminal code. `transmission_code`/`message_kind` are single-char strs."""
    esc_code = bytes([0x1B])
    header = (
        transmission_code.encode('ascii') +  # 1.1 TRANSMISSION-CODE
        message_kind.encode('ascii') +       # 1.2 MESSAGE-KIND
        info_time +                          # 1.3 INFORMATION-TIME (BCD 6B)
        _bcd(channel_id, 2) +                # 1.4 CHANNEL-ID (BCD)
        _bcd(channel_seq, 5) +               # 1.5 CHANNEL-SEQ (BCD)
        _bcd(1, 1) +                         # 1.6 VERSION-NO: 1 (BCD)
        _bcd(len(body), 2)                   # 1.7 BODY-LENGTH (BCD)
    )
    content = header + body
    checksum = bytes([calculate_checksum(content)])  # 3.1 CHECK-SUM: X(1)
    terminal_code = b'\x0D\x0A'                       # 4.1 TERMINAL-CODE: HEX 0D0A
    return esc_code + content + checksum + terminal_code

def _prod(prod_id):
    return prod_id.ljust(20).encode('ascii')

def _md_entry(entry_type, price, qty, level, sign='0'):
    """I083/I084-'O' style MD entry (no update_action byte)."""
    return (entry_type.encode('ascii') + sign.encode('ascii') +
            _bcd(price, 5) + _bcd(qty, 4) + _bcd(level, 1))

def _md_entry_i081(action, entry_type, price, qty, level, sign='0'):
    """I081 MD entry (leading MD-UPDATE-ACTION byte)."""
    return action.encode('ascii') + _md_entry(entry_type, price, qty, level, sign)

def build_i024(prod_id, prod_msg_seq, channel_seq, price=12000500, qty=1):
    body = (
        _prod(prod_id) + _bcd(prod_msg_seq, 5) +
        b'\x30' +                            # CALCULATED-FLAG '0'
        b'\x13\x15\x40\x00\x00\x00' +        # MATCH-TIME (BCD)
        b'\x30' + _bcd(price, 5) +           # FIRST-MATCH sign + price
        _bcd(qty, 4) +                       # FIRST-MATCH-QTY
        b'\x80' +                            # DISPLAY-ITEM: first packet, 0 repeats
        _bcd(qty, 4) + _bcd(1, 4) + _bcd(1, 4)  # totals: qty / buy cnt / sell cnt
    )
    return _build_packet('2', 'D', channel_seq, body)

def build_i025(prod_id, prod_msg_seq, channel_seq,
               day_high=12001000, day_low=11999000):
    body = (
        _prod(prod_id) + _bcd(prod_msg_seq, 5) +
        b'\x30' + _bcd(day_high, 5) +        # DAY-HIGH sign + price
        b'\x30' + _bcd(day_low, 5) +         # DAY-LOW sign + price
        b'\x13\x15\x40\x00\x00\x00'          # SHOW-TIME (BCD)
    )
    return _build_packet('2', 'E', channel_seq, body)

def build_i081(prod_id, prod_msg_seq, entries, channel_seq):
    """entries: list of (action, entry_type, price, qty, level) tuples."""
    body = _prod(prod_id) + _bcd(prod_msg_seq, 5) + _bcd(len(entries), 1)
    for action, entry_type, price, qty, level in entries:
        body += _md_entry_i081(action, entry_type, price, qty, level)
    return _build_packet('2', 'A', channel_seq, body)

def build_i083(prod_id, prod_msg_seq, entries, channel_seq, calc_flag='0'):
    """entries: list of (entry_type, price, qty, level) tuples."""
    body = (_prod(prod_id) + _bcd(prod_msg_seq, 5) +
            calc_flag.encode('ascii') + _bcd(len(entries), 1))
    for entry_type, price, qty, level in entries:
        body += _md_entry(entry_type, price, qty, level)
    return _build_packet('2', 'B', channel_seq, body)

def build_i084_o(products, channel_seq):
    """products: list of (prod_id, last_prod_msg_seq, entries) with entries as
    (entry_type, price, qty, level) tuples."""
    body = b'\x4F' + _bcd(len(products), 1)   # 'O', NO-ENTRIES
    for prod_id, last_prod_msg_seq, entries in products:
        body += _prod(prod_id) + _bcd(last_prod_msg_seq, 5) + _bcd(len(entries), 1)
        for entry_type, price, qty, level in entries:
            body += _md_entry(entry_type, price, qty, level)
    return _build_packet('4', 'C', channel_seq, body, channel_id=13)

def build_i084_a(last_seq):
    # Per spec the Refresh Begin CHANNEL-SEQ is fixed at 1.
    return _build_packet('4', 'C', 1, b'\x41' + _bcd(last_seq, 5), channel_id=13)

def build_i084_z(last_seq, channel_seq):
    return _build_packet('4', 'C', channel_seq, b'\x5A' + _bcd(last_seq, 5),
                         channel_id=13)

def _build_i084(channel_seq, body):
    """Wrap an I084 body (MESSAGE-TYPE byte + type-specific fields) with the
    18-byte common header (MESSAGE-KIND 'C'), checksum and terminal code."""
    return _build_packet('4', 'C', channel_seq, body)

def create_packet_format_I084_A_TAIFEX():
    # MESSAGE-TYPE 'A' (Refresh Begin): LAST-SEQ 9(10) PACK BCD 5B.
    # Per spec the Refresh Begin CHANNEL-SEQ is fixed at 1.
    body = b'\x41' + _bcd(1000, 5)               # 'A', LAST-SEQ=1000
    return _build_i084(channel_seq=1, body=body)

def create_packet_format_I084_O_TAIFEX():
    # MESSAGE-TYPE 'O' (Order Data): NO-ENTRIES, then per-product order book.
    product = (
        b'TXF202603           ' +                # PROD-ID X(20)
        _bcd(248, 5) +                           # LAST-PROD-MSG-SEQ 9(10)
        _bcd(2, 1) +                             # NO-MD-ENTRIES: 2
        # MD entry 1: Best Bid @ 12000.500 x10 level 1
        b'\x30' + b'\x30' + _bcd(12000500, 5) + _bcd(10, 4) + _bcd(1, 1) +
        # MD entry 2: Best Ask @ 12000.600 x8 level 1
        b'\x31' + b'\x30' + _bcd(12000600, 5) + _bcd(8, 4) + _bcd(1, 1)
    )
    body = b'\x4F' + _bcd(1, 1) + product        # 'O', NO-ENTRIES=1
    return _build_i084(channel_seq=2, body=body)

def create_packet_format_I084_Z_TAIFEX():
    # MESSAGE-TYPE 'Z' (Refresh Complete): LAST-SEQ 9(10) PACK BCD 5B.
    body = b'\x5A' + _bcd(1005, 5)               # 'Z', LAST-SEQ=1005
    return _build_i084(channel_seq=3, body=body)

def send_udp_packet(packet, ip, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.sendto(packet, (ip, port))
        print("Packet sent successfully!", flush=True)
    except Exception as e:
        print(f"Failed to send packet: {e}", flush=True)
    finally:
        sock.close()

def run_roundrobin(target_ip, realtime_port, snapshot_port):
    # (packet, dst_port): realtime kinds go to the realtime port, the I084
    # snapshot-refresh kinds (A/O/Z) go to the snapshot port — mirroring the
    # per-channel capture/processing split.
    packets = [
        (create_packet_format_I024_TAIFEX(),   realtime_port),
        (create_packet_format_I081_TAIFEX(),   realtime_port),
        (create_packet_format_I083_TAIFEX(),   realtime_port),
        (create_packet_format_I084_A_TAIFEX(), snapshot_port),
        (create_packet_format_I084_O_TAIFEX(), snapshot_port),
        (create_packet_format_I084_Z_TAIFEX(), snapshot_port),
    ]
    packet_index = 0

    while True:
        packet, port = packets[packet_index]
        print(f"Sending packet {packet_index + 1} -> {target_ip}:{port}", flush=True)
        send_udp_packet(packet, target_ip, port)
        packet_index = (packet_index + 1) % len(packets)  # Round-robin
        time.sleep(1)

def run_gap(target_ip, realtime_port, snapshot_port, prod="TXFG6"):
    """Order-book maintenance scenario: builds a book from an I083 snapshot,
    keeps it FRESH through I081/I024/I025 interleaving, simulates packet loss
    (skipped PROD-MSG-SEQ + CHANNEL-SEQ -> STALE), recovers via an I084 'O'
    snapshot, breaks the chain again, and recovers via I083. Loops forever
    with strictly increasing sequences."""
    prod_seq = 99    # last consumed PROD-MSG-SEQ
    chan_seq = 0     # last consumed realtime CHANNEL-SEQ

    def nxt():
        nonlocal prod_seq, chan_seq
        prod_seq += 1
        chan_seq += 1
        return prod_seq, chan_seq

    def skip(note):
        nonlocal prod_seq, chan_seq
        prod_seq += 1
        chan_seq += 1
        print(f"--- simulating loss of prod_seq={prod_seq} "
              f"(chan_seq={chan_seq}): {note}", flush=True)

    while True:
        steps = []

        p, c = nxt()  # I083 base snapshot -> FRESH
        steps.append(("I083 snapshot (expect FRESH)", realtime_port,
                      build_i083(prod, p, [('0', 12000500, 10, 1),
                                           ('1', 12000600, 8, 1)], c)))
        p, c = nxt()  # contiguous increment
        steps.append(("I081 new bid L2 (expect FRESH)", realtime_port,
                      build_i081(prod, p, [('0', '0', 12000400, 5, 2)], c)))
        p, c = nxt()  # trade consumes a prod seq: must NOT stale the book
        steps.append(("I024 trade (seq interleave, no stale)", realtime_port,
                      build_i024(prod, p, c)))
        p, c = nxt()  # day-high/low consumes a prod seq too
        steps.append(("I025 high/low (seq interleave, no stale)", realtime_port,
                      build_i025(prod, p, c)))
        p, c = nxt()
        steps.append(("I081 change ask qty (expect FRESH)", realtime_port,
                      build_i081(prod, p, [('1', '1', 12000600, 6, 1)], c)))

        for note, port, pkt in steps:
            print(f"Sending {note} -> {target_ip}:{port}", flush=True)
            send_udp_packet(pkt, target_ip, port)
            time.sleep(1)

        skip("book must turn STALE")
        p, c = nxt()
        pkt = build_i081(prod, p, [('1', '0', 12000500, 9, 1)], c)
        print(f"Sending I081 after loss (expect STALE) -> {target_ip}:{realtime_port}", flush=True)
        send_udp_packet(pkt, target_ip, realtime_port)
        time.sleep(1)

        p, c = nxt()
        pkt = build_i081(prod, p, [('1', '0', 12000500, 8, 1)], c)
        print(f"Sending I081 best-effort (still STALE) -> {target_ip}:{realtime_port}", flush=True)
        send_udp_packet(pkt, target_ip, realtime_port)
        time.sleep(1)

        # I084 carousel: A / O / Z. The 'O' carries the product's current
        # LAST-PROD-MSG-SEQ -> adoption clears the stale flag.
        print(f"Sending I084 A/O/Z (O expect FRESH recovery) -> {target_ip}:{snapshot_port}", flush=True)
        send_udp_packet(build_i084_a(chan_seq), target_ip, snapshot_port)
        send_udp_packet(build_i084_o([(prod, prod_seq,
                                       [('0', 12000500, 8, 1),
                                        ('1', 12000600, 6, 1)])], 2),
                        target_ip, snapshot_port)
        send_udp_packet(build_i084_z(chan_seq, 3), target_ip, snapshot_port)
        time.sleep(1)

        p, c = nxt()
        pkt = build_i081(prod, p, [('0', '1', 12000700, 3, 2)], c)
        print(f"Sending I081 post-recovery (expect FRESH) -> {target_ip}:{realtime_port}", flush=True)
        send_udp_packet(pkt, target_ip, realtime_port)
        time.sleep(1)

        skip("second loss, STALE until the I083 below")
        p, c = nxt()
        pkt = build_i081(prod, p, [('1', '1', 12000600, 5, 1)], c)
        print(f"Sending I081 after loss (expect STALE) -> {target_ip}:{realtime_port}", flush=True)
        send_udp_packet(pkt, target_ip, realtime_port)
        time.sleep(1)
        # Next loop iteration opens with an I083 -> recovery via snapshot.

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="TAIFEX packet mocker")
    ap.add_argument("--scenario", choices=["roundrobin", "gap"],
                    default="roundrobin",
                    help="roundrobin: legacy fixed packets; gap: order-book "
                         "staleness/recovery walkthrough")
    ap.add_argument("--ip", default="127.0.0.1")
    ap.add_argument("--realtime-port", type=int, default=14000)  # 即時行情 I024/I025/I081/I083
    ap.add_argument("--snapshot-port", type=int, default=14700)  # 快照更新 I084
    ap.add_argument("--prod", default="TXFG6",
                    help="PROD-ID short code for the gap scenario "
                         "(symbol + month letter A-L + year digit)")
    args = ap.parse_args()

    if args.scenario == "gap":
        run_gap(args.ip, args.realtime_port, args.snapshot_port, args.prod)
    else:
        run_roundrobin(args.ip, args.realtime_port, args.snapshot_port)