/// @file main.cpp
/// @brief HFT Engine entry point — dispatches to simulate / backtest / sweep modes.
///
/// Modes:
///   --mode simulate   Live simulation with feed replay (Qt GUI if built)
///   --mode backtest   Single deterministic backtest run, prints result
///   --mode sweep      OpenMP parallel parameter sweep, single process
///   --mode mpi-sweep  MPI distributed sweep (launch with mpirun)
///
/// Example:
///   ./hft-engine --mode sweep --itch-file data/sample.itch --strategy market_maker
///   mpirun -n 4 ./hft-engine --mode mpi-sweep --itch-file data/sample.itch

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <omp.h>
#include <string>
#include <vector>

#include "backtest/backtest_engine.hpp"
#include "backtest/mpi_coordinator.hpp"
#include "backtest/parameter_sweep.hpp"
#include "core/matching_engine.hpp"
#include "core/spsc_queue.hpp"
#include "feed/feed_simulator.hpp"
#include "feed/itch_parser.hpp"
#include "metrics/prometheus_server.hpp"

namespace {

// ─── Synthetic tick generator ────────────────────────────────────────────────
// Produces a simple random walk around mid-price 1'000'000 for testing when
// no ITCH file is provided.

std::vector<hft::Order> make_synthetic_ticks(int n_ticks = 10'000) {
    std::vector<hft::Order> ticks;
    ticks.reserve(static_cast<std::size_t>(n_ticks * 6));

    int64_t  mid  = 1'000'000; // $100.00 in fixed-point (4 dp)
    uint64_t ns   = 1'000'000'000ULL;
    uint64_t oid  = 1000;

    for (int i = 0; i < n_ticks; ++i) {
        // Passive limit orders defining the BBO (1-tick inside mid)
        hft::Order bid{};
        bid.order_id     = oid++;
        bid.order_type   = hft::OrderType::Limit;
        bid.side         = hft::Side::Bid;
        bid.price        = mid - 100;
        bid.quantity     = 100 + (i % 5) * 50;
        bid.timestamp_ns = ns;
        ticks.push_back(bid);

        hft::Order ask{};
        ask.order_id     = oid++;
        ask.order_type   = hft::OrderType::Limit;
        ask.side         = hft::Side::Ask;
        ask.price        = mid + 100;
        ask.quantity     = 100 + (i % 7) * 50;
        ask.timestamp_ns = ns;
        ticks.push_back(ask);

        // Every 5 ticks: aggressive market buy sweeps the entire ask side,
        // filling resting ask orders including those placed by strategies.
        if (i > 0 && i % 5 == 0) {
            hft::Order mkt{};
            mkt.order_id     = oid++;
            mkt.order_type   = hft::OrderType::Market;
            mkt.side         = hft::Side::Bid;
            mkt.price        = 0;
            mkt.quantity     = 50'000;  // large enough to sweep all ask levels
            mkt.timestamp_ns = ns + 100;
            ticks.push_back(mkt);
        }

        // Every 7 ticks: aggressive market sell sweeps the entire bid side.
        if (i > 0 && i % 7 == 0) {
            hft::Order mkt{};
            mkt.order_id     = oid++;
            mkt.order_type   = hft::OrderType::Market;
            mkt.side         = hft::Side::Ask;
            mkt.price        = 0;
            mkt.quantity     = 50'000;
            mkt.timestamp_ns = ns + 200;
            ticks.push_back(mkt);
        }

        // Random-walk mid price by ±1 tick each step
        mid += (i % 3 == 0) ? 100 : (i % 3 == 1 ? -100 : 0);
        ns  += 1'000'000; // 1ms between updates
    }

    return ticks;
}

// ─── Argument parsing ────────────────────────────────────────────────────────

struct Args {
    std::string mode       = "simulate";
    std::string itch_file;
    std::string strategy   = "market_maker";
    std::string output_csv = "sweep_results.csv";
    int         threads    = 0;  // 0 = use OMP default
    int         cpu_core   = -1; // for simulate mode engine pinning
};

Args parse_args(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--mode"       && i + 1 < argc) a.mode       = argv[++i];
        else if (arg == "--itch-file"  && i + 1 < argc) a.itch_file  = argv[++i];
        else if (arg == "--strategy"   && i + 1 < argc) a.strategy   = argv[++i];
        else if (arg == "--output-csv" && i + 1 < argc) a.output_csv = argv[++i];
        else if (arg == "--threads"    && i + 1 < argc) a.threads    = std::stoi(argv[++i]);
        else if (arg == "--cpu-core"   && i + 1 < argc) a.cpu_core   = std::stoi(argv[++i]);
    }
    return a;
}

// ─── Load ticks ──────────────────────────────────────────────────────────────

std::vector<hft::Order> load_ticks(const std::string& itch_file) {
    if (!itch_file.empty()) {
        try {
            hft::ITCHParser parser(itch_file);
            auto ticks = parser.parse_all();
            std::fprintf(stdout, "Loaded %zu ticks from %s\n",
                         ticks.size(), itch_file.c_str());
            return ticks;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Failed to parse ITCH file: %s\n", e.what());
            std::fprintf(stderr, "Falling back to synthetic data.\n");
        }
    }
    auto ticks = make_synthetic_ticks();
    std::fprintf(stdout, "Using %zu synthetic ticks.\n", ticks.size());
    return ticks;
}

// ─── Mode: backtest ──────────────────────────────────────────────────────────

int run_backtest(const Args& args) {
    auto ticks = load_ticks(args.itch_file);

    hft::ParameterSet params;
    if (args.strategy == "momentum") {
        params.strategy_name = hft::StrategyType::Momentum;
        params.lookback_ns   = 100'000'000ULL;  // 100ms
        params.threshold_bps = 5;
    }

    hft::BacktestEngine engine(ticks);
    const hft::BacktestResult r = engine.run(params);

    std::printf("=== Backtest Result ===\n");
    std::printf("  Strategy    : %s\n", args.strategy.c_str());
    std::printf("  Total P&L   : %.2f\n", r.total_pnl);
    std::printf("  Sharpe      : %.4f\n", r.sharpe);
    std::printf("  Max Drawdown: %.2f\n", r.max_drawdown);
    std::printf("  Fill Rate   : %.4f\n", r.fill_rate);
    std::printf("  Avg Latency : %.1f ns\n", r.avg_latency_ns);
    return 0;
}

// ─── Mode: sweep ─────────────────────────────────────────────────────────────

int run_sweep(const Args& args) {
    auto ticks = load_ticks(args.itch_file);

    hft::ParameterSweep sweep;
    if (args.strategy == "momentum") {
        sweep.set_strategy(hft::StrategyType::Momentum);
        sweep.set_spread_range(1, 5, 1);
        sweep.set_lookback_range(50'000'000ULL, 500'000'000ULL, 50'000'000ULL);
    } else {
        sweep.set_spread_range(2, 10, 2);
        sweep.set_qty_range(100, 500, 100);
    }

    std::fprintf(stdout, "Running OpenMP parameter sweep...\n");
    const auto results = sweep.run(ticks);

    std::printf("=== Top 10 Results ===\n");
    std::printf("%-12s %-10s %-12s %-10s %-12s\n",
                "spread", "qty", "lookback_ns", "sharpe", "total_pnl");
    const int top = std::min(10, static_cast<int>(results.size()));
    for (int i = 0; i < top; ++i) {
        const auto& r = results[static_cast<std::size_t>(i)];
        std::printf("%-12d %-10d %-12llu %-10.4f %-12.2f\n",
                    r.params.spread_ticks,
                    r.params.order_qty,
                    static_cast<unsigned long long>(r.params.lookback_ns),
                    r.result.sharpe,
                    r.result.total_pnl);
    }

    hft::MPICoordinator::write_csv(results, args.output_csv);
    std::fprintf(stdout, "Results written to %s\n", args.output_csv.c_str());
    return 0;
}

// ─── Mode: mpi-sweep ─────────────────────────────────────────────────────────

int run_mpi_sweep(int argc, char* argv[], const Args& args) {
    hft::MPICoordinator mpi;
    mpi.init(&argc, &argv);

    auto ticks = load_ticks(args.itch_file);

    const auto results =
        mpi.run_distributed_sweep(ticks, args.output_csv);

    if (mpi.world_rank() == 0) {
        std::printf("MPI sweep complete: %zu results, written to %s\n",
                    results.size(), args.output_csv.c_str());
    }

    mpi.finalize();
    return 0;
}

// ─── Mode: simulate ──────────────────────────────────────────────────────────

int run_simulate(const Args& args) {
#ifdef HFT_USE_QT
    (void)args;
    // Qt mode: launch GUI application (handled in gui/main_window.cpp)
    std::fprintf(stderr, "Start the hft-dashboard executable for GUI mode.\n");
    return 1;
#else
    // Headless simulate: replay feed through the matching engine, print stats.
    auto ticks = load_ticks(args.itch_file);

    hft::SPSCQueue<hft::Order, 65536> order_queue;
    hft::SPSCQueue<hft::Trade, 65536> trade_queue;

    hft::MatchingEngine    engine(&order_queue, &trade_queue);
    hft::FeedSimulator     sim(ticks, order_queue, 0.0 /*max speed*/);
    hft::PrometheusServer  prom(9090);

    engine.start(args.cpu_core);
    sim.start();

    if (prom.start()) {
        std::fprintf(stdout, "Prometheus metrics at http://localhost:9090/metrics\n");
    }

    // Poll metrics every 100ms while the simulator runs.
    while (sim.running()) {
        struct timespec ts{0, 100'000'000};
        nanosleep(&ts, nullptr);
        prom.set_latency(engine.latency_histogram(), sim.orders_sent());
    }
    sim.stop();
    engine.stop();
    prom.stop();

    std::printf("Simulation complete: %llu orders sent\n",
                static_cast<unsigned long long>(sim.orders_sent()));

    const auto& hist = engine.latency_histogram();
    std::printf("Match latency — p50: %.0f ns  p99: %.0f ns  p999: %.0f ns\n",
                hist.percentile(50.0), hist.percentile(99.0), hist.percentile(99.9));
    return 0;
#endif
}

} // anonymous namespace

// ─── Entry point ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const Args args = parse_args(argc, argv);

    if (args.threads > 0) {
        omp_set_num_threads(args.threads);
    }

    if (args.mode == "backtest") {
        return run_backtest(args);
    } else if (args.mode == "sweep") {
        return run_sweep(args);
    } else if (args.mode == "mpi-sweep") {
        return run_mpi_sweep(argc, argv, args);
    } else if (args.mode == "simulate") {
        return run_simulate(args);
    } else {
        std::fprintf(stderr, "Unknown mode: %s\n", args.mode.c_str());
        std::fprintf(stderr, "Valid modes: simulate backtest sweep mpi-sweep\n");
        return 1;
    }
}
