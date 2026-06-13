#pragma once

#include <QAbstractListModel>
#include <QTimer>
#include <QMutex>
#include <QVector>
#include <cstdint>

/// @brief Qt model exposing order book bid/ask levels to QML.
///
/// Each row represents a single price level with price, quantity, and side.
/// The model is updated at 60fps (16ms) by polling an internal snapshot buffer.
class OrderBookModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int bestBidPrice READ bestBidPrice NOTIFY dataChanged)
    Q_PROPERTY(int bestAskPrice READ bestAskPrice NOTIFY dataChanged)
    Q_PROPERTY(int spread READ spread NOTIFY dataChanged)

public:
    enum Roles {
        PriceRole = Qt::UserRole + 1,
        QuantityRole,
        SideRole,          // "bid" or "ask"
        DepthPercentRole,  // quantity as % of max quantity (for bar width)
    };

    struct LevelData {
        int64_t price = 0;
        int64_t quantity = 0;
        bool is_bid = true;
    };

    explicit OrderBookModel(QObject* parent = nullptr);
    ~OrderBookModel() override = default;

    // QAbstractListModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Properties
    int bestBidPrice() const;
    int bestAskPrice() const;
    int spread() const;

    /// Called from the engine thread to push a new book snapshot.
    /// Thread-safe — copies data under a mutex.
    void updateSnapshot(const QVector<LevelData>& bids, const QVector<LevelData>& asks);

signals:
    void dataChanged();

private slots:
    void onRefreshTimer();

private:
    QTimer refresh_timer_;
    mutable QMutex mutex_;

    // Double-buffered: pending_ is written by engine thread, display_ is read by QML
    QVector<LevelData> pending_levels_;
    QVector<LevelData> display_levels_;
    bool has_pending_ = false;

    int64_t best_bid_ = 0;
    int64_t best_ask_ = 0;
    int64_t max_quantity_ = 1;  // avoid div-by-zero
};
