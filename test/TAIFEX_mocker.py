import socket
import time

def calculate_checksum(data):
    """Calculate the XOR checksum of the given data."""
    checksum = 0
    for byte in data:
        checksum ^= byte
    return checksum

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

def send_udp_packet(packet, ip, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.sendto(packet, (ip, port))
        print("Packet sent successfully!", flush=True)
    except Exception as e:
        print(f"Failed to send packet: {e}", flush=True)
    finally:
        sock.close()

if __name__ == "__main__":
    target_ip = "127.0.0.1"
    target_port = 14000

    packets = [
         create_packet_format_I024_TAIFEX(), create_packet_format_I081_TAIFEX(), create_packet_format_I083_TAIFEX()
    ]
    packet_index = 0

    while True:
        print(f"Sending packet {packet_index + 1}", flush=True)
        send_udp_packet(packets[packet_index], target_ip, target_port)
        packet_index = (packet_index + 1) % len(packets)  # Round-robin
        time.sleep(1)