/// @file main_window.cpp
/// @brief Qt GUI application entry point for the HFT dashboard.
///
/// Registers C++ backend models with the QML engine and loads Main.qml.
/// The --itch-file argument is forwarded to start a FeedSimulator that feeds
/// live data through the matching engine into the GUI models.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QThread>
#include <QTimer>
#include <memory>

#include "backend/latency_model.hpp"
#include "backend/order_book_model.hpp"
#include "backend/sweep_result_model.hpp"

// Engine headers
#include "../src/core/matching_engine.hpp"
#include "../src/core/spsc_queue.hpp"
#include "../src/feed/feed_simulator.hpp"
#include "../src/feed/itch_parser.hpp"
#include "../src/metrics/latency_histogram.hpp"

namespace {

// Parse --itch-file <path> from argv.
QString itch_file_from_args(int argc, char* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--itch-file") == 0) {
            return QString::fromLocal8Bit(argv[i + 1]);
        }
    }
    return {};
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("HFT Engine Dashboard");
    app.setApplicationVersion("1.0");

    // ── C++ backend models ───────────────────────────────────────────────────
    auto* orderBookModel   = new OrderBookModel(&app);
    auto* latencyModel     = new LatencyModel(&app);
    auto* sweepResultModel = new SweepResultModel(&app);

    // Register to QML as instantiable types (unused in QML directly, but
    // exposed as context properties for simplicity).
    QQmlApplicationEngine engine;

    engine.rootContext()->setContextProperty("orderBookModel",   orderBookModel);
    engine.rootContext()->setContextProperty("latencyModel",     latencyModel);
    engine.rootContext()->setContextProperty("sweepResultModel", sweepResultModel);

    // ── Load QML ─────────────────────────────────────────────────────────────
    engine.load(QUrl(QStringLiteral("qrc:/HFTEngine/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    // ── Start live feed if ITCH file provided ────────────────────────────────
    const QString itch_path = itch_file_from_args(argc, argv);

    // Matching engine queues
    static hft::SPSCQueue<hft::Order, 65536> order_queue;
    static hft::SPSCQueue<hft::Trade, 65536> trade_queue;

    std::unique_ptr<hft::MatchingEngine> matching_engine;
    std::unique_ptr<hft::FeedSimulator>  feed_sim;

    if (!itch_path.isEmpty()) {
        try {
            hft::ITCHParser parser(itch_path.toStdString());
            static auto ticks = parser.parse_all();

            matching_engine = std::make_unique<hft::MatchingEngine>(
                &order_queue, &trade_queue);
            matching_engine->start();

            feed_sim = std::make_unique<hft::FeedSimulator>(
                ticks, order_queue, 1.0 /*real-time*/, 0);
            feed_sim->start();
        } catch (const std::exception& e) {
            qWarning("Could not load ITCH file: %s", e.what());
        }
    }

    // ── Polling timer: push book snapshots and latency data to GUI ───────────
    QTimer poll_timer;
    poll_timer.setInterval(16); // 60fps
    QObject::connect(&poll_timer, &QTimer::timeout, [&]() {
        if (!matching_engine) return;

        const hft::OrderBook& book = matching_engine->book();

        // Build bid/ask snapshots
        QVector<OrderBookModel::LevelData> bids, asks;
        for (std::size_t i = 0; i < book.num_bids(); ++i) {
            bids.append({book.bids()[i].price, book.bids()[i].quantity, true});
        }
        for (std::size_t i = 0; i < book.num_asks(); ++i) {
            asks.append({book.asks()[i].price, book.asks()[i].quantity, false});
        }
        orderBookModel->updateSnapshot(bids, asks);

        // Latency histogram (update every 500ms would be better, but 60fps is harmless)
        const hft::LatencyHistogram& hist = matching_engine->latency_histogram();
        QVector<LatencyModel::BucketData> buckets;
        for (int b = 0; b < hft::LatencyHistogram::kNumBuckets; ++b) {
            LatencyModel::BucketData bd;
            bd.label  = QString("%1ns").arg(1ULL << b);
            bd.count  = hist.bucket_count(b);
            bd.min_ns = (b == 0) ? 0ULL : (1ULL << (b - 1));
            bd.max_ns = (1ULL << b) - 1;
            buckets.append(bd);
        }
        latencyModel->updateHistogram(buckets,
            hist.percentile(50.0), hist.percentile(99.0),
            hist.percentile(99.9), hist.percentile(50.0));
    });
    poll_timer.start();

    const int ret = app.exec();

    // ── Cleanup ───────────────────────────────────────────────────────────────
    if (feed_sim) feed_sim->stop();
    if (matching_engine) matching_engine->stop();

    return ret;
}
