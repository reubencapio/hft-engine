# hft-engine

[![CI](https://github.com/reubencapio/hft-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/reubencapio/hft-engine/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![OpenMP](https://img.shields.io/badge/OpenMP-5.2-green)

A low-latency C++20 matching engine and backtesting framework built for high-frequency trading research. Designed around the same primitives used in production HFT systems: lock-free queues, RDTSC-based latency measurement, price-time priority matching, and OpenMP/MPI parallel parameter sweeps.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                          hft-engine                             │
│                                                                 │
│  ┌──────────────┐   SPSC    ┌─────────────────┐   SPSC         │
│  │     Feed     │ ────────▶ │ MatchingEngine  │ ────────▶ ...  │
│  │  Simulator / │           │  (price-time    │                 │
│  │ ITCH Parser  │           │   priority)     │                 │
│  └──────────────┘           └────────┬────────┘                │
│                                      │                          │
│                             ┌────────▼────────┐                 │
│                             │   Order Book    │                 │
│                             │  (bid/ask map)  │                 │
│                             └─────────────────┘                 │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                     Strategies                           │   │
│  │    MarketMaker (two-sided quoting, position limits)      │   │
│  │    Momentum    (trend-following, lookback window)        │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                    Backtest Layer                        │   │
│  │   BacktestEngine ──▶ ParameterSweep (OpenMP / MPI)      │   │
│  │   PnLTracker · LatencyHistogram (RDTSC) · Sharpe ratio  │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

## Key Design Decisions

| Component | Technique | Why |
|---|---|---|
| Order queues | Lock-free SPSC ring buffer | Zero contention on the hot path |
| Latency measurement | `RDTSC` intrinsic, power-of-two histogram | ~20 cycle overhead vs µs-scale `clock_gettime` |
| Parameter sweep | OpenMP `parallel for`, `thread_local` engines | No shared mutable state between threads |
| Distributed sweep | MPI (optional) | Scale across co-lo nodes |
| Metrics | Atomic relaxed increments | No fence overhead on x86 |
| Data layout | Fixed-width integer types, `alignas(64)` | Avoids false sharing, trivially copyable for MPI |

## Components

### `src/core/`
- **`spsc_queue.hpp`** — Lock-free single-producer/single-consumer ring buffer. Power-of-two capacity, cache-line-separated head/tail, acquire/release memory ordering.
- **`order_book.hpp`** — Bid/ask price levels backed by sorted maps.
- **`matching_engine.hpp`** — Price-time priority matching. Supports limit, market, and cancel orders. Two modes: synchronous (backtest) and threaded (real-time).
- **`order.hpp`** — `Order` and `Trade` POD types. Fixed-point price representation.

### `src/strategy/`
- **`market_maker`** — Two-sided quoting around mid-price, constrained by `max_position`.
- **`momentum`** — Trend-following entry triggered when price moves more than `threshold_bps` over a `lookback_ns` window.

### `src/backtest/`
- **`BacktestEngine`** — Deterministic single-threaded tick replay. Holds a `const&` to tick data; creates fresh engine/strategy state each `run()` call.
- **`ParameterSweep`** — Cartesian product grid over spread, quantity, lookback, and threshold. Each OpenMP thread owns a `thread_local BacktestEngine`. Results sorted by Sharpe ratio.
- **`MPICoordinator`** — Distributes the grid across MPI ranks; rank 0 gathers and writes CSV.

### `src/feed/`
- **`ITCHParser`** — Parses NASDAQ ITCH 5.0 binary feed files into `Order` vectors.
- **`FeedSimulator`** — Replays a tick vector into an SPSC queue at configurable speed.

### `src/metrics/`
- **`LatencyHistogram`** — RDTSC-based, 28 power-of-two buckets (1 ns – 134 ms). Calibrated against `CLOCK_MONOTONIC` at startup. Reports p50/p99/p999.
- **`PnLTracker`** — Per-fill P&L accounting (realized + unrealized), max drawdown, rolling Sharpe over a circular buffer of per-second returns.
- **`PrometheusServer`** — Exposes latency and order metrics at `localhost:9090/metrics`.

## Prerequisites

| Dependency | Version | Required |
|---|---|---|
| GCC or Clang | GCC ≥ 11, Clang ≥ 14 | Yes |
| CMake | ≥ 3.20 | Yes |
| GTest | ≥ 1.11 | For tests |
| OpenMP | ≥ 4.5 | Yes (bundled with GCC) |
| OpenMPI | any | No (`-DHFT_USE_MPI=ON`) |
| Qt6 | ≥ 6.4 | No (`-DHFT_USE_QT=ON`) |
| libnuma | any | No (auto-detected) |

**Fedora / RHEL:**
```bash
sudo dnf install gcc-c++ cmake gtest-devel libasan libubsan libtsan valgrind
```

**Ubuntu / Debian:**
```bash
sudo apt-get install build-essential cmake libgtest-dev valgrind
```

## Build

### ASAN (development default)
```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j$(nproc)
cd build-asan && ctest --output-on-failure
```

### Release (co-lo deployment)
```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j$(nproc)
```

### ThreadSanitizer (data race detection)
```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=TSan
cmake --build build-tsan -j$(nproc)
OMP_NUM_THREADS=1 ctest --test-dir build-tsan --output-on-failure
```

### UndefinedBehaviorSanitizer
```bash
cmake -B build-ubsan -DCMAKE_BUILD_TYPE=UBSan
cmake --build build-ubsan -j$(nproc)
UBSAN_OPTIONS="halt_on_error=1 print_stacktrace=1" ctest --test-dir build-ubsan --output-on-failure
```

### Valgrind
```bash
cmake -B build-valgrind -DCMAKE_BUILD_TYPE=Valgrind
cmake --build build-valgrind -j$(nproc)

for test in test_spsc_queue test_order_book test_matching_engine test_parameter_sweep; do
  valgrind --leak-check=full --track-origins=yes --error-exitcode=1 ./build-valgrind/$test
done
```

### Coverage
```bash
cmake -B build-cov -DCMAKE_BUILD_TYPE=Coverage
cmake --build build-cov -j$(nproc)
ctest --test-dir build-cov --output-on-failure
gcovr --root . --object-directory build-cov --exclude 'tests/.*' --exclude '/usr/.*' --xml coverage.xml
```

## Usage

### Quick start

```bash
# 1. Build release binary
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j$(nproc)

# 2. Run a backtest on synthetic tick data
./build-release/hft-engine --mode backtest --strategy market_maker

# 3. Run a parameter sweep and save results
./build-release/hft-engine --mode sweep --output-csv results.csv
```

The sweep produces a CSV ranked by Sharpe ratio. Open it in any spreadsheet tool to find the best parameter combination:

```
spread_ticks,order_qty,lookback_ns,threshold_bps,sharpe,max_drawdown,total_pnl,fill_rate,avg_latency_ns
2,50,500000000,10,1.823456,-120.50,3450.00,0.4231,142.3
3,100,300000000,15,1.654321,-95.20,2890.00,0.3812,138.7
...
```

| Column | Meaning |
|---|---|
| `spread_ticks` | Quote spread in price ticks (market maker) |
| `order_qty` | Order size in shares |
| `lookback_ns` | Price history window for momentum signal (nanoseconds) |
| `threshold_bps` | Minimum move to trigger entry (basis points) |
| `sharpe` | Annualised Sharpe ratio — **primary ranking metric** |
| `max_drawdown` | Worst peak-to-trough P&L loss |
| `total_pnl` | Cumulative P&L over the backtest |
| `fill_rate` | Fraction of sent orders that received fills |
| `avg_latency_ns` | Mean order-to-match latency (RDTSC-measured) |

A Sharpe above 1.0 is worth examining. Above 2.0 is strong for a synthetic data run; apply skepticism until validated on real ITCH data.

### Modes

#### `backtest` — single parameter set

Replays tick data through one strategy instance and prints a summary:

```bash
# Synthetic data (no file needed)
./build-release/hft-engine --mode backtest --strategy market_maker

# Real NASDAQ ITCH 5.0 file
./build-release/hft-engine --mode backtest \
  --itch-file data/20231103.PSX_ITCH_50 \
  --strategy momentum
```

#### `sweep` — OpenMP parameter grid

Explores the full Cartesian product of spread × qty × lookback × threshold using all available CPU cores. Each thread owns its own engine instance — no locking:

```bash
# Use all cores
./build-release/hft-engine --mode sweep --output-csv results.csv

# Limit to 4 threads
./build-release/hft-engine --mode sweep --threads 4 --output-csv results.csv
```

Runtime scales linearly with grid size. A default grid (~600 combinations) finishes in under a minute on a 8-core machine.

#### `mpi-sweep` — MPI distributed sweep

Divides the grid across MPI ranks; each rank runs its share in parallel with OpenMP. Rank 0 gathers all results, sorts by Sharpe, and writes the CSV:

```bash
# 4 MPI ranks, 1 OpenMP thread each (safe on a 4-core laptop)
mpirun -n 4 ./build-release/hft-engine \
  --mode mpi-sweep \
  --output-csv results.csv

# On a multi-node cluster (add a hostfile for real distribution)
mpirun -n 32 --hostfile hosts.txt ./build-release/hft-engine \
  --mode mpi-sweep \
  --itch-file data/20231103.PSX_ITCH_50 \
  --output-csv results.csv
```

Requires building with `-DHFT_USE_MPI=ON` and having OpenMPI installed.

#### `simulate` — live feed loop

Runs the matching engine in a real-time loop fed by the `FeedSimulator`. Useful for profiling and latency measurement. Optionally pins the engine thread to a CPU core:

```bash
# Run on any core
./build-release/hft-engine --mode simulate

# Pin to core 3 (keep core 3 isolated with isolcpus=3 in your kernel cmdline)
./build-release/hft-engine --mode simulate --cpu-core 3
```

With Prometheus enabled (`-DHFT_USE_PROMETHEUS=ON`), metrics are available at `http://localhost:9090/metrics`:

```
hft_order_latency_ns_p50 142
hft_order_latency_ns_p99 380
hft_orders_sent_total 48291
hft_fills_total 20433
hft_pnl_realised 1842.50
```

### Using real ITCH data

NASDAQ publishes historical ITCH 5.0 binary files through their [market data portal](https://itch.nasdaq.com/). Files are named by date and exchange, e.g. `20231103.PSX_ITCH_50`.

```bash
# Point any mode at a real file
./build-release/hft-engine --mode backtest \
  --itch-file /data/20231103.PSX_ITCH_50 \
  --strategy momentum
```

The parser handles message types `A` (add order), `D` (delete), `E` (executed), and `P` (trade). All other message types are skipped. Files are `mmap`'d read-only so parsing a full day's file (~10 GB) adds no heap pressure.

### Tuning strategies

**MarketMaker** parameters to focus on:
- `spread_ticks` — too tight and adverse selection kills P&L; too wide and fill rate collapses. Start at 2–5 ticks.
- `order_qty` — keep it small enough that a single fill doesn't breach your position limit.

**Momentum** parameters to focus on:
- `lookback_ns` — 100ms–1s captures intraday trends; shorter windows are noisier.
- `threshold_bps` — filters out noise. 5–20 bps is typical. Lower means more trades but more noise fills.

Run a sweep first to map the P&L surface, then backtest the top 5 combinations individually to check for overfitting.

### CLI flags

| Flag | Default | Description |
|---|---|---|
| `--mode` | `simulate` | `simulate` · `backtest` · `sweep` · `mpi-sweep` |
| `--itch-file` | *(synthetic)* | Path to NASDAQ ITCH 5.0 binary file |
| `--strategy` | `market_maker` | `market_maker` · `momentum` |
| `--threads` | OMP default | Override `OMP_NUM_THREADS` |
| `--cpu-core` | `-1` | Pin simulate-mode engine thread to this core |
| `--output-csv` | `sweep_results.csv` | Sweep results output path |

## CMake Options

| Option | Default | Description |
|---|---|---|
| `HFT_BUILD_TESTS` | `ON` | Build the GTest suite |
| `HFT_USE_MPI` | `OFF` | Link OpenMPI for distributed sweeps |
| `HFT_USE_QT` | `OFF` | Build Qt6 GUI dashboard |
| `HFT_USE_PROMETHEUS` | `OFF` | Enable Prometheus metrics server |

## CI Pipeline

Every push and PR runs the following gates in parallel on GitHub Actions:

| Job | Tool | What it catches |
|---|---|---|
| **ASAN** | GCC `-fsanitize=address` | Heap/stack overflows, use-after-free, use-after-return |
| **TSan** | GCC `-fsanitize=thread` | Data races between threads |
| **UBSan** | GCC `-fsanitize=undefined` | Signed overflow, null deref, misaligned loads |
| **Valgrind** | `memcheck` | Definite/indirect leaks, uninitialised reads, invalid frees |
| **cppcheck** | Static analysis | Null deref, division by zero, portability issues |
| **clang-tidy** | Lint | Bug-prone patterns, concurrency issues, performance anti-patterns |
| **Coverage** | `gcovr` + Codecov | Line/branch coverage trends |
| **Release** | Smoke test | Binary compiles and runs on synthetic data |

## Project Structure

```
hft-engine/
├── src/
│   ├── core/           # SPSC queue, order book, matching engine, order types
│   ├── strategy/       # MarketMaker and Momentum strategy implementations
│   ├── backtest/       # BacktestEngine, ParameterSweep, MPICoordinator
│   ├── feed/           # ITCH parser, feed simulator
│   ├── metrics/        # RDTSC histogram, PnL tracker, Prometheus server
│   └── main.cpp        # CLI entry point (simulate / backtest / sweep / mpi-sweep)
├── tests/              # GTest suites for each component
├── gui/                # Qt6 QML dashboard (optional)
├── k8s/                # Kubernetes manifests for MPI job on Kind cluster
├── .github/workflows/  # CI pipeline (ASAN, TSan, UBSan, Valgrind, coverage)
└── CMakeLists.txt
```
