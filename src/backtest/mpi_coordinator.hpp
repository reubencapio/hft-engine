#pragma once

/// @file mpi_coordinator.hpp
/// @brief MPI-based distributed parameter sweep coordinator.
///
/// Distributes the parameter grid across MPI ranks using MPI_Scatter,
/// runs an OpenMP sweep on each rank's local chunk, then gathers results
/// back to rank 0 using MPI_Gather.
///
/// Compilation:
///   This file is conditionally compiled.  Define HFT_USE_MPI to enable
///   MPI support.  Without the define, the class provides stub methods
///   that print a warning and return empty results.
///
/// Usage (launched via mpirun):
/// @code
///   mpirun -np 4 ./hft_engine --mode mpi-sweep --itch-file data.itch
/// @endcode
///
/// Data distribution strategy:
///   - Rank 0 generates the full parameter grid (N total combinations).
///   - The grid is split into world_size chunks of ~N/world_size each.
///   - Each rank receives its chunk via MPI_Scatter of raw bytes.
///   - Each rank runs OpenMP ParameterSweep::run_subset on its chunk.
///   - Results are MPI_Gathered back to rank 0.
///   - Rank 0 merges and sorts the aggregated results.

#include <cstdint>
#include <string>
#include <vector>

#include "../core/order.hpp"
#include "backtest_engine.hpp"
#include "parameter_sweep.hpp"

#ifdef HFT_USE_MPI
#include <mpi.h>
#endif

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// MPICoordinator
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Coordinates distributed parameter sweeps across MPI ranks.
///
/// Lifecycle:
///   1. init()                      — MPI_Init
///   2. run_distributed_sweep()     — scatter/compute/gather
///   3. finalize()                  — MPI_Finalize
///
/// The coordinator does not hold any persistent state between sweep runs.
/// MPI datatypes are created and freed within each run_distributed_sweep call.
class MPICoordinator {
public:
    MPICoordinator() = default;
    ~MPICoordinator() = default;

    // Non-copyable (MPI state is process-global)
    MPICoordinator(const MPICoordinator&)            = delete;
    MPICoordinator& operator=(const MPICoordinator&) = delete;
    MPICoordinator(MPICoordinator&&)                 = default;
    MPICoordinator& operator=(MPICoordinator&&)      = default;

    /// @brief Initialize MPI.
    /// @param argc  Pointer to argc from main().
    /// @param argv  Pointer to argv from main().
    /// @return true if MPI_Init succeeded (or if MPI is disabled at compile).
    bool init(int* argc, char*** argv);

    /// @brief Finalize MPI (must be called before process exit).
    void finalize();

    /// @brief Get the rank of this process [0, world_size).
    [[nodiscard]] int world_rank() const noexcept { return rank_; }

    /// @brief Get the total number of MPI ranks.
    [[nodiscard]] int world_size() const noexcept { return size_; }

    /// @brief Run the full distributed parameter sweep.
    ///
    /// Rank 0:
    ///   1. Generate full parameter grid.
    ///   2. Pad grid so it divides evenly by world_size.
    ///   3. MPI_Scatter parameter chunks.
    ///   4. Run local OpenMP sweep on own chunk.
    ///   5. MPI_Gather all SweepResults.
    ///   6. Sort and write CSV output.
    ///
    /// Other ranks:
    ///   1. Receive parameter chunk via MPI_Scatter.
    ///   2. Run local OpenMP sweep.
    ///   3. MPI_Gather results to rank 0.
    ///
    /// @param tick_data    Immutable market tick data (must be on all ranks).
    /// @param output_csv   Path to write CSV results (rank 0 only).
    /// @return             Sorted results (rank 0 only; empty on other ranks).
    std::vector<SweepResult> run_distributed_sweep(
        const std::vector<Order>& tick_data,
        const std::string& output_csv = "sweep_results.csv");

    /// @brief Write sorted results to a CSV file.
    static void write_csv(const std::vector<SweepResult>& results,
                          const std::string& filepath);

private:
    int rank_ = 0;  ///< This process's MPI rank
    int size_ = 1;  ///< Total number of MPI processes

#ifdef HFT_USE_MPI
    static void create_parameter_set_type(MPI_Datatype& type);
    static void create_sweep_result_type(MPI_Datatype& type);
#endif
};

} // namespace hft
