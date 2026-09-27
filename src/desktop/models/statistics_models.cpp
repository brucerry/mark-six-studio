#include "statistics_models.hpp"

#include <QVariant>

#include <stdexcept>

using marksix::statistics::Presentation;

StatisticsTableModel::StatisticsTableModel(QObject* parent) : QAbstractTableModel(parent) {}

int StatisticsTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(presentation_.rows.size());
}

int StatisticsTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(presentation_.columns.size());
}

QVariant StatisticsTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.column() < 0 ||
        size_t(index.row()) >= presentation_.rows.size() ||
        size_t(index.column()) >= presentation_.rows[size_t(index.row())].size()) return {};
    const auto& cell = presentation_.rows[size_t(index.row())][size_t(index.column())];
    switch (role) {
    case Qt::DisplayRole:
    case DisplayRole: return QString::fromStdString(cell.text);
    case NumericRole: return cell.numeric ? QVariant(*cell.numeric) : QVariant();
    case IdentityRole:
        if (size_t(index.row()) >= presentation_.identities.size()) return {};
        return QVariant::fromValue(qulonglong(identityBase_ + presentation_.identities[size_t(index.row())]));
    default: return {};
    }
}

QVariant StatisticsTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 &&
        size_t(section) < presentation_.columns.size())
        return QString::fromStdString(presentation_.columns[size_t(section)]);
    return {};
}

QHash<int, QByteArray> StatisticsTableModel::roleNames() const {
    return {{DisplayRole, "display"}, {NumericRole, "numeric"}, {IdentityRole, "stableId"}};
}

void StatisticsTableModel::sort(int column, Qt::SortOrder order) {
    if (column < 0 || size_t(column) >= presentation_.columns.size()) return;
    // A reset invalidates old QModelIndex values; consumers reselect by
    // stableId instead of accidentally reading a different row after sort.
    beginResetModel();
    marksix::statistics::sortRows(presentation_, size_t(column), order == Qt::DescendingOrder);
    endResetModel();
}

void StatisticsTableModel::setPresentation(Presentation value, size_t identityBase) {
    if (value.rows.size() != value.identities.size())
        throw std::invalid_argument("Statistics presentation row identities missing");
    for (const auto& row : value.rows)
        if (row.size() != value.columns.size())
            throw std::invalid_argument("Statistics presentation columns mismatch");
    beginResetModel();
    presentation_ = std::move(value);
    identityBase_ = identityBase;
    endResetModel();
    emit presentationChanged();
}

QString StatisticsTableModel::detailForId(qulonglong stableId) const {
    QString detail;
    for (size_t row = 0; row < presentation_.identities.size(); ++row) {
        if (identityBase_ + presentation_.identities[row] != stableId) continue;
        for (size_t column = 0; column < presentation_.columns.size(); ++column) {
            if (!detail.isEmpty()) detail += QStringLiteral("\n");
            detail += QString::fromStdString(presentation_.columns[column]) + QStringLiteral(": ") +
                QString::fromStdString(presentation_.rows[row][column].text);
        }
        return detail;
    }
    return QStringLiteral("Select a row to inspect its evidence and exact values.");
}

QString StatisticsTableModel::summary() const { return QString::fromStdString(presentation_.summary); }
QString StatisticsTableModel::explanation() const { return QString::fromStdString(presentation_.explanation); }
QString StatisticsTableModel::chartTitle() const { return QString::fromStdString(presentation_.chartTitle); }
QVariantList StatisticsTableModel::chartValues() const {
    QVariantList values;
    for (double value : presentation_.chart) values.push_back(value);
    return values;
}
QVariantList StatisticsTableModel::chartOther() const {
    QVariantList values;
    for (double value : presentation_.chartOther) values.push_back(value);
    return values;
}
QStringList StatisticsTableModel::headers() const {
    QStringList values;
    for (const auto& name : presentation_.columns) values.push_back(QString::fromStdString(name));
    return values;
}

StatisticsModels::StatisticsModels(QObject* parent) : QObject(parent) {}
