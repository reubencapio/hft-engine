/// @file test_spsc_queue.cpp
/// @brief Google Test suite for the SPSC ring buffer.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "core/spsc_queue.hpp"

using hft::SPSCQueue;

// ─── Compile-time constraints ─────────────────────────────────────────────────

TEST(SPSCQueue, CapacityIsPowerOfTwo) {
    // Verified at compile time by static_assert in the class — this just
    // confirms the capacity() accessor returns the correct value.
    EXPECT_EQ((SPSCQueue<int, 8>::capacity()), 8u);
    EXPECT_EQ((SPSCQueue<int, 1024>::capacity()), 1024u);
}

// ─── Single-threaded round-trip ───────────────────────────────────────────────

TEST(SPSCQueue, SingleThreadRoundTrip) {
    SPSCQueue<int, 16> q;

    // Push until full
    for (int i = 0; i < 16; ++i) {
        ASSERT_TRUE(q.push(i)) << "push failed at i=" << i;
    }
    EXPECT_FALSE(q.push(99)) << "push on full queue should return false";

    // Pop and verify ordering
    for (int i = 0; i < 16; ++i) {
        int v = -1;
        ASSERT_TRUE(q.pop(v)) << "pop failed at i=" << i;
        EXPECT_EQ(v, i);
    }
    int v;
    EXPECT_FALSE(q.pop(v)) << "pop on empty queue should return false";
}

TEST(SPSCQueue, SizeApprox) {
    SPSCQueue<int, 8> q;
    EXPECT_EQ(q.size_approx(), 0u);
    EXPECT_TRUE(q.empty_approx());

    q.push(1);
    q.push(2);
    EXPECT_EQ(q.size_approx(), 2u);

    int v;
    q.pop(v);
    EXPECT_EQ(q.size_approx(), 1u);
}

TEST(SPSCQueue, WrapAround) {
    // Push and pop more items than the capacity to exercise wrap-around.
    SPSCQueue<int, 4> q;
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.push(round * 4 + i));
        for (int i = 0; i < 4; ++i) {
            int v;
            ASSERT_TRUE(q.pop(v));
            EXPECT_EQ(v, round * 4 + i);
        }
    }
}

// ─── Producer / consumer thread pair ─────────────────────────────────────────

TEST(SPSCQueue, ProducerConsumerOrdering) {
    constexpr int N = 1'000'000;
    SPSCQueue<int, 65536> q;

    std::atomic<bool>  done{false};
    std::vector<int>   received;
    received.reserve(static_cast<std::size_t>(N));

    // Consumer thread
    std::thread consumer([&]() {
        int v;
        while (!done.load(std::memory_order_acquire) || !q.empty_approx()) {
            if (q.pop(v)) {
                received.push_back(v);
            }
        }
        // Drain any remaining items
        while (q.pop(v)) received.push_back(v);
    });

    // Producer: push 0..N-1
    for (int i = 0; i < N; ++i) {
        while (!q.push(i)) { /* spin if full */ }
    }
    done.store(true, std::memory_order_release);
    consumer.join();

    ASSERT_EQ(static_cast<int>(received.size()), N);
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(received[static_cast<std::size_t>(i)], i) << "out-of-order at i=" << i;
    }
}

TEST(SPSCQueue, NoDataLoss) {
    constexpr int N = 500'000;
    SPSCQueue<uint64_t, 4096> q;
    uint64_t sum_produced = 0;
    uint64_t sum_consumed = 0;

    std::thread consumer([&]() {
        int received = 0;
        uint64_t v;
        while (received < N) {
            if (q.pop(v)) {
                sum_consumed += v;
                ++received;
            }
        }
    });

    for (int i = 1; i <= N; ++i) {
        uint64_t val = static_cast<uint64_t>(i);
        while (!q.push(val)) { /* spin */ }
        sum_produced += val;
    }
    consumer.join();

    EXPECT_EQ(sum_produced, sum_consumed) << "data lost in transit";
}
