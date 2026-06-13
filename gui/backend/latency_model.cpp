#include "latency_model.hpp"

LatencyModel::LatencyModel(QObject* parent)
    : QAbstractListModel(parent)
{
    // Update every 500ms — latency histograms don't need 60fps
    refresh_timer_.setInterval(500);
    connect(&refresh_timer_, &QTimer::timeout, this, &LatencyModel::onRefreshTimer);
    refresh_timer_.start();
}

int LatencyModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(display_buckets_.size());
}

QVariant LatencyModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= display_buckets_.size())
        return {};

    const auto& bucket = display_buckets_[index.row()];

    switch (role) {
    case BucketLabelRole:
        return bucket.label;
    case CountRole:
        return QVariant::fromValue(static_cast<qulonglong>(bucket.count));
    case BucketMinRole:
        return QVariant::fromValue(static_cast<qulonglong>(bucket.min_ns));
    case BucketMaxRole:
        return QVariant::fromValue(static_cast<qulonglong>(bucket.max_ns));
    default:
        return {};
    }
}

QHash<int, QByteArray> LatencyModel::roleNames() const {
    return {
        {BucketLabelRole, "bucketLabel"},
        {CountRole,       "count"},
        {BucketMinRole,   "bucketMin"},
        {BucketMaxRole,   "bucketMax"},
    };
}

void LatencyModel::updateHistogram(const QVector<BucketData>& buckets,
                                    double p50, double p99, double p999, double avg) {
    QMutexLocker lock(&mutex_);
    pending_buckets_ = buckets;
    pending_p50_ = p50;
    pending_p99_ = p99;
    pending_p999_ = p999;
    pending_avg_ = avg;
    has_pending_ = true;
}

void LatencyModel::onRefreshTimer() {
    QMutexLocker lock(&mutex_);
    if (!has_pending_) return;

    beginResetModel();
    display_buckets_ = pending_buckets_;
    p50_ = pending_p50_;
    p99_ = pending_p99_;
    p999_ = pending_p999_;
    avg_latency_ = pending_avg_;
    has_pending_ = false;
    endResetModel();

    emit dataUpdated();
}
