/// @file mpi_coordinator.cpp
/// @brief MPI distributed parameter sweep coordinator.

#include "mpi_coordinator.hpp"

#include <algorithm>
#include <cstdio>

namespace hft {

// ─── Lifecycle ────────────────────────────────────────────────────────────────

bool MPICoordinator::init(int* argc, char*** argv) {
#ifdef HFT_USE_MPI
    int provided;
    MPI_Init_thread(argc, argv, MPI_THREAD_FUNNELED, &provided);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank_);
    MPI_Comm_size(MPI_COMM_WORLD, &size_);
#else
    (void)argc; (void)argv;
    rank_ = 0;
    size_ = 1;
#endif
    return true;
}

void MPICoordinator::finalize() {
#ifdef HFT_USE_MPI
    MPI_Finalize();
#endif
}

// ─── Distributed sweep ────────────────────────────────────────────────────────

std::vector<SweepResult> MPICoordinator::run_distributed_sweep(
    const std::vector<Order>& tick_data,
    const std::string& output_csv)
{
#ifdef HFT_USE_MPI
    // ── Rank 0: build and pad the full grid ──────────────────────────────────
    std::vector<ParameterSet> full_grid;
    int total = 0;

    if (rank_ == 0) {
        ParameterSweep sweep;
        full_grid = sweep.generate_grid();
        total = static_cast<int>(full_grid.size());
        // Pad so grid divides evenly among all ranks.
        while (total % size_ != 0) {
            full_grid.push_back(full_grid.back());
            ++total;
        }
    }

    // Broadcast total so all ranks can allocate correctly.
    MPI_Bcast(&total, 1, MPI_INT, 0, MPI_COMM_WORLD);

    const int chunk = total / size_;
    std::vector<ParameterSet> local_chunk(static_cast<std::size_t>(chunk));

    MPI_Scatter(
        (rank_ == 0) ? full_grid.data() : nullptr,
        static_cast<int>(chunk * sizeof(ParameterSet)), MPI_BYTE,
        local_chunk.data(),
        static_cast<int>(chunk * sizeof(ParameterSet)), MPI_BYTE,
        0, MPI_COMM_WORLD);

    // ── Each rank runs its OpenMP sub-sweep ──────────────────────────────────
    ParameterSweep sweep;
    auto local_results = sweep.run_subset(tick_data, local_chunk);
    local_results.resize(static_cast<std::size_t>(chunk));  // ensure exact count

    // ── Gather to rank 0 ─────────────────────────────────────────────────────
    std::vector<SweepResult> all_results;
    if (rank_ == 0) all_results.resize(static_cast<std::size_t>(total));

    MPI_Gather(
        local_results.data(),
        static_cast<int>(chunk * sizeof(SweepResult)), MPI_BYTE,
        (rank_ == 0) ? all_results.data() : nullptr,
        static_cast<int>(chunk * sizeof(SweepResult)), MPI_BYTE,
        0, MPI_COMM_WORLD);

    if (rank_ == 0) {
        std::sort(all_results.begin(), all_results.end(),
            [](const SweepResult& a, const SweepResult& b) {
                return a.result.sharpe > b.result.sharpe;
            });
        if (!output_csv.empty()) write_csv(all_results, output_csv);
        return all_results;
    }
    return {};

#else
    // ── Non-MPI fallback: single-process OpenMP sweep ────────────────────────
    ParameterSweep sweep;
    auto results = sweep.run(tick_data);
    if (!output_csv.empty()) write_csv(results, output_csv);
    return results;
#endif
}

// ─── CSV output ───────────────────────────────────────────────────────────────

void MPICoordinator::write_csv(const std::vector<SweepResult>& results,
                               const std::string& filepath)
{
    std::FILE* f = std::fopen(filepath.c_str(), "w");
    if (!f) return;

    std::fprintf(f,
        "spread_ticks,order_qty,lookback_ns,threshold_bps,"
        "sharpe,max_drawdown,total_pnl,fill_rate,avg_latency_ns\n");

    for (const auto& r : results) {
        std::fprintf(f, "%d,%d,%llu,%d,%.6f,%.2f,%.2f,%.4f,%.1f\n",
            r.params.spread_ticks,
            r.params.order_qty,
            static_cast<unsigned long long>(r.params.lookback_ns),
            r.params.threshold_bps,
            r.result.sharpe,
            r.result.max_drawdown,
            r.result.total_pnl,
            r.result.fill_rate,
            r.result.avg_latency_ns);
    }

    std::fclose(f);
}

} // namespace hft
