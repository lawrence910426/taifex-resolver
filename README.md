# taifex-resolver

For those operating in IDC/Colo environments, directly consuming UDP packets from the Taiwan Futures Exchange (TAIFEX) can be significantly faster than using traditional APIs to fetch market data. To facilitate this, a high-performance library is required to parse and decode these binary packets efficiently.

This utility is designed to handle TAIFEX real-time market data, focusing on **I024 (Trade)**, **I081 (Incremental Update)**, and **I083 (Snapshot)** formats.

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

## Usage (C/C++)

```
#include "parser.h"
#include <iostream>

void handle_packet(const Packet& packet) {
    // Print basic information from the TAIFEX packet
    std::cout << "Received TAIFEX Packet:" << std::endl;
    std::cout << "Product ID: " << packet.prod_id << std::endl;
    std::cout << "Message Type: " << packet.msg_type << std::endl;
    std::cout << "Message Seq: " << packet.msg_seq << std::endl;
}

int main() {
    const int port = 14000;
    Parser parser;
    parser.start_loop(port, handle_packet);
    parser.end_loop();
    return 0;
}

Refer to our [example](./example/taifex_resolver_interface.cc).
```

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
./taifex_resolver_cpp -port 14000
```

### Offline Data Replay

For development and logic verification without a live feed, you can use the utility in the fix/ directory to simulate traffic from pcap sources.

```
cd fix
python3 quick_send.py
```