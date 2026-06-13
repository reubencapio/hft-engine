#pragma once

// =============================================================================
// latency_histogram.hpp — RDTSC-based Latency Histogram for HFT Metrics
//
// Provides sub-microsecond latency measurement using the TSC (Time Stamp
// Counter) and a power-of-two bucketed histogram for computing percentiles.
//
// Design decisions:
//   - rdtsc() uses __rdtsc() intrinsic — a single instruction, ~20 cycles
//     overhead. Far cheaper than clock_gettime() which involves a vDSO call.
//   - TSC-to-nanosecond conversion is calibrated once at startup by measuring
//     TSC delta against CLOCK_MONOTONIC over a short spin period.
//   - Buckets use power-of-two boundaries: [0, 1), [1, 2), [2, 4), [4, 8),
//     ... up to [2^(K-2), 2^(K-1)), [2^(K-1), ∞). This covers the range
//     from ~1 ns to ~134 ms with 28 buckets — sufficient for HFT latencies
//     in the 100 ns to 100 µs range.
//   - Bucket counters are std::atomic<uint64_t> with relaxed ordering. This
//     is safe because we only need approximate counts for statistical queries;
//     exact synchronization is unnecessary. Relaxed atomics compile to plain
//     loads/stores on x86 (no fence overhead).
//   - No heap allocation — all buckets are a fixed-size array.
//   - Thread-safe for concurrent record() calls from multiple threads.
//
// Usage:
//   hft::LatencyHistogram hist;
//   hist.calibrate();  // Call once at startup
//
//   uint64_t t0 = hft::rdtsc();
//   // ... hot-path operation ...
//   uint64_t t1 = hft::rdtsc();
//   hist.record(t0, t1);
//
//   double p99 = hist.percentile(99.0);  // p99 latency in nanoseconds
// =============================================================================

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <x86intrin.h>

namespace hft {

// ---------------------------------------------------------------------------
// rdtsc — Read the Time Stamp Counter
// ---------------------------------------------------------------------------
// Returns the current TSC value. On modern x86, the TSC is invariant (runs
// at a fixed rate regardless of CPU frequency scaling) and synchronized
// across cores, so it's suitable for inter-thread latency measurement.
//
// __rdtsc() compiles to RDTSC (or RDTSCP on newer compilers with the right
// flags). The overhead is ~20 cycles, which is negligible compared to the
// operations being measured.
// ---------------------------------------------------------------------------
inline uint64_t rdtsc() noexcept {
    return __rdtsc();
}

// ---------------------------------------------------------------------------
// LatencyHistogram — Power-of-two bucketed histogram
// ---------------------------------------------------------------------------
class LatencyHistogram {
public:
    // Number of buckets. Bucket i covers [2^(i-1), 2^i) nanoseconds, except:
    //   - Bucket 0: [0, 1) ns
    //   - Bucket kNumBuckets-1: [2^(kNumBuckets-2), ∞) ns
    //
    // With 28 buckets: max boundary = 2^27 = 134,217,728 ns ≈ 134 ms.
    // This comfortably covers the 100 ns – 100 µs range of HFT latencies,
    // while also capturing outliers up to ~134 ms.
    static constexpr int kNumBuckets = 28;

    LatencyHistogram() noexcept {
        // Zero all bucket counters.
        for (int i = 0; i < kNumBuckets; ++i) {
            buckets_[i].store(0, std::memory_order_relaxed);
        }
    }

    // -----------------------------------------------------------------------
    // calibrate — Measure TSC frequency against CLOCK_MONOTONIC
    // -----------------------------------------------------------------------
    // Spins for ~10 ms, measuring both TSC ticks and wall-clock nanoseconds,
    // then computes the ratio ns_per_tsc_tick_.
    //
    // Must be called once before the first record(). Not thread-safe with
    // concurrent record() calls — call it during single-threaded init.
    //
    // Why 10 ms? Short enough to not delay startup, long enough for the
    // ratio to converge to a stable value (TSC granularity on modern CPUs
    // is ~0.3 ns/tick @ 3 GHz).
    // -----------------------------------------------------------------------
    void calibrate() noexcept {
        constexpr uint64_t kCalibrationNs = 10'000'000ULL; // 10 ms

        struct timespec ts_start{};
        struct timespec ts_end{};

        // Serialize before reading TSC to get a clean measurement.
        // The _mm_lfence() ensures all prior instructions complete before RDTSC.
        _mm_lfence();
        const uint64_t tsc_start = rdtsc();
        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        // Spin-wait for the calibration period.
        uint64_t wall_ns = 0;
        do {
            clock_gettime(CLOCK_MONOTONIC, &ts_end);
            wall_ns = static_cast<uint64_t>(
                (ts_end.tv_sec - ts_start.tv_sec) * 1'000'000'000ULL +
                (ts_end.tv_nsec - ts_start.tv_nsec));
        } while (wall_ns < kCalibrationNs);

        _mm_lfence();
        const uint64_t tsc_end = rdtsc();
        const uint64_t tsc_delta = tsc_end - tsc_start;

        // Compute nanoseconds per TSC tick.
        // Guard against division by zero (shouldn't happen on real hardware).
        if (tsc_delta > 0) {
            ns_per_tsc_tick_ = static_cast<double>(wall_ns) /
                               static_cast<double>(tsc_delta);
        }

        calibrated_ = true;
    }

    // -----------------------------------------------------------------------
    // record — Record a latency sample from TSC start/end values
    // -----------------------------------------------------------------------
    // Converts the TSC delta to nanoseconds using the calibrated ratio, then
    // increments the appropriate bucket counter.
    //
    // Thread-safe: uses relaxed atomic increment. On x86, this compiles to
    // a single LOCK XADD instruction — no fence, no CAS loop.
    //
    // Hot path: ~5 instructions (subtract, multiply, clz, load, add, store).
    // -----------------------------------------------------------------------
    void record(uint64_t start_tsc, uint64_t end_tsc) noexcept {
        const uint64_t tsc_delta = end_tsc - start_tsc;
        const uint64_t latency_ns = static_cast<uint64_t>(
            static_cast<double>(tsc_delta) * ns_per_tsc_tick_);

        const int bucket = bucket_index(latency_ns);
        buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
        total_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // percentile — Compute the p-th percentile latency in nanoseconds
    // -----------------------------------------------------------------------
    // Walks the buckets in order, accumulating counts until the cumulative
    // count exceeds p% of total. Returns the upper bound of that bucket.
    //
    // Not hot-path — called during reporting, so allocations / divisions
    // are acceptable here.
    //
    // @param p  Percentile value in [0, 100], e.g. 50.0 for median,
    //           99.0 for p99, 99.9 for p999.
    // @return   Upper bound of the bucket containing the p-th percentile,
    //           in nanoseconds. Returns 0 if no samples recorded.
    // -----------------------------------------------------------------------
    [[nodiscard]] double percentile(double p) const noexcept {
        const uint64_t total = total_count_.load(std::memory_order_relaxed);
        if (total == 0) {
            return 0.0;
        }

        // Number of samples that must be at or below the percentile threshold.
        const double threshold = (p / 100.0) * static_cast<double>(total);
        uint64_t cumulative = 0;

        for (int i = 0; i < kNumBuckets; ++i) {
            cumulative += buckets_[i].load(std::memory_order_relaxed);
            if (static_cast<double>(cumulative) >= threshold) {
                return bucket_upper_bound(i);
            }
        }

        // Should not reach here, but return the max bucket bound.
        return bucket_upper_bound(kNumBuckets - 1);
    }

    // -----------------------------------------------------------------------
    // Accessors (for testing and reporting)
    // -----------------------------------------------------------------------

    /// @brief Total number of recorded samples.
    [[nodiscard]] uint64_t count() const noexcept {
        return total_count_.load(std::memory_order_relaxed);
    }

    /// @brief Count in a specific bucket.
    [[nodiscard]] uint64_t bucket_count(int idx) const noexcept {
        if (idx < 0 || idx >= kNumBuckets) return 0;
        return buckets_[idx].load(std::memory_order_relaxed);
    }

    /// @brief Whether calibrate() has been called.
    [[nodiscard]] bool is_calibrated() const noexcept {
        return calibrated_;
    }

    /// @brief The calibrated TSC-to-ns ratio.
    [[nodiscard]] double tsc_to_ns_ratio() const noexcept {
        return ns_per_tsc_tick_;
    }

    /// @brief Reset all counters to zero.
    void reset() noexcept {
        for (int i = 0; i < kNumBuckets; ++i) {
            buckets_[i].store(0, std::memory_order_relaxed);
        }
        total_count_.store(0, std::memory_order_relaxed);
    }

private:
    // -----------------------------------------------------------------------
    // bucket_index — Map a nanosecond latency to a bucket index
    // -----------------------------------------------------------------------
    // Uses bit-scan (count leading zeros) to find the highest set bit,
    // which gives us the power-of-two bucket in O(1).
    //
    //   latency_ns   bucket
    //   ──────────   ──────
    //   0            0
    //   1            1        [1, 2)
    //   2–3          2        [2, 4)
    //   4–7          3        [4, 8)
    //   ...
    //   2^26–2^27-1  27       [2^26, 2^27)
    //   ≥ 2^27       27       [2^27, ∞)  (overflow bucket)
    // -----------------------------------------------------------------------
    [[nodiscard]] static int bucket_index(uint64_t latency_ns) noexcept {
        if (latency_ns == 0) return 0;

        // __builtin_clzll returns the number of leading zero bits.
        // For a 64-bit value, the position of the highest set bit is
        // (63 - clz), which equals floor(log2(latency_ns)).
        // We add 1 because bucket 0 is reserved for latency_ns == 0.
        const int bit = 63 - __builtin_clzll(latency_ns);
        const int idx = bit + 1;

        // Clamp to the last bucket.
        return (idx < kNumBuckets) ? idx : (kNumBuckets - 1);
    }

    // -----------------------------------------------------------------------
    // bucket_upper_bound — Upper boundary of a bucket in nanoseconds
    // -----------------------------------------------------------------------
    [[nodiscard]] static double bucket_upper_bound(int idx) noexcept {
        if (idx <= 0) return 1.0;
        if (idx >= kNumBuckets - 1) {
            // Last bucket: return its lower bound as a conservative estimate.
            return static_cast<double>(1ULL << (kNumBuckets - 2));
        }
        return static_cast<double>(1ULL << idx);
    }

    // -----------------------------------------------------------------------
    // Member data
    // -----------------------------------------------------------------------

    // Bucket counters — relaxed atomics, no false-sharing concern because
    // they are all in the same array (shared write pattern is rare;
    // typically only one thread records at a time per histogram instance).
    std::atomic<uint64_t> buckets_[kNumBuckets]{};

    // Total sample count — separate atomic for O(1) count() queries.
    std::atomic<uint64_t> total_count_{0};

    // Calibration ratio: multiply TSC ticks by this to get nanoseconds.
    // Written once by calibrate(), then read-only. No atomic needed because
    // calibrate() is called during single-threaded init.
    double ns_per_tsc_tick_ = 0.3;  // Sensible default: ~3.3 GHz CPU

    // Flag to track whether calibration has been performed.
    bool calibrated_ = false;
};

} // namespace hft
