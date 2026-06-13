#include "sweep_result_model.hpp"
#include <QFile>
#include <QTextStream>
#include <algorithm>

SweepResultModel::SweepResultModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int SweepResultModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(rows_.size());
}

int SweepResultModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ColCount;
}

QVariant SweepResultModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size())
        return {};

    const auto& row = rows_[index.row()];

    if (role == Qt::DisplayRole || role == Qt::UserRole + index.column()) {
        switch (index.column()) {
        case ColSpreadTicks:  return row.spread_ticks;
        case ColOrderQty:     return row.order_qty;
        case ColLookbackNs:   return QVariant::fromValue(static_cast<qulonglong>(row.lookback_ns));
        case ColSharpe:       return QString::number(row.sharpe, 'f', 4);
        case ColMaxDrawdown:  return QString::number(row.max_drawdown, 'f', 2);
        case ColTotalPnl:     return QString::number(row.total_pnl, 'f', 2);
        case ColFillRate:     return QString::number(row.fill_rate * 100.0, 'f', 1) + "%";
        case ColAvgLatency:   return QString::number(row.avg_latency_ns, 'f', 0) + "ns";
        default: return {};
        }
    }

    // Raw numeric roles for sorting/coloring in QML
    if (role == Qt::UserRole + 100) {  // sharpeRaw for color coding
        return row.sharpe;
    }

    return {};
}

QVariant SweepResultModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return {};

    switch (section) {
    case ColSpreadTicks:  return QStringLiteral("Spread (ticks)");
    case ColOrderQty:     return QStringLiteral("Order Qty");
    case ColLookbackNs:   return QStringLiteral("Lookback (ns)");
    case ColSharpe:       return QStringLiteral("Sharpe");
    case ColMaxDrawdown:  return QStringLiteral("Max DD");
    case ColTotalPnl:     return QStringLiteral("Total P&L");
    case ColFillRate:     return QStringLiteral("Fill Rate");
    case ColAvgLatency:   return QStringLiteral("Avg Latency");
    default: return {};
    }
}

QHash<int, QByteArray> SweepResultModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles[Qt::DisplayRole] = "display";
    roles[Qt::UserRole + ColSpreadTicks] = "spreadTicks";
    roles[Qt::UserRole + ColOrderQty] = "orderQty";
    roles[Qt::UserRole + ColLookbackNs] = "lookbackNs";
    roles[Qt::UserRole + ColSharpe] = "sharpe";
    roles[Qt::UserRole + ColMaxDrawdown] = "maxDrawdown";
    roles[Qt::UserRole + ColTotalPnl] = "totalPnl";
    roles[Qt::UserRole + ColFillRate] = "fillRate";
    roles[Qt::UserRole + ColAvgLatency] = "avgLatency";
    roles[Qt::UserRole + 100] = "sharpeRaw";
    return roles;
}

void SweepResultModel::loadFromCSV(const QString& filepath) {
    is_loading_ = true;
    emit loadingChanged();

    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        is_loading_ = false;
        emit loadingChanged();
        return;
    }

    QVector<SweepRow> new_rows;
    QTextStream stream(&file);

    // Skip header line
    if (!stream.atEnd()) stream.readLine();

    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty()) continue;

        QStringList fields = line.split(',');
        if (fields.size() < 8) continue;

        SweepRow row;
        row.spread_ticks  = fields[0].toInt();
        row.order_qty     = fields[1].toInt();
        row.lookback_ns   = fields[2].toULongLong();
        row.sharpe        = fields[3].toDouble();
        row.max_drawdown  = fields[4].toDouble();
        row.total_pnl     = fields[5].toDouble();
        row.fill_rate     = fields[6].toDouble();
        row.avg_latency_ns = fields[7].toDouble();
        new_rows.append(row);
    }

    file.close();
    setResults(new_rows);

    is_loading_ = false;
    emit loadingChanged();
}

void SweepResultModel::setResults(const QVector<SweepRow>& rows) {
    beginResetModel();
    rows_ = rows;
    endResetModel();
    emit resultsUpdated();
}

void SweepResultModel::sortByColumn(int column, bool ascending) {
    beginResetModel();

    auto cmp = [column, ascending](const SweepRow& a, const SweepRow& b) -> bool {
        double va = 0, vb = 0;
        switch (column) {
        case ColSpreadTicks:  va = a.spread_ticks;  vb = b.spread_ticks;  break;
        case ColOrderQty:     va = a.order_qty;     vb = b.order_qty;     break;
        case ColLookbackNs:   va = static_cast<double>(a.lookback_ns); vb = static_cast<double>(b.lookback_ns); break;
        case ColSharpe:       va = a.sharpe;        vb = b.sharpe;        break;
        case ColMaxDrawdown:  va = a.max_drawdown;  vb = b.max_drawdown;  break;
        case ColTotalPnl:     va = a.total_pnl;     vb = b.total_pnl;     break;
        case ColFillRate:     va = a.fill_rate;     vb = b.fill_rate;     break;
        case ColAvgLatency:   va = a.avg_latency_ns; vb = b.avg_latency_ns; break;
        default: return false;
        }
        return ascending ? (va < vb) : (va > vb);
    };

    std::sort(rows_.begin(), rows_.end(), cmp);
    endResetModel();
}
