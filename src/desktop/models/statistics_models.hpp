#pragma once

#include "presentation.hpp"

#include <QAbstractTableModel>
#include <QObject>
#include <QString>

// UI-thread-only value adapter. Expensive archive/model work runs elsewhere and
// hands a completed immutable presentation to setPresentation().
class StatisticsTableModel final : public QAbstractTableModel {
    Q_OBJECT
    Q_PROPERTY(QString summary READ summary NOTIFY presentationChanged)
    Q_PROPERTY(QString explanation READ explanation NOTIFY presentationChanged)
    Q_PROPERTY(QString chartTitle READ chartTitle NOTIFY presentationChanged)
    Q_PROPERTY(QVariantList chartValues READ chartValues NOTIFY presentationChanged)
    Q_PROPERTY(QVariantList chartOther READ chartOther NOTIFY presentationChanged)
    Q_PROPERTY(bool chartZero READ chartZero NOTIFY presentationChanged)
    Q_PROPERTY(QStringList headers READ headers NOTIFY presentationChanged)
public:
    enum Role { DisplayRole = Qt::UserRole + 1, NumericRole, IdentityRole };
    explicit StatisticsTableModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    Q_INVOKABLE void sortBy(int column, bool descending) { sort(column, descending ? Qt::DescendingOrder : Qt::AscendingOrder); }
    Q_INVOKABLE QString detailForId(qulonglong stableId) const;
    void setPresentation(marksix::statistics::Presentation value, size_t identityBase = 0);
    QString summary() const;
    QString explanation() const;
    QString chartTitle() const;
    QVariantList chartValues() const;
    QVariantList chartOther() const;
    bool chartZero() const { return presentation_.chartZero; }
    QStringList headers() const;
    const marksix::statistics::Presentation& presentation() const { return presentation_; }
signals:
    void presentationChanged();
private:
    marksix::statistics::Presentation presentation_;
    size_t identityBase_ = 0;
};

class StatisticsModels final : public QObject {
    Q_OBJECT
    Q_PROPERTY(StatisticsTableModel* archive READ archive CONSTANT)
    Q_PROPERTY(StatisticsTableModel* sources READ sources CONSTANT)
    Q_PROPERTY(StatisticsTableModel* analysis READ analysis CONSTANT)
    Q_PROPERTY(StatisticsTableModel* pairs READ pairs CONSTANT)
    Q_PROPERTY(StatisticsTableModel* rolling READ rolling CONSTANT)
    Q_PROPERTY(StatisticsTableModel* forecastLab READ forecastLab CONSTANT)
    Q_PROPERTY(StatisticsTableModel* learning READ learning CONSTANT)
    Q_PROPERTY(StatisticsTableModel* progress READ progress CONSTANT)
    Q_PROPERTY(StatisticsTableModel* labProbabilities READ labProbabilities CONSTANT)
    Q_PROPERTY(StatisticsTableModel* adaptive READ adaptive CONSTANT)
    Q_PROPERTY(StatisticsTableModel* fixed READ fixed CONSTANT)
    Q_PROPERTY(StatisticsTableModel* exact READ exact CONSTANT)
    Q_PROPERTY(StatisticsTableModel* calibration READ calibration CONSTANT)
public:
    explicit StatisticsModels(QObject* parent = nullptr);
    StatisticsTableModel* archive() { return &archive_; }
    StatisticsTableModel* sources() { return &sources_; }
    StatisticsTableModel* analysis() { return &analysis_; }
    StatisticsTableModel* pairs() { return &pairs_; }
    StatisticsTableModel* rolling() { return &rolling_; }
    StatisticsTableModel* forecastLab() { return &forecastLab_; }
    StatisticsTableModel* learning() { return &learning_; }
    StatisticsTableModel* progress() { return &progress_; }
    StatisticsTableModel* labProbabilities() { return &labProbabilities_; }
    StatisticsTableModel* adaptive() { return &adaptive_; }
    StatisticsTableModel* fixed() { return &fixed_; }
    StatisticsTableModel* exact() { return &exact_; }
    StatisticsTableModel* calibration() { return &calibration_; }
private:
    StatisticsTableModel archive_{this};
    StatisticsTableModel sources_{this};
    StatisticsTableModel analysis_{this};
    StatisticsTableModel pairs_{this};
    StatisticsTableModel rolling_{this};
    StatisticsTableModel forecastLab_{this};
    StatisticsTableModel learning_{this};
    StatisticsTableModel progress_{this};
    StatisticsTableModel labProbabilities_{this};
    StatisticsTableModel adaptive_{this};
    StatisticsTableModel fixed_{this};
    StatisticsTableModel exact_{this};
    StatisticsTableModel calibration_{this};
};
