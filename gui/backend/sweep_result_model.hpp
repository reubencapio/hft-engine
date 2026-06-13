#pragma once

#include <QAbstractTableModel>
#include <QVector>
#include <QMutex>
#include <cstdint>

/// @brief Qt table model for displaying parameter sweep results in QML.
///
/// Columns: spread_ticks, order_qty, lookback_ns, sharpe, max_drawdown, total_pnl
/// Supports sorting by clicking column headers (handled in QML via SortFilterProxyModel).
/// Color-coded by Sharpe ratio in QML.
class SweepResultModel : public QAbstractTableModel {
    Q_OBJECT
    Q_PROPERTY(int totalResults READ totalResults NOTIFY resultsUpdated)
    Q_PROPERTY(bool isLoading READ isLoading NOTIFY loadingChanged)

public:
    enum Columns {
        ColSpreadTicks = 0,
        ColOrderQty,
        ColLookbackNs,
        ColSharpe,
        ColMaxDrawdown,
        ColTotalPnl,
        ColFillRate,
        ColAvgLatency,
        ColCount
    };

    struct SweepRow {
        int spread_ticks = 0;
        int order_qty = 0;
        uint64_t lookback_ns = 0;
        double sharpe = 0.0;
        double max_drawdown = 0.0;
        double total_pnl = 0.0;
        double fill_rate = 0.0;
        double avg_latency_ns = 0.0;
    };

    explicit SweepResultModel(QObject* parent = nullptr);
    ~SweepResultModel() override = default;

    // QAbstractTableModel
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Properties
    int totalResults() const { return static_cast<int>(rows_.size()); }
    bool isLoading() const { return is_loading_; }

    /// Load results from a CSV file (called from background thread)
    Q_INVOKABLE void loadFromCSV(const QString& filepath);

    /// Set results directly (thread-safe)
    void setResults(const QVector<SweepRow>& rows);

    /// Sort by column
    Q_INVOKABLE void sortByColumn(int column, bool ascending = false);

signals:
    void resultsUpdated();
    void loadingChanged();

private:
    QVector<SweepRow> rows_;
    mutable QMutex mutex_;
    bool is_loading_ = false;
};
