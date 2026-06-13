#pragma once

#include <QAbstractListModel>
#include <QTimer>
#include <QMutex>
#include <QVector>
#include <cstdint>

/// @brief Qt model exposing latency histogram data to QML charts.
///
/// Provides bucket data for bar charts and live p50/p99/p999 percentile text.
/// Updates every 500ms.
class LatencyModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(double p50 READ p50 NOTIFY dataUpdated)
    Q_PROPERTY(double p99 READ p99 NOTIFY dataUpdated)
    Q_PROPERTY(double p999 READ p999 NOTIFY dataUpdated)
    Q_PROPERTY(double avgLatency READ avgLatency NOTIFY dataUpdated)

public:
    enum Roles {
        BucketLabelRole = Qt::UserRole + 1,  // e.g. "100ns", "200ns", ...
        CountRole,                            // number of samples in bucket
        BucketMinRole,                        // bucket lower bound in ns
        BucketMaxRole,                        // bucket upper bound in ns
    };

    struct BucketData {
        QString label;
        uint64_t count = 0;
        uint64_t min_ns = 0;
        uint64_t max_ns = 0;
    };

    explicit LatencyModel(QObject* parent = nullptr);
    ~LatencyModel() override = default;

    // QAbstractListModel
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Properties
    double p50() const { return p50_; }
    double p99() const { return p99_; }
    double p999() const { return p999_; }
    double avgLatency() const { return avg_latency_; }

    /// Thread-safe update from engine thread
    void updateHistogram(const QVector<BucketData>& buckets,
                         double p50, double p99, double p999, double avg);

signals:
    void dataUpdated();

private slots:
    void onRefreshTimer();

private:
    QTimer refresh_timer_;
    mutable QMutex mutex_;

    QVector<BucketData> pending_buckets_;
    QVector<BucketData> display_buckets_;
    bool has_pending_ = false;

    double p50_ = 0, p99_ = 0, p999_ = 0, avg_latency_ = 0;
    double pending_p50_ = 0, pending_p99_ = 0, pending_p999_ = 0, pending_avg_ = 0;
};
