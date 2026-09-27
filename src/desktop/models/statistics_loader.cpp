#include "statistics_loader.hpp"

#include "analysis.hpp"
#include "archive.hpp"
#include "coverage.hpp"
#include "forecast.hpp"
#include "forecast_lab_ledger.hpp"
#include "hkjc_provider.hpp"
#include "learning_progress.hpp"

#include <algorithm>
#include <stdexcept>

using namespace marksix::statistics;
namespace {
bool valid(const Presentation& value) {
    if (value.rows.size() != value.identities.size()) return false;
    for (const auto& row : value.rows) if (row.size() != value.columns.size()) return false;
    return true;
}
}

StatisticsLoader::StatisticsLoader(std::filesystem::path root, StatisticsModels& models, QObject* parent)
    : QObject(parent), models_(models), root_(std::move(root)),
      updater_(makeHkjcProvider(), {}, [this](const ArchiveState& state, int64_t attempt, std::stop_token stop) {
          coordinator_.prepare(root_, state, attempt, stop).get();
      }),
      controlWorker_(root_) {
    pollTimer_.setInterval(20);
    connect(&pollTimer_, &QTimer::timeout, this, &StatisticsLoader::poll);
}

StatisticsLoader::StatisticsLoader(Job injectedJob, StatisticsModels& models, QObject* parent)
    : QObject(parent), models_(models), controlWorker_({}), job_(std::move(injectedJob)) {
    pollTimer_.setInterval(20);
    connect(&pollTimer_, &QTimer::timeout, this, &StatisticsLoader::poll);
}

StatisticsLoader::StatisticsLoader(std::filesystem::path root, std::shared_ptr<UpdateProvider> provider,
                                   Job injectedJob, StatisticsModels& models, QObject* parent)
    : QObject(parent), models_(models), root_(std::move(root)),
      updater_(std::move(provider), {}, [this](const ArchiveState& state, int64_t attempt, std::stop_token stop) {
          coordinator_.prepare(root_, state, attempt, stop).get();
      }), controlWorker_(root_), job_(std::move(injectedJob)) {
    pollTimer_.setInterval(20);
    connect(&pollTimer_, &QTimer::timeout, this, &StatisticsLoader::poll);
}

StatisticsLoader::~StatisticsLoader() { close(); }

QVariantList StatisticsLoader::suggestedMain() const {
    QVariantList values;
    for (int number : dashboard_.suggestedMain) values.push_back(number);
    return values;
}
QString StatisticsLoader::selectionState() const { return QString::fromStdString(dashboard_.selectionState); }
QString StatisticsLoader::selectedModel() const { return QString::fromStdString(dashboard_.selectedModel); }
QString StatisticsLoader::cutoff() const { return QString::fromStdString(dashboard_.cutoff); }
QString StatisticsLoader::evidence() const { return QString::fromStdString(dashboard_.evidence); }
QString StatisticsLoader::sourceStatus() const { return QString::fromStdString(dashboard_.sourceStatus); }
QStringList StatisticsLoader::eraFirstDates() const {
    return {
        QString::fromStdString(dashboard_.eraFirstDates[0]),
        QString::fromStdString(dashboard_.eraFirstDates[1])
    };
}
QStringList StatisticsLoader::eraLastDates() const {
    return {
        QString::fromStdString(dashboard_.eraLastDates[0]),
        QString::fromStdString(dashboard_.eraLastDates[1])
    };
}
QVariantList StatisticsLoader::progressPoints() const {
    QVariantList values;
    for (double point : dashboard_.progressPoints) values.push_back(point);
    return values;
}
QString StatisticsLoader::progressSummary() const { return QString::fromStdString(dashboard_.progressSummary); }
QString StatisticsLoader::comparisonSummary() const { return QString::fromStdString(dashboard_.comparisonSummary); }
QVariantList StatisticsLoader::adaptiveMain() const {
    QVariantList values; for (int number : dashboard_.adaptiveMain) values.push_back(number); return values;
}
QVariantList StatisticsLoader::fixedMain() const {
    QVariantList values; for (int number : dashboard_.fixedMain) values.push_back(number); return values;
}
QStringList StatisticsLoader::learningLineages() const {
    QStringList values;
    for (const auto& name : dashboard_.learningLineages) values.push_back(QString::fromStdString(name));
    return values;
}

void StatisticsLoader::chooseLearningLineage(int index) {
    if (index < 0 || size_t(index) >= dashboard_.learningLineageIds.size()) return;
    options_.learningLineage = dashboard_.learningLineageIds[size_t(index)];
    options_.learningOffset = 0;
    reload();
}

void StatisticsLoader::pageLearning(int direction) {
    if (dashboard_.learningCount == 0) return;
    const auto last = dashboard_.learningCount > 100 ? dashboard_.learningCount - 100 : 0;
    const auto current = dashboard_.learningOffset;
    if (direction == 0) options_.learningOffset = 0;
    else if (direction == 1) options_.learningOffset = current > 100 ? current - 100 : 0;
    else if (direction == 2) options_.learningOffset = std::min(last, current + 100);
    else if (direction == 3) options_.learningOffset = last;
    else return;
    if (options_.learningOffset != current) reload();
}

void StatisticsLoader::setProgressView(int evidence, int mode) {
    if (evidence < 0 || evidence > 1 || mode < 0 || mode > 3) return;
    if (options_.progressEvidence == evidence && options_.progressMode == mode) return;
    options_.progressEvidence = evidence;
    options_.progressMode = mode;
    reload();
}

bool StatisticsLoader::setAnalysisSettings(int scope, int lastN, const QString& from,
                                           const QString& through, int number, int era) {
    const auto first = from.trimmed().toStdString(), last = through.trimmed().toStdString();
    if (scope < 0 || scope > 2 || lastN < 0 || lastN > 10000 || number < 1 || number > 49 ||
        era < 0 || era > 1 || (!first.empty() && !validDate(first)) ||
        (!last.empty() && !validDate(last)) || (!first.empty() && !last.empty() && first > last)) {
        analysisStatus_ = QStringLiteral("Invalid analysis controls. Use YYYY-MM-DD dates in ascending order.");
        emit stateChanged();
        return false;
    }
    options_.scope = scope;
    options_.lastN = size_t(lastN);
    options_.from = first;
    options_.through = last;
    options_.selectedNumber = number;
    options_.era = era;
    analysisStatus_ = QStringLiteral("Applying local history and analysis filters. Forecast training is unchanged.");
    reload();
    return true;
}

bool StatisticsLoader::previewCsv(const QUrl& csv, const QUrl& manifest) {
    if (closed_ || root_.empty() || updater_.busy() || sourceWorker_.busy() ||
        !csv.isLocalFile() || !manifest.isLocalFile()) return false;
    preview_.reset();
    const auto csvPath = std::filesystem::path(csv.toLocalFile().toStdWString());
    const auto manifestPath = std::filesystem::path(manifest.toLocalFile().toStdWString());
    const auto root = root_;
    sourceWorker_.submit([root, csvPath, manifestPath](std::stop_token stop) {
        if (stop.stop_requested()) throw std::runtime_error("Import preview cancelled");
        Archive archive(root);
        SourceWorkResult result;
        result.preview = previewImport(archive, readBounded(csvPath), readBounded(manifestPath, 65536));
        result.status = "Preview " + std::to_string(result.preview->rows.size()) + " rows | " +
            std::to_string(result.preview->duplicates) + " duplicates | " +
            std::to_string(result.preview->conflicts) + " conflicts | " +
            std::to_string(result.preview->earlierQuarantined) + " earlier/quarantined. Unverified import; review before acceptance.";
        return result;
    });
    sourceWorkStatus_ = QStringLiteral("Validating CSV and manifest locally...");
    pollTimer_.start(); emit stateChanged(); return true;
}

bool StatisticsLoader::acceptCsv() {
    if (closed_ || !preview_ || sourceWorker_.busy() || updater_.busy()) return false;
    const auto root = root_;
    const auto preview = *preview_;
    preview_.reset();
    sourceWorker_.submit([root, preview](std::stop_token stop) {
        Archive archive(root);
        const auto outcome = acceptImport(archive, preview, stop);
        SourceWorkResult result;
        result.committed = true;
        result.status = "Import committed: " + std::to_string(outcome.inserted) + " new, " +
            std::to_string(outcome.duplicate) + " duplicates, " +
            std::to_string(outcome.conflicts) + " conflicts. Refreshing local views.";
        return result;
    });
    sourceWorkStatus_ = QStringLiteral("Accepting validated import atomically...");
    pollTimer_.start(); emit stateChanged(); return true;
}

void StatisticsLoader::cancelSourceWork() {
    if (closed_) return;
    sourceWorker_.cancel();
    preview_.reset();
    sourceWorkStatus_ = QStringLiteral("Import/resolution cancelled; an already committed transaction remains durable. Reload to inspect data.");
    emit stateChanged();
}
bool StatisticsLoader::resolveConflict(int revisionIndex, const QString& reason, const QString& evidence) {
    if (closed_ || revisionIndex < 0 || root_.empty() || updater_.busy() || sourceWorker_.busy()) return false;
    const auto why = reason.trimmed().toStdString(), source = evidence.trimmed().toStdString();
    if (why.empty() || source.empty()) {
        sourceWorkStatus_ = QStringLiteral("Enter a resolution reason and source evidence first.");
        emit stateChanged(); return false;
    }
    const auto root = root_;
    const auto version = dashboard_.sourceVersion;
    sourceWorker_.submit([root, version, revisionIndex, why, source](std::stop_token stop) {
        Archive archive(root);
        const auto state = archive.readState();
        if (state.version != version || size_t(revisionIndex) >= state.revisions.size())
            throw std::runtime_error("Source selection is stale; reload before resolving");
        const auto& revision = state.revisions[size_t(revisionIndex)];
        if (!revision.result.unresolvedConflict)
            throw std::runtime_error("Selected revision has no unresolved conflict");
        archive.resolve(drawKey(revision.result), revision.digest, why, source, stop, version);
        SourceWorkResult result;
        result.committed = true;
        result.status = "Conflict resolution recorded with reason and source evidence; old revisions retained.";
        return result;
    });
    sourceWorkStatus_ = QStringLiteral("Recording explicit conflict resolution...");
    pollTimer_.start(); emit stateChanged(); return true;
}

void StatisticsLoader::reload() {
    if (closed_) return;
    const auto options = options_;
    generation_ = worker_.submit(job_ ? job_ : Job([this, options](std::stop_token stop) {
        return prepare(root_, coordinator_, stop, options);
    }));
    busy_ = true;
    error_.clear();
    pollTimer_.start();
    emit stateChanged();
}

void StatisticsLoader::cancel() {
    if (closed_) return;
    worker_.cancel();
    ++generation_;
    busy_ = false;
    if (!updater_.busy()) pollTimer_.stop();
    emit stateChanged();
}

bool StatisticsLoader::startUpdate() {
    if (closed_ || root_.empty() || sourceWorker_.busy() || preview_ ||
        !updater_.available() || !updater_.start(root_)) return false;
    updateStatus_ = QStringLiteral("Checking source and local history...");
    pollTimer_.start();
    emit stateChanged();
    return true;
}

void StatisticsLoader::cancelUpdate() { if (!closed_) updater_.cancel(); }

bool StatisticsLoader::startFairControls() {
    if (closed_ || !dashboard_.controlEligible || controlWorker_.progress().active) return false;
    try {
        controlFuture_ = controlWorker_.start(dashboard_.controlHistoryLength,
            dashboard_.controlObservedStatistic, dashboard_.controlDigest);
        controlStatus_ = QStringLiteral("Running separate fair-history controls...");
        pollTimer_.start();
        emit stateChanged();
        return true;
    } catch (const std::exception& error) {
        controlStatus_ = QString::fromUtf8(error.what());
        emit stateChanged();
        return false;
    }
}

void StatisticsLoader::cancelFairControls() { if (!closed_) controlWorker_.cancel(); }

void StatisticsLoader::close() {
    if (closed_) return;
    closed_ = true;
    pollTimer_.stop();
    updater_.close();
    sourceWorker_.close();
    worker_.close();
    controlWorker_.cancel();
    if (controlFuture_.valid()) controlFuture_.wait();
    coordinator_.close();
    busy_ = false;
}

void StatisticsLoader::poll() {
    if (closed_) return;
    if (auto source = sourceWorker_.poll()) {
        if (!source->error.empty()) sourceWorkStatus_ = QString::fromStdString(source->error);
        else if (source->value) {
            preview_ = source->value->preview;
            sourceWorkStatus_ = QString::fromStdString(source->value->status);
            if (source->value->committed) reload();
        }
        emit stateChanged();
    }
    if (controlFuture_.valid() && controlFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        try {
            const auto report = controlFuture_.get();
            controlStatus_ = QString::fromStdString("Fair-history controls " +
                std::to_string(report.completed) + "/" + std::to_string(report.requested) +
                "; Monte Carlo right-tail " + std::to_string(report.tailFraction) +
                " ± simulation SE " + std::to_string(report.simulationStdError) +
                ". Not a next-draw winning probability.");
        } catch (const std::exception& error) { controlStatus_ = QString::fromUtf8(error.what()); }
        emit stateChanged();
    } else if (controlWorker_.progress().active) {
        const auto progress = controlWorker_.progress();
        const auto status = QString::fromStdString("Fair-history controls " + std::to_string(progress.completed) +
            "/" + std::to_string(progress.requested) + " complete...");
        if (status != controlStatus_) { controlStatus_ = status; emit stateChanged(); }
    }
    const auto progress = updater_.progress();
    if (updater_.busy()) {
        const auto status = QString::fromStdString(updateProgressText(progress));
        if (status != updateStatus_) {
            updateStatus_ = status;
            emit stateChanged();
        }
    }
    if (auto update = updater_.takeResult()) {
        updateStatus_ = QString::fromStdString(updateResultText(*update));
        if (update->committed) reload(); // Archive commit is final even if later model loading fails.
        emit stateChanged();
    }
    auto result = worker_.poll();
    if (!result || result->generation != generation_) {
        if (!busy_ && !updater_.busy() && !sourceWorker_.busy() && !controlWorker_.progress().active && !controlFuture_.valid()) pollTimer_.stop();
        return;
    }
    if (!updater_.busy() && !sourceWorker_.busy() && !controlWorker_.progress().active && !controlFuture_.valid()) pollTimer_.stop();
    busy_ = false;
    if (!result->error.empty()) {
        error_ = QString::fromStdString(result->error);
        if (hadGoodData_) dashboard_.selectionState = "Previously loaded data; reload failed (stale)";
    } else if (result->value) {
        const auto& p = *result->value;
        if (!valid(p.archive) || !valid(p.sources) || !valid(p.analysis) || !valid(p.pairs) || !valid(p.rolling) ||
            !valid(p.forecastLab) || !valid(p.learning) ||
            !valid(p.progress) ||
            !valid(p.labProbabilities) || !valid(p.adaptive) || !valid(p.fixed) ||
            !valid(p.exact) || !valid(p.calibration))
            error_ = QStringLiteral("Prepared statistics presentation is inconsistent; previously displayed data retained.");
        else {
            models_.archive()->setPresentation(p.archive);
            models_.sources()->setPresentation(p.sources);
            models_.analysis()->setPresentation(p.analysis);
            models_.pairs()->setPresentation(p.pairs);
            models_.rolling()->setPresentation(p.rolling);
            models_.forecastLab()->setPresentation(p.forecastLab, p.labOffset);
            models_.learning()->setPresentation(p.learning, p.learningOffset);
            models_.progress()->setPresentation(p.progress);
            models_.labProbabilities()->setPresentation(p.labProbabilities);
            models_.adaptive()->setPresentation(p.adaptive);
            models_.fixed()->setPresentation(p.fixed);
            models_.exact()->setPresentation(p.exact);
            models_.calibration()->setPresentation(p.calibration);
            dashboard_ = p;
            controlStatus_ = QString::fromStdString(p.controlSummary);
            hadGoodData_ = true;
            error_.clear();
        }
    }
    emit stateChanged();
}
