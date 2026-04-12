# NanoTrade

A low-latency trading system built in C++20, featuring a matching engine, order book, market data distribution, and algorithmic trading strategies with sub-microsecond order processing.

## Overview

NanoTrade is a full-stack electronic trading system that includes:

- **Matching Engine** - limit order book with price-time priority matching
- **Order Book** - efficient order management with O(1) operations using custom memory pools
- **Market Data Publisher** - multicast-based real-time market data distribution
- **Trading Strategies** - market making and liquidity taking with risk management
- **Backtesting Infrastructure** - historical replay and strategy evaluation (in progress)

## Architecture

```
                        ┌──────────────────────────────────┐
                        │           Exchange               │
                        │                                  │
Market Data  ◄──────────┤  Matching Engine                 │
(Multicast)             │    └─ Order Book (per ticker)    │
                        │    └─ FIFO Sequencer             │
                        │                                  │
                        │  Market Data Publisher           │
                        │  Snapshot Synthesizer            │
                        │  Order Server (TCP)              │
                        └──────────┬───────────────────────┘
                                   │
                          TCP / Multicast
                                   │
                        ┌──────────▼───────────────────────┐
                        │        Trading Client            │
                        │                                  │
                        │  Market Data Consumer            │
                        │    └─ Market Order Book          │
                        │    └─ Feature Engine             │
                        │                                  │
                        │  Strategy Engine                 │
                        │    └─ Market Maker               │
                        │    └─ Liquidity Taker            │
                        │    └─ Order Manager              │
                        │    └─ Risk Manager               │
                        │    └─ Position Keeper            │
                        │                                  │
                        │  Order Gateway (TCP)             │
                        └──────────────────────────────────┘
```

## Key Low-Latency Techniques

| Technique | Implementation | Purpose |
|---|---|---|
| Lock-free queues | `common/lf_queue.h` | Inter-thread communication without mutex overhead |
| Custom memory pool | `common/mem_pool.h` | Pre-allocated memory to avoid malloc/new latency |
| CPU affinity | `common/thread_utils.h` | Pin threads to specific cores to reduce context switches |
| Kernel-level I/O | `common/tcp_server.cpp` | epoll for efficient socket polling |
| Cache-friendly data | Packed structs, array-based order book | Minimize cache misses on hot paths |
| RDTSC timing | `common/perf_utils.h` | CPU cycle-level latency measurement |
| Async logging | `common/logging.h` | Non-blocking log writes to avoid I/O stalls on hot paths |

## Project Structure

```
├── common/                  # Core infrastructure
│   ├── lf_queue.h          # Lock-free SPSC queue
│   ├── mem_pool.h          # Fixed-size memory pool allocator
│   ├── thread_utils.h      # Thread creation with CPU affinity
│   ├── logging.h           # Low-latency async logger
│   ├── tcp_server.h/cpp    # TCP server with epoll
│   ├── mcast_socket.h/cpp  # UDP multicast socket
│   ├── time_utils.h        # Nanosecond timestamp utilities
│   └── perf_utils.h        # RDTSC cycle counter
│
├── exchange/                # Exchange-side components
│   ├── matcher/
│   │   ├── matching_engine.h/cpp    # Core matching logic
│   │   ├── me_order_book.h/cpp      # Limit order book
│   │   └── me_order.h/cpp           # Order representation
│   ├── market_data/
│   │   ├── market_data_publisher.h/cpp
│   │   └── snapshot_synthesizer.h/cpp
│   ├── order_server/
│   │   ├── order_server.h/cpp
│   │   └── fifo_sequencer.h         # Time-ordered request processing
│   └── exchange_main.cpp
│
├── trading/                 # Trading client components
│   ├── strategy/
│   │   ├── trade_engine.h/cpp       # Strategy orchestrator
│   │   ├── market_maker.h/cpp       # Market making strategy
│   │   ├── liquidity_taker.h/cpp    # Liquidity taking strategy
│   │   ├── order_manager.h/cpp      # Order lifecycle management
│   │   ├── risk_manager.h/cpp       # Pre-trade risk checks
│   │   ├── position_keeper.h        # Position and PnL tracking
│   │   └── market_order_book.h/cpp  # Client-side order book
│   ├── market_data/
│   │   └── market_data_consumer.h/cpp
│   ├── order_gw/
│   │   └── order_gateway.h/cpp
│   └── trading_main.cpp
│
├── benchmarks/              # Performance benchmarks
│   ├── logger_benchmark.cpp
│   ├── release_benchmark.cpp
│   └── hash_benchmark.cpp
│
└── scripts/                 # Build and run scripts
    ├── build.sh
    ├── run_exchange_and_clients.sh
    ├── run_clients.sh
    └── run_benchmarks.sh
```

## Building

### Prerequisites

- Linux (Ubuntu 22.04 recommended)
- C++20 compatible compiler (g++)
- CMake >= 3.10
- Ninja build system

```bash
sudo apt install -y cmake ninja-build g++
```

### Build

```bash
bash scripts/build.sh
```

This builds both Release and Debug targets.

## Running

### Start the exchange

```bash
./cmake-build-release/exchange_main &
sleep 10  # wait for exchange to initialize
```

### Start trading clients

```bash
# Market maker client
./cmake-build-release/trading_main 1 MAKER \
  100 0.6 150 300 -100 \
  60 0.6 150 300 -100 \
  150 0.5 250 600 -100 \
  200 0.4 500 3000 -100 \
  1000 0.9 5000 4000 -100 \
  300 0.8 1500 3000 -100 \
  50 0.7 150 300 -100 \
  100 0.3 250 300 -100 &

# Random order client
./cmake-build-release/trading_main 5 RANDOM &
```

### One-command run

```bash
bash scripts/run_exchange_and_clients.sh
```

### Run benchmarks

```bash
./cmake-build-release/logger_benchmark
./cmake-build-release/release_benchmark
./cmake-build-release/hash_benchmark
```

### Stop the system

```bash
pkill exchange_main
pkill trading_main
```

## Output

Results are written to log files in the project directory:

| Log File | Contents |
|---|---|
| `exchange_matching_engine.log` | Order matching, fills, RDTSC latency data |
| `exchange_order_server.log` | Client connections, order routing |
| `exchange_market_data_publisher.log` | Market data publication events |
| `trading_engine_*.log` | Strategy decisions, positions, PnL |
| `trading_order_gateway_*.log` | Order submission and responses |

## Deployment

Deployed on AWS EC2 (Ubuntu 22.04, x86_64) to leverage Linux-native low-latency features:

- `epoll` for kernel-level I/O multiplexing
- `pthread_setaffinity_np` for CPU core pinning
- `rdtsc` for CPU cycle-level latency measurement

## Roadmap

- [x] Core trading system (matching engine, order book, strategies)
- [x] AWS deployment
- [ ] Historical data replay engine
- [ ] Backtesting with fill simulation and PnL tracking
- [ ] Custom strategy: order book imbalance
- [ ] Performance benchmarks with latency analysis
- [ ] Binance historical data integration

## Acknowledgments

Based on *Building Low-Latency Applications with C++* by Sourav Ghosh. Extended with backtesting infrastructure, additional strategies, and historical data replay.
