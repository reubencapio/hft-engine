#include "order_book_model.hpp"
#include <algorithm>

OrderBookModel::OrderBookModel(QObject* parent)
    : QAbstractListModel(parent)
{
    // Refresh at 60fps for real-time order book visualization
    refresh_timer_.setInterval(16);
    connect(&refresh_timer_, &QTimer::timeout, this, &OrderBookModel::onRefreshTimer);
    refresh_timer_.start();
}

int OrderBookModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(display_levels_.size());
}

QVariant OrderBookModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= display_levels_.size())
        return {};

    const auto& level = display_levels_[index.row()];

    switch (role) {
    case PriceRole:
        return QVariant::fromValue(static_cast<qlonglong>(level.price));
    case QuantityRole:
        return QVariant::fromValue(static_cast<qlonglong>(level.quantity));
    case SideRole:
        return level.is_bid ? QStringLiteral("bid") : QStringLiteral("ask");
    case DepthPercentRole: {
        double pct = (max_quantity_ > 0)
            ? (static_cast<double>(level.quantity) / static_cast<double>(max_quantity_)) * 100.0
            : 0.0;
        return QVariant::fromValue(pct);
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> OrderBookModel::roleNames() const {
    return {
        {PriceRole,        "price"},
        {QuantityRole,     "quantity"},
        {SideRole,         "side"},
        {DepthPercentRole, "depthPercent"},
    };
}

int OrderBookModel::bestBidPrice() const {
    return static_cast<int>(best_bid_);
}

int OrderBookModel::bestAskPrice() const {
    return static_cast<int>(best_ask_);
}

int OrderBookModel::spread() const {
    return static_cast<int>(best_ask_ - best_bid_);
}

void OrderBookModel::updateSnapshot(const QVector<LevelData>& bids,
                                     const QVector<LevelData>& asks) {
    QMutexLocker lock(&mutex_);

    pending_levels_.clear();
    pending_levels_.reserve(bids.size() + asks.size());

    // Asks in ascending order (top of book first)
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
        pending_levels_.append(*it);
    }
    // Bids in descending order (top of book first)
    for (const auto& bid : bids) {
        pending_levels_.append(bid);
    }

    if (!bids.isEmpty()) best_bid_ = bids.first().price;
    if (!asks.isEmpty()) best_ask_ = asks.first().price;

    has_pending_ = true;
}

void OrderBookModel::onRefreshTimer() {
    QMutexLocker lock(&mutex_);
    if (!has_pending_) return;

    beginResetModel();
    display_levels_ = pending_levels_;

    // Compute max quantity for depth visualization
    max_quantity_ = 1;
    for (const auto& level : display_levels_) {
        max_quantity_ = std::max(max_quantity_, level.quantity);
    }

    has_pending_ = false;
    endResetModel();

    emit dataChanged();
}
