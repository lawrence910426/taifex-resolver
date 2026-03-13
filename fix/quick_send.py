import dpkt, socket, time
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
with open('futures_day_20260302_0840.pcap', 'rb') as f:
    for ts, buf in dpkt.pcap.Reader(f):
        try:
            eth = dpkt.ethernet.Ethernet(buf)
            # 取得 UDP 資料
            payload = eth.data.data.data 
            sock.sendto(payload, ('127.0.0.1', 14000))
            time.sleep(0.01)
        except: continue