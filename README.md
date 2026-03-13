# taifex-resolver

`taifex-resolver` is a high-performance C++ utility designed to parse Taiwan Futures Exchange (TAIFEX) binary market data packets. It supports **I024 (Trade)**, **I081 (Incremental Update)**, and **I083 (Snapshot)** formats.

---

## Quick Start

### 1. Build the C++ Resolver
Run the following command in the project root directory. The **-O3** optimization flag is enabled to ensure maximum performance for high-frequency market data processing, linking the system thread library.

* **Build Command:**
    ```bash
    g++ -O3 -Wall -std=c++17 \
        -I ./include \
        example/taifex_resolver_interface.cc \
        src/parser.cc \
        -o example/taifex_resolver_app \
        -lpthread
    ```

### 2. Run the Service
Once compiled, start the resolver to listen on a specific UDP port (default is 14000). The program will run continuously until a termination signal (Ctrl+C) is received.

* **Execution Command:**
    ```bash
    ./example/taifex_resolver_app -port 14000
    ```

### 3. Extract and Replay Data (Offline)
The utility located in the `fix/` directory extracts data from **offline pcap files** and simulates UDP traffic towards the resolver. This is ideal for offline backtesting and logic verification during development.

* **Execution Path:**
    ```bash
    cd fix
    python quick_send.py
    ```