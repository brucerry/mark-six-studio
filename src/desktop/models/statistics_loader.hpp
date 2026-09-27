#pragma once

#include "adaptive_coordinator.hpp"
#include "statistics_models.hpp"
#include "worker.hpp"
#include "update_worker.hpp"
#include "forecast_lab_control_worker.hpp"
#include "import.hpp"

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QUrl>

#include <filesystem>
#include <functional>

struct PreparedStatistics {
    marksix::statistics::Presentation archive, sources, analysis, pairs, rolling, forecastLab, learning, progress;
    marksix::statistics::Presentation labProbabilities, adaptive, fixed, exact, calibration;
    size_t labOffset = 0, learningOffset = 0;
    std::array<int, 6> suggestedMain{};
    bool selectionAvailable = false;
    std::string selectionState = "No compatible forecast yet";
    std::string selectedModel, cutoff, evidence, sourceStatus;
    std::array<std::string, 2> eraFirstDates{};
    std::array<std::string, 2> eraLastDates{};
    std::vector<double> progressPoints;
    std::string progressSummary;
    std::string comparisonSummary;
    std::string controlSummary = "Fair-history controls not run automatically.";
    std::string controlDigest;
    size_t controlHistoryLength = 0;
    double controlObservedStatistic = 0;
    bool controlEligible = false;
    std::array<int, 6> adaptiveMain{}, fixedMain{};
    int adaptiveExtra = 0, fixedExtra = 0;
    std::string adaptiveCutoff, fixedCutoff;
    std::vector<std::string> learningLineages;
    std::vector<int64_t> learningLineageIds;
    size_t learningCount = 0;
    int learningLineageIndex = 0;
    uint64_t sourceVersion = 0;
};

// All database/model work executes on owned workers. A queued timer callback
// consumes only the latest generation and touches Qt models on the UI thread.
class StatisticsLoader final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(bool updateBusy READ updateBusy NOTIFY stateChanged)
    Q_PROPERTY(QString updateStatus READ updateStatus NOTIFY stateChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable CONSTANT)
    Q_PROPERTY(QStringList eraFirstDates READ eraFirstDates NOTIFY stateChanged)
    Q_PROPERTY(QStringList eraLastDates READ eraLastDates NOTIFY stateChanged)
    Q_PROPERTY(QVariantList suggestedMain READ suggestedMain NOTIFY stateChanged)
    Q_PROPERTY(bool selectionAvailable READ selectionAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString selectionState READ selectionState NOTIFY stateChanged)
    Q_PROPERTY(QString selectedModel READ selectedModel NOTIFY stateChanged)
    Q_PROPERTY(QString cutoff READ cutoff NOTIFY stateChanged)
    Q_PROPERTY(QString evidence READ evidence NOTIFY stateChanged)
    Q_PROPERTY(QString sourceStatus READ sourceStatus NOTIFY stateChanged)
    Q_PROPERTY(QVariantList progressPoints READ progressPoints NOTIFY stateChanged)
    Q_PROPERTY(QString progressSummary READ progressSummary NOTIFY stateChanged)
    Q_PROPERTY(QString comparisonSummary READ comparisonSummary NOTIFY stateChanged)
    Q_PROPERTY(QString controlStatus READ controlStatus NOTIFY stateChanged)
    Q_PROPERTY(bool controlBusy READ controlBusy NOTIFY stateChanged)
    Q_PROPERTY(bool controlEligible READ controlEligible NOTIFY stateChanged)
    Q_PROPERTY(QVariantList adaptiveMain READ adaptiveMain NOTIFY stateChanged)
    Q_PROPERTY(int adaptiveExtra READ adaptiveExtra NOTIFY stateChanged)
    Q_PROPERTY(QString adaptiveCutoff READ adaptiveCutoff NOTIFY stateChanged)
    Q_PROPERTY(QVariantList fixedMain READ fixedMain NOTIFY stateChanged)
    Q_PROPERTY(int fixedExtra READ fixedExtra NOTIFY stateChanged)
    Q_PROPERTY(QString fixedCutoff READ fixedCutoff NOTIFY stateChanged)
    Q_PROPERTY(QStringList learningLineages READ learningLineages NOTIFY stateChanged)
    Q_PROPERTY(int learningLineageIndex READ learningLineageIndex NOTIFY stateChanged)
    Q_PROPERTY(int learningOffset READ learningOffset NOTIFY stateChanged)
    Q_PROPERTY(int learningCount READ learningCount NOTIFY stateChanged)
    Q_PROPERTY(int progressEvidence READ progressEvidence NOTIFY stateChanged)
    Q_PROPERTY(int progressMode READ progressMode NOTIFY stateChanged)
    Q_PROPERTY(QString analysisStatus READ analysisStatus NOTIFY stateChanged)
    Q_PROPERTY(QString sourceWorkStatus READ sourceWorkStatus NOTIFY stateChanged)
    Q_PROPERTY(bool sourceWorkBusy READ sourceWorkBusy NOTIFY stateChanged)
    Q_PROPERTY(bool importPreviewReady READ importPreviewReady NOTIFY stateChanged)
public:
    using Job = std::function<PreparedStatistics(std::stop_token)>;
    StatisticsLoader(std::filesystem::path root, StatisticsModels& models, QObject* parent = nullptr);
    StatisticsLoader(Job injectedJob, StatisticsModels& models, QObject* parent = nullptr);
    StatisticsLoader(std::filesystem::path root, std::shared_ptr<marksix::statistics::UpdateProvider> provider,
                     Job injectedJob, StatisticsModels& models, QObject* parent = nullptr);
    ~StatisticsLoader() override;
    Q_INVOKABLE void reload();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool startUpdate();
    Q_INVOKABLE void cancelUpdate();
    void close();
    bool busy() const { return busy_; }
    QString error() const { return error_; }
    bool updateBusy() const { return updater_.busy(); }
    QString updateStatus() const { return updateStatus_; }
    bool updateAvailable() const { return updater_.available(); }
    QStringList eraFirstDates() const;
    QStringList eraLastDates() const;
    QVariantList suggestedMain() const;
    bool selectionAvailable() const { return dashboard_.selectionAvailable; }
    QString selectionState() const;
    QString selectedModel() const;
    QString cutoff() const;
    QString evidence() const;
    QString sourceStatus() const;
    QVariantList progressPoints() const;
    QString progressSummary() const;
    QString comparisonSummary() const;
    QString controlStatus() const { return controlStatus_; }
    bool controlBusy() const { return controlWorker_.progress().active; }
    bool controlEligible() const { return dashboard_.controlEligible; }
    QVariantList adaptiveMain() const;
    int adaptiveExtra() const { return dashboard_.adaptiveExtra; }
    QString adaptiveCutoff() const { return QString::fromStdString(dashboard_.adaptiveCutoff); }
    QVariantList fixedMain() const;
    int fixedExtra() const { return dashboard_.fixedExtra; }
    QString fixedCutoff() const { return QString::fromStdString(dashboard_.fixedCutoff); }
    Q_INVOKABLE bool startFairControls();
    Q_INVOKABLE void cancelFairControls();
    QStringList learningLineages() const;
    int learningLineageIndex() const { return dashboard_.learningLineageIndex; }
    int learningOffset() const { return int(dashboard_.learningOffset); }
    int learningCount() const { return int(dashboard_.learningCount); }
    int progressEvidence() const { return options_.progressEvidence; }
    int progressMode() const { return options_.progressMode; }
    Q_INVOKABLE void chooseLearningLineage(int index);
    Q_INVOKABLE void pageLearning(int direction);
    Q_INVOKABLE void setProgressView(int evidence, int mode);
    Q_INVOKABLE bool setAnalysisSettings(int scope, int lastN, const QString& from,
                                        const QString& through, int number, int era);
    QString analysisStatus() const { return analysisStatus_; }
    QString sourceWorkStatus() const { return sourceWorkStatus_; }
    bool sourceWorkBusy() const { return sourceWorker_.busy(); }
    bool importPreviewReady() const { return preview_.has_value(); }
    Q_INVOKABLE bool previewCsv(const QUrl& csv, const QUrl& manifest);
    Q_INVOKABLE bool acceptCsv();
    Q_INVOKABLE void cancelSourceWork();
    Q_INVOKABLE bool resolveConflict(int revisionIndex, const QString& reason, const QString& evidence);
signals:
    void stateChanged();
private:
    void poll();
    struct SourceWorkResult {
        std::optional<marksix::statistics::ImportPreview> preview;
        std::string status;
        bool committed = false;
    };
    struct ViewOptions {
        int64_t learningLineage = 0;
        size_t learningOffset = 0;
        int progressEvidence = 0, progressMode = 1;
        int scope = 0, era = 0, selectedNumber = 1;
        size_t lastN = 0;
        std::string from, through;
    };
    static PreparedStatistics prepare(const std::filesystem::path& root,
                                      marksix::statistics::AdaptiveCoordinator& coordinator,
                                      std::stop_token stop, ViewOptions options);
    StatisticsModels& models_;
    std::filesystem::path root_;
    // Member declaration order matters: worker is destroyed before coordinator.
    marksix::statistics::AdaptiveCoordinator coordinator_;
    marksix::statistics::UpdateWorker updater_;
    marksix::statistics::LabControlWorker controlWorker_;
    std::future<marksix::statistics::LabControlReport> controlFuture_;
    Job job_;
    marksix::statistics::LatestWorker<PreparedStatistics> worker_;
    marksix::statistics::LatestWorker<SourceWorkResult> sourceWorker_;
    QTimer pollTimer_;
    uint64_t generation_ = 0;
    bool busy_ = false, closed_ = false;
    QString error_;
    QString updateStatus_;
    QString controlStatus_ = QStringLiteral("Fair-history controls not run automatically.");
    QString analysisStatus_;
    QString sourceWorkStatus_;
    std::optional<marksix::statistics::ImportPreview> preview_;
    PreparedStatistics dashboard_;
    ViewOptions options_;
    bool hadGoodData_ = false;
};
