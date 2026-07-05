# taifex-resolver

For those operating in IDC/Colo environments, directly consuming UDP packets from the Taiwan Futures Exchange (TAIFEX) can be significantly faster than using traditional APIs to fetch market data. To facilitate this, a high-performance library is required to parse and decode these binary packets efficiently.

This utility is designed to handle TAIFEX real-time market data, focusing on **I024 (Trade)**, **I025 (Day High/Low)**, **I081 (Incremental Update)**, **I083 (Snapshot)** and **I084 (Snapshot Refresh)** formats.

Two usage modes are available and can be combined freely:

- **Native `on_` mode** — register raw packet callbacks via `start_loop`; you get every decoded `I0XXPacket` as-is.
- **Wrapped `handle_` mode** — attach an `OrderBookManager` that maintains the current per-instrument order book (with packet-loss detection and snapshot recovery) and fires your `handle_futopt_order_book(order_book)` callback whenever a book changes. See [Maintained Order Book](#maintained-order-book-wrapped-handle_-mode).

---

## Prerequisites and dependencies

```
sudo apt install python3 python3-pip python3-dev build-essential cmake -y 
```

---

## Build and Install

Run the following commands in the project root directory. The build process utilizes CMake to ensure all dependencies and threading libraries are correctly linked.

```
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

---

## Usage (C/C++) — native `on_` mode

```cpp
#include "parser.h"
#include <iostream>

// One handler per message type — pass nullptr for types you don't need.
// Full implementations: example/taifex_resolver_interface.cc.
void on_trade_match(const I024_Packet& pkt) {
    std::cout << "I024 " << pkt.prod_id << " seq=" << pkt.prod_msg_seq << std::endl;
}
void on_day_high_low(const I025_Packet& pkt) {
    std::cout << "I025 " << pkt.prod_id << " high=" << pkt.day_high_price
              << " low=" << pkt.day_low_price << std::endl;
}
void on_incremental(const I081_Packet& pkt) {
    std::cout << "I081 " << pkt.prod_id << " seq=" << pkt.prod_msg_seq
              << " entries=" << (int)pkt.no_md_entries << std::endl;
}
void on_snapshot(const I083_Packet& pkt) {
    std::cout << "I083 " << pkt.prod_id << " seq=" << pkt.prod_msg_seq << std::endl;
}
void on_refresh(const I084_Packet& pkt) {
    std::cout << "I084 type=" << pkt.message_type
              << " products=" << pkt.products.size() << std::endl;
}

int main() {
    // Realtime data (I024/I025/I081/I083) and the I084 snapshot carousel ride
    // different multicast channels/ports — one parser per port.
    TaifexParser rt, snap;
    rt.start_loop(14000,
                  on_trade_match,   // I024
                  on_day_high_low,  // I025
                  on_incremental,   // I081
                  on_snapshot,      // I083
                  nullptr);         // I084 never arrives on the realtime port
    snap.start_loop(14700,
                    nullptr, nullptr, nullptr, nullptr,  // realtime kinds never arrive here
                    on_refresh);                         // I084
    // ... run ...
    rt.end_loop();
    snap.end_loop();
    return 0;
}
```

Refer to our [example](./example/taifex_resolver_interface.cc).

---

## Maintained Order Book (wrapped `handle_` mode)

`OrderBookManager` (see [include/order_book.h](./include/order_book.h)) maintains the current 5-level order book per instrument — regular and derived (implied) sides — built from **I083** snapshots plus **I081** increments, and recovered through the **I084** snapshot carousel. Register a callback per instrument (or a wildcard) and it fires with a self-consistent copy of the book on every change, including stale-flag transitions.

Because realtime data (I024/I025/I081/I083, port 14000/4000) and the I084 carousel (port 14700/4700) ride **different multicast channels**, a live deployment uses two `TaifexParser` instances feeding one shared manager:

```
realtime parser (14000)  ─┐
                          ├──►  OrderBookManager ──► handle_futopt_order_book(order_book)
snapshot parser (14700)  ─┘        (thread-safe)
```

### Loss detection and the stale flag

TAIFEX numbers messages two ways: `CHANNEL-SEQ` (per channel, all message kinds) and `PROD-MSG-SEQ` (per product, shared across I024/I025/I081/I083 — a trade legitimately advances a product's serial between two book updates). The manager tracks both:

| Situation | Effect |
|---|---|
| Product serial contiguous (`seq == last+1`) | Clears channel-gap *suspicion* only (proves the loss didn't concern this product). A chain already broken by a product-serial gap stays stale until a snapshot re-bases it |
| Product serial gap (`seq > last+1`) | Book **stale**; increments still applied best-effort, callbacks keep firing with `is_stale=true` |
| `CHANNEL-SEQ` gap on a realtime channel (incl. gaps revealed by I001 heartbeats) | All books **stale** (suspect) until each product proves contiguity or re-bases from a snapshot |
| Normal I083, or an I084 `'O'` product entry, with `seq >= last` | Book adopted wholesale, stale cleared |
| Trial I083 (`calculated_flag='1'`) | Serial tracked (it consumes one), content ignored |
| Duplicate/retransmit (`seq <= last`) | Dropped |
| I002 sequence reset | All books and trackers cleared (per spec) |

**No-buffering caveat:** increments received between an I084 snapshot's generation and its adoption are discarded by the wholesale adoption. If the product changed inside that window, the next increment shows a gap and the book goes stale again until the next carousel cycle (or an I083). Quiet products converge in one cycle; very active ones may take a few.

### C++

```cpp
#include "parser.h"
#include "order_book.h"

void handle_futopt_order_book(const OrderBook& book) {
    // book.is_stale, book.last_prod_msg_seq, book.bids/asks/
    // derived_bids/derived_asks (index 0 = best level), book.has_snapshot.
    // The reference is valid ONLY during this call — one shared snapshot per
    // delivery event. Copy the OrderBook explicitly if you need to keep it.
}

int main() {
    OrderBookManager mgr;
    // PROD-ID short code: symbol + month letter (A=Jan..L=Dec) + year digit,
    // e.g. TXFG6 = TXF July 2026.
    mgr.register_callback("TXFG6", handle_futopt_order_book);
    // or every instrument: mgr.register_callback_all(handle_futopt_order_book);

    TaifexParser rt, snap;
    rt.set_order_book_manager(&mgr);    // realtime: I024/I025/I081/I083
    snap.set_order_book_manager(&mgr);  // snapshot: I084 carousel

    // Wrapped mode only: no raw callbacks here. The native on_ handlers from
    // the first example can be passed alongside — both modes coexist, and the
    // manager is fed before the raw callbacks fire.
    rt.start_loop(14000, nullptr, nullptr, nullptr, nullptr, nullptr);
    snap.start_loop(14700, nullptr, nullptr, nullptr, nullptr, nullptr);
    // ... run ... then end_loop() on both parsers.
}
```

Python bindings expose the same API — see [Usage (Python)](#usage-python) at the end of this document.

Notes:

- Real PROD-IDs are short codes: symbol + month letter (A=Jan..L=Dec) + year digit — `TXFG6`, `MXFG6`, `TMFG6`, `TXO18000G6`, … Registration keys are matched with trailing padding trimmed; the delivered `book.prod_id` keeps the raw padded wire form.
- Each delivery event copies the book exactly once and passes it to every registered callback by `const&` — the reference is valid only during the call, so callbacks must copy explicitly to retain it.
- Callbacks run on the receive threads — keep them fast. With two feed threads, deliveries for one product may occasionally interleave out of chronological order across threads; every delivered copy is internally consistent.
- Feeding the manager manually from raw callbacks (`mgr.on_i081(pkt)`, …) works too, but misses I001/I002 and unparsed message kinds, so channel-level gap detection degrades — prefer `set_order_book_manager`.

---

## Testing

Navigate to the root directory of this repository and execute the following commands:

```
cd test
bash bash.sh
```

This script initiates the test suite using Docker to ensure a clean environment for network simulation.

### Test Setup

The test environment utilizes two main components:
1. Parser Container: Runs the resolver to decode incoming TAIFEX UDP packets.
2. Mocker (TAIFEX_mocker.py): Simulates the exchange by replaying packets towards the parser.

You should go into the Docker container to run the test and observe the real-time decoding.

### Run the C++ example

Run the C++ example in standard listening mode. In a separate terminal, you can follow the logs: tail -f build/logger/taifex_parser.log.

```
cd build
./taifex_resolver_cpp -port 14000 -snapshot-port 14700
```

Book transitions are also echoed to stdout as `[BOOK][FRESH]` / `[BOOK][STALE]` lines.

### Order-book gap/recovery walkthrough

`--scenario gap` drives the maintained order book through its full lifecycle (build → interleaved trades → simulated packet loss → stale best-effort → I084 recovery → second loss → I083 recovery):

```
# terminal 1
./build/taifex_resolver_cpp -port 14000 -snapshot-port 14700
# terminal 2
python3 test/TAIFEX_mocker.py --scenario gap
```

### C++ unit tests

```
cd build && cmake .. && make -j$(nproc)
./order_book_test        # or: ctest --output-on-failure
```

### Multicast

When both `-multicast` and `-iface` flags are provided, the parser will automatically join the specified multicast group on the given network interface. No additional setup is required — the IGMP join is handled internally.

```
./taifex_resolver_cpp -port 14000 -multicast <multicast_group_ip> -iface <interface_ip>
```

For example, to listen on interface `127.0.0.1` for multicast group `225.0.140.140`:

```
./taifex_resolver_cpp -port 14000 -multicast 225.0.140.140 -iface 127.0.0.1 -port 14000
```

### Offline Data Replay

For development and logic verification without a live feed, you can use the utility in the helpers/ directory to simulate traffic from pcap sources.

```
cd helpers
python3 send_pcap_without_multicast.py
```

---

## Usage (Python)

The pybind11 module `taifex_udp_resolver` exposes the same API as C++ (built via `pip install .`, which drives CMake with `-DTAIFEX_BUILD_PYTHON=ON`). Both modes work identically; the maintained order book looks like this:

```python
import taifex_udp_resolver as t

mgr = t.OrderBookManager()

def handle_futopt_order_book(book):
    tag = "STALE" if book.is_stale else "FRESH"
    print(book.prod_id.strip(), tag, book.last_prod_msg_seq,
          [(l.price, l.quantity) for l in book.bids if l.valid])

mgr.register_callback("TXFG6", handle_futopt_order_book)

rt, snap = t.Parser(), t.Parser()
rt.set_order_book_manager(mgr)
snap.set_order_book_manager(mgr)
# Same positional callbacks as C++, in message-ID order (cb_i024, cb_i025,
# cb_i081, cb_i083, cb_i084); None = no raw callback for that type.
rt.start_loop(14000, None, None, None, None, None)
snap.start_loop(14700, None, None, None, None, None)
# mgr.get_book("TXFG6") returns a copy (or None); mgr.product_ids() lists books.
```

Python-specific notes:

- Callbacks run on the receive threads; pybind acquires the GIL automatically. Keep them fast.
- The transient-reference delivery contract does not affect Python: pybind materialises a fresh `OrderBook` object at the boundary, so the object handed to your callback is yours to keep.