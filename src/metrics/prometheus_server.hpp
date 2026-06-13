#pragma once

/// @file prometheus_server.hpp
/// @brief Lightweight Prometheus text-format metrics HTTP server.
///
/// Exposes a /metrics endpoint on a configurable TCP port using a
/// dedicated background thread. Uses POSIX sockets — no external
/// HTTP library required.
///
/// Metrics exposed:
///   hft_match_latency_ns_bucket   — latency histogram (64 power-of-2 buckets)
///   hft_match_latency_ns_count    — total orders processed
///   hft_match_latency_ns_sum      — sum of latencies in ns
///   hft_orders_total              — cumulative order counter
///   hft_pnl_total                 — current total P&L (gauge)
///   hft_fill_rate                 — fraction of orders filled (gauge)
///   hft_mpi_sweep_duration_seconds — sweep duration (gauge)
///
/// Usage:
/// @code
///   hft::PrometheusServer prom(9090);
///   prom.set_latency_histogram(engine.latency_histogram());
///   prom.set_pnl(tracker.total_pnl(), tracker.fill_rate());
///   prom.start();
///   // ... run engine ...
///   prom.stop();
/// @endcode

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "latency_histogram.hpp"

namespace hft {

/// @brief Shared metrics snapshot (written by engine, read by HTTP thread).
struct MetricsSnapshot {
    // Latency histogram — copy of bucket counts
    std::array<uint64_t, LatencyHistogram::kNumBuckets> lat_buckets = {};
    uint64_t lat_count = 0;
    uint64_t lat_sum_ns = 0;
    double   lat_p50_ns = 0.0;
    double   lat_p99_ns = 0.0;

    // Strategy / PnL metrics
    uint64_t orders_total  = 0;
    double   pnl_total     = 0.0;
    double   fill_rate     = 0.0;

    // MPI sweep
    double   sweep_duration_s = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// PrometheusServer
// ─────────────────────────────────────────────────────────────────────────────

class PrometheusServer {
public:
    explicit PrometheusServer(uint16_t port = 9090) noexcept
        : port_(port) {}

    ~PrometheusServer() { stop(); }

    PrometheusServer(const PrometheusServer&)            = delete;
    PrometheusServer& operator=(const PrometheusServer&) = delete;

    // ─── Metric setters (thread-safe) ─────────────────────────────────────

    void update(const MetricsSnapshot& snap) noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        snap_ = snap;
    }

    void set_latency(const LatencyHistogram& hist, uint64_t orders) noexcept {
        MetricsSnapshot s;
        {
            std::lock_guard<std::mutex> lk(mu_);
            s = snap_;
        }
        s.orders_total = orders;
        s.lat_p50_ns   = hist.percentile(50.0);
        s.lat_p99_ns   = hist.percentile(99.0);
        s.lat_count    = orders;
        for (int b = 0; b < LatencyHistogram::kNumBuckets; ++b) {
            s.lat_buckets[b] = hist.bucket_count(b);
            const uint64_t upper = (1ULL << b);
            s.lat_sum_ns    += hist.bucket_count(b) * upper; // approximate
        }
        std::lock_guard<std::mutex> lk(mu_);
        snap_ = s;
    }

    void set_pnl(double pnl, double fill_rate) noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.pnl_total  = pnl;
        snap_.fill_rate  = fill_rate;
    }

    void set_sweep_duration(double seconds) noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.sweep_duration_s = seconds;
    }

    // ─── Lifecycle ────────────────────────────────────────────────────────

    bool start() {
        server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) return false;

        int opt = 1;
        ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);

        if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(server_fd_);
            server_fd_ = -1;
            return false;
        }
        if (::listen(server_fd_, 4) < 0) {
            ::close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { serve_loop(); });
        return true;
    }

    void stop() noexcept {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (server_fd_ >= 0) {
            ::shutdown(server_fd_, SHUT_RDWR);
            ::close(server_fd_);
            server_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] uint16_t port() const noexcept { return port_; }

private:
    // ─── HTTP serve loop ──────────────────────────────────────────────────

    void serve_loop() noexcept {
        while (running_.load(std::memory_order_acquire)) {
            sockaddr_in client_addr{};
            socklen_t   client_len = sizeof(client_addr);
            int         client_fd  = ::accept(server_fd_,
                reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) break;

            handle_request(client_fd);
            ::close(client_fd);
        }
    }

    void handle_request(int fd) noexcept {
        // Read the request (we only need the first line to dispatch)
        char buf[512];
        ::recv(fd, buf, sizeof(buf) - 1, 0);
        buf[511] = '\0';

        // Only handle GET /metrics; return 404 for everything else.
        const bool is_metrics = (std::strncmp(buf, "GET /metrics", 12) == 0);
        if (!is_metrics) {
            const char* r404 =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 9\r\n\r\nNot Found";
            ::send(fd, r404, std::strlen(r404), MSG_NOSIGNAL);
            return;
        }

        const std::string body = render_metrics();
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
            << "Content-Length: " << body.size() << "\r\n\r\n"
            << body;
        const std::string response = oss.str();
        ::send(fd, response.c_str(), response.size(), MSG_NOSIGNAL);
    }

    // ─── Prometheus text format rendering ────────────────────────────────

    std::string render_metrics() const {
        MetricsSnapshot s;
        {
            std::lock_guard<std::mutex> lk(mu_);
            s = snap_;
        }

        std::ostringstream o;

        // ── Match latency histogram ──────────────────────────────────────
        o << "# HELP hft_match_latency_ns Order-to-fill latency in nanoseconds\n"
          << "# TYPE hft_match_latency_ns histogram\n";
        uint64_t cumulative = 0;
        for (int b = 0; b < LatencyHistogram::kNumBuckets; ++b) {
            cumulative += s.lat_buckets[b];
            const uint64_t upper_ns = (1ULL << b);
            o << "hft_match_latency_ns_bucket{le=\"" << upper_ns << "\"} "
              << cumulative << "\n";
        }
        o << "hft_match_latency_ns_bucket{le=\"+Inf\"} " << s.lat_count << "\n"
          << "hft_match_latency_ns_count " << s.lat_count << "\n"
          << "hft_match_latency_ns_sum "   << s.lat_sum_ns << "\n\n";

        // ── Orders counter ───────────────────────────────────────────────
        o << "# HELP hft_orders_total Total number of orders processed\n"
          << "# TYPE hft_orders_total counter\n"
          << "hft_orders_total " << s.orders_total << "\n\n";

        // ── P&L gauge ────────────────────────────────────────────────────
        o << "# HELP hft_pnl_total Current total P&L (realized + unrealized)\n"
          << "# TYPE hft_pnl_total gauge\n"
          << "hft_pnl_total " << s.pnl_total << "\n\n";

        // ── Fill rate gauge ──────────────────────────────────────────────
        o << "# HELP hft_fill_rate Fraction of orders filled [0, 1]\n"
          << "# TYPE hft_fill_rate gauge\n"
          << "hft_fill_rate " << s.fill_rate << "\n\n";

        // ── Sweep duration ───────────────────────────────────────────────
        o << "# HELP hft_mpi_sweep_duration_seconds Total parameter sweep duration\n"
          << "# TYPE hft_mpi_sweep_duration_seconds gauge\n"
          << "hft_mpi_sweep_duration_seconds " << s.sweep_duration_s << "\n\n";

        // ── Latency percentiles (additional convenience gauges) ──────────
        o << "# HELP hft_latency_p50_ns 50th percentile match latency\n"
          << "# TYPE hft_latency_p50_ns gauge\n"
          << "hft_latency_p50_ns " << s.lat_p50_ns << "\n\n"
          << "# HELP hft_latency_p99_ns 99th percentile match latency\n"
          << "# TYPE hft_latency_p99_ns gauge\n"
          << "hft_latency_p99_ns " << s.lat_p99_ns << "\n";

        return o.str();
    }

    // ─── State ────────────────────────────────────────────────────────────

    uint16_t             port_;
    int                  server_fd_ = -1;
    std::thread          thread_;
    std::atomic<bool>    running_{false};
    mutable std::mutex   mu_;
    MetricsSnapshot      snap_;
};

} // namespace hft
