#include "statistics_models.hpp"
#include "statistics_loader.hpp"
#include "statistics_data_root.hpp"

#include "adaptive.hpp"
#include "coverage.hpp"
#include "forecast_lab.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTemporaryDir>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>

using namespace marksix::statistics;

namespace {
int checks = 0;
const char* stage = "setup";
void check(bool condition) {
    ++checks;
    if (!condition) throw std::runtime_error(std::string("Qt statistics model ") + stage + " mismatch at check " + std::to_string(checks));
}
Result fixture(int day) {
    Result result;
    result.id = "03/00" + std::to_string(day);
    result.date = "2003-01-0" + std::to_string(day);
    result.main = {1, 2, 3, 4, 5, 6};
    result.extra = 7;
    result.source = {"synthetic-test", "https://example.invalid/test", "2026-09-25T00:00:00Z",
                     sha256("synthetic-test"), "test-only", Trust::OfficialRetrieval, "synthetic fixture"};
    return result;
}
Result seedFixture() {
    auto result = fixture(1);
    result.id = "02/053";
    result.date = "2002-07-04";
    return result;
}
PreparedStatistics prepared(const char* label) {
    PreparedStatistics out;
    const auto make = [label] {
        Presentation value;
        value.columns = {"Value"};
        value.rows = {{{label, {}}}};
        value.identities = {7};
        return value;
    };
    out.archive = make();
    out.analysis = make();
    out.forecastLab = make();
    out.learning = make();
    return out;
}
template<class Condition> void until(Condition condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 3000) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(condition());
}
class FixtureProvider final : public UpdateProvider {
public:
    bool enabled() const override { return true; }
    UpdatePolicy policy() const override {
        return {"synthetic-worker", "fixture-v1", {"2003-01-01", "2003-01-03"}, 1, 1};
    }
    std::optional<Result> latest(std::stop_token) override { return sourced(3); }
    FetchedWindow fetch(const DateRange& range, std::stop_token) override {
        return {range, sha256("fixture response"), {sourced(std::stoi(range.from.substr(8)))}};
    }
private:
    static Result sourced(int day) {
        auto result = fixture(day);
        result.source.sourceId = "synthetic-worker";
        result.source.contentSha256 = sha256("fixture response");
        return result;
    }
};
class BlockingProvider final : public UpdateProvider {
public:
    std::atomic<bool> entered{false};
    bool enabled() const override { return true; }
    UpdatePolicy policy() const override {
        return {"synthetic-worker", "fixture-v1", {"2003-01-01", "2003-01-03"}, 1, 1};
    }
    std::optional<Result> latest(std::stop_token stop) override {
        entered = true;
        while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        throw ProviderFailure(UpdateFailure::Cancelled);
    }
    FetchedWindow fetch(const DateRange&, std::stop_token) override { throw std::runtime_error("Unexpected fetch"); }
};
class FailingProvider final : public UpdateProvider {
public:
    explicit FailingProvider(UpdateFailure failure) : failure_(failure) {}
    bool enabled() const override { return true; }
    UpdatePolicy policy() const override {
        return {"synthetic-worker", "fixture-v1", {"2003-01-01", "2003-01-03"}, 1, 1};
    }
    std::optional<Result> latest(std::stop_token) override { throw ProviderFailure(failure_); }
    FetchedWindow fetch(const DateRange&, std::stop_token) override { throw std::runtime_error("Unexpected fetch"); }
private:
    UpdateFailure failure_;
};
void matches(const StatisticsTableModel& model, const Presentation& source, size_t identityBase = 0) {
    check(model.rowCount() == int(source.rows.size()));
    check(model.columnCount() == int(source.columns.size()));
    for (int col = 0; col < model.columnCount(); ++col)
        check(model.headerData(col, Qt::Horizontal, Qt::DisplayRole).toString().toStdString() == source.columns[size_t(col)]);
    for (int row = 0; row < model.rowCount(); ++row)
        for (int col = 0; col < model.columnCount(); ++col) {
            const auto index = model.index(row, col);
            const auto& expected = source.rows[size_t(row)][size_t(col)];
            check(model.data(index, StatisticsTableModel::DisplayRole).toString().toStdString() == expected.text);
            check(model.data(index, StatisticsTableModel::IdentityRole).toULongLong() == identityBase + source.identities[size_t(row)]);
            const auto numeric = model.data(index, StatisticsTableModel::NumericRole);
            check(numeric.isValid() == expected.numeric.has_value());
            if (expected.numeric) check(numeric.toDouble() == *expected.numeric);
        }
}
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        check(defaultStatisticsDataRoot().filename() == L"MarkSixEmulator");
        const auto snapshot = std::make_shared<const Snapshot>(std::vector<Result>{fixture(1), fixture(2), fixture(3)});
        ViewData data;
        data.snapshot = snapshot;
        data.coverage = coverage(*snapshot);
        data.analysis = analyze(*snapshot);
        StatisticsModels models;

        stage = "archive";
        auto archive = present(data, Page::History);
        models.archive()->setPresentation(archive);
        matches(*models.archive(), archive);
        models.archive()->sort(1, Qt::DescendingOrder);
        sortRows(archive, 1, true);
        matches(*models.archive(), archive);
        check(models.archive()->data(models.archive()->index(0, 0), StatisticsTableModel::IdentityRole).toULongLong() == 2);
        models.archive()->setPresentation(present(data, Page::History, 1, Era::EarlierUnreviewed));
        check(models.archive()->rowCount() == 0);
        data.historyFrom = "2003-01-02";
        data.historyThrough = "2003-01-02";
        const auto filteredHistory = present(data, Page::History);
        check(filteredHistory.rows.size() == 1);
        check(filteredHistory.identities.front() == 1);
        check(filteredHistory.rows.front()[1].text == "2003-01-02");
        data.historyFrom.clear();
        data.historyThrough.clear();

        stage = "analysis";
        auto analysis = present(data, Page::Numbers);
        models.analysis()->setPresentation(analysis);
        matches(*models.analysis(), analysis);
        models.analysis()->sort(0, Qt::DescendingOrder);
        sortRows(analysis, 0, true);
        matches(*models.analysis(), analysis);
        check(models.analysis()->data(models.analysis()->index(0, 0), StatisticsTableModel::IdentityRole).toULongLong() == 48);

        stage = "lab";
        const Snapshot modelSnapshot({seedFixture(), fixture(1), fixture(2)});
        const auto labReplay = replayLab(modelSnapshot);
        check(labReplay.available && labReplay.next.has_value() && labReplay.steps.size() >= 2);
        auto lab = std::make_shared<LabWorkspace>();
        lab->result.available = true;
        lab->result.replay = labReplay;
        lab->historyOffset = 100;
        for (const auto& step : labReplay.steps) {
            LabStoredForecast record;
            record.forecast = step.frozen;
            record.outcome = step.actual;
            lab->history.push_back(std::move(record));
        }
        data.lab = lab;
        auto labPage = present(data, Page::Lab);
        check(labPage.identities.front() == 0 && labPage.identities.back() == 1);
        models.forecastLab()->setPresentation(labPage, lab->historyOffset);
        matches(*models.forecastLab(), labPage, lab->historyOffset);
        models.forecastLab()->sort(0, Qt::DescendingOrder);
        sortRows(labPage, 0, true);
        matches(*models.forecastLab(), labPage, lab->historyOffset);

        stage = "learning";
        const auto adaptiveReplay = replayAdaptive(modelSnapshot);
        check(adaptiveReplay.available && adaptiveReplay.steps.size() >= 2);
        auto adaptive = std::make_shared<AdaptiveWorkspace>();
        adaptive->historyOffset = 200;
        for (const auto& step : adaptiveReplay.steps) {
            AdaptiveStoredPrediction record;
            record.before = step.before;
            record.prediction = step.prediction;
            record.outcome = step;
            adaptive->history.push_back(std::move(record));
        }
        data.adaptive = adaptive;
        auto learning = present(data, Page::Learning);
        check(learning.identities.front() == 0 && learning.identities.back() == 1);
        models.learning()->setPresentation(learning, adaptive->historyOffset);
        matches(*models.learning(), learning, adaptive->historyOffset);
        models.learning()->sort(0, Qt::DescendingOrder);
        sortRows(learning, 0, true);
        matches(*models.learning(), learning, adaptive->historyOffset);

        stage = "validation";
        Presentation malformed;
        malformed.columns = {"a"};
        malformed.rows = {{{"x", {}}}};
        bool rejected = false;
        try { models.archive()->setPresentation(std::move(malformed)); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected && models.archive()->rowCount() == 0);

        stage = "worker generations";
        std::atomic<int> calls{0};
        std::atomic<bool> firstEntered{false};
        bool publishedOnUiThread = false;
        QObject::connect(models.archive(), &QAbstractItemModel::modelReset, &application, [&] {
            publishedOnUiThread = QThread::currentThread() == application.thread();
        });
        StatisticsLoader loader(StatisticsLoader::Job([&](std::stop_token stop) {
            const int call = ++calls;
            if (call == 1) {
                firstEntered = true;
                while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return prepared("obsolete");
            }
            return prepared("current");
        }), models);
        loader.reload();
        until([&] { return firstEntered.load(); });
        loader.reload();
        until([&] { return !loader.busy(); });
        check(loader.error().isEmpty());
        check(publishedOnUiThread);
        check(models.archive()->data(models.archive()->index(0, 0)).toString() == QStringLiteral("current"));
        check(models.forecastLab()->data(models.forecastLab()->index(0, 0), StatisticsTableModel::IdentityRole).toULongLong() == 7);
        loader.cancel();
        loader.close();

        stage = "worker failure retention";
        StatisticsLoader failing(StatisticsLoader::Job([](std::stop_token) {
            auto out = prepared("should not publish");
            out.learning.identities.clear();
            return out;
        }), models);
        failing.reload();
        until([&] { return !failing.busy(); });
        check(!failing.error().isEmpty());
        check(models.archive()->data(models.archive()->index(0, 0)).toString() == QStringLiteral("current"));
        failing.close();

        stage = "committed update then model failure";
        QTemporaryDir temporary;
        check(temporary.isValid());
        const auto root = std::filesystem::path(temporary.path().toStdWString());
        StatisticsLoader update(root, std::make_shared<FixtureProvider>(),
            StatisticsLoader::Job([](std::stop_token) {
                auto out = prepared("invalid after commit");
                out.learning.identities.clear();
                return out;
            }), models);
        check(update.startUpdate());
        until([&] { return !update.updateBusy() && !update.busy() && !update.error().isEmpty(); });
        Archive committed(root);
        check(committed.snapshot().records().size() == 3);
        check(committed.committedWindows().size() == 3);
        check(committed.updateAttempts().back().status == "succeeded");
        check(update.updateStatus().contains(QStringLiteral("New 3")));
        check(models.archive()->data(models.archive()->index(0, 0)).toString() == QStringLiteral("current"));
        check(update.startUpdate());
        until([&] { return !update.updateBusy() && update.updateStatus().contains(QStringLiteral("No new records")); });
        check(committed.snapshot().records().size() == 3);
        update.close();

        stage = "offline TLS and schema update categories";
        for (auto failure : {UpdateFailure::Connection, UpdateFailure::Tls, UpdateFailure::Schema}) {
            QTemporaryDir failedDirectory;
            check(failedDirectory.isValid());
            const auto failedRoot = std::filesystem::path(failedDirectory.path().toStdWString());
            StatisticsLoader failedUpdate(failedRoot, std::make_shared<FailingProvider>(failure),
                StatisticsLoader::Job([](std::stop_token) { return prepared("unused"); }), models);
            check(failedUpdate.startUpdate());
            until([&] { return !failedUpdate.updateBusy() &&
                failedUpdate.updateStatus() != QStringLiteral("Checking source and local history..."); });
            check(Archive(failedRoot).snapshot().records().empty());
            check(!failedUpdate.updateStatus().isEmpty());
            failedUpdate.close();
        }

        stage = "cancel and close";
        std::atomic<bool> cancelEntered{false};
        StatisticsLoader cancelled(StatisticsLoader::Job([&](std::stop_token stop) {
            cancelEntered = true;
            while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return prepared("cancelled");
        }), models);
        cancelled.reload();
        until([&] { return cancelEntered.load(); });
        cancelled.cancel();
        until([&] { return !cancelled.busy(); });
        QCoreApplication::processEvents();
        check(models.archive()->data(models.archive()->index(0, 0)).toString() == QStringLiteral("current"));
        cancelled.close();

        QTemporaryDir closeDirectory;
        check(closeDirectory.isValid());
        const auto closeRoot = std::filesystem::path(closeDirectory.path().toStdWString());
        auto blockingProvider = std::make_shared<BlockingProvider>();
        StatisticsLoader closing(closeRoot, blockingProvider,
            StatisticsLoader::Job([](std::stop_token) { return prepared("never loaded"); }), models);
        check(closing.startUpdate());
        until([&] { return blockingProvider->entered.load(); });
        closing.close();
        Archive afterClose(closeRoot);
        check(afterClose.snapshot().records().empty());
        check(afterClose.committedWindows().empty());

        stage = "dashboard empty and warm-up";
        QTemporaryDir emptyDirectory;
        check(emptyDirectory.isValid());
        StatisticsLoader empty(std::filesystem::path(emptyDirectory.path().toStdWString()), models);
        empty.reload();
        until([&] { return !empty.busy(); });
        check(empty.error().isEmpty() && !empty.selectionAvailable());
        check(empty.sourceStatus().contains(QStringLiteral("0 archived")));
        empty.close();

        QTemporaryDir warmDirectory;
        check(warmDirectory.isValid());
        const auto warmRoot = std::filesystem::path(warmDirectory.path().toStdWString());
        { Archive warmArchive(warmRoot); warmArchive.ingest({seedFixture(), fixture(1)}); }
        StatisticsLoader warm(warmRoot, models);
        warm.reload();
        until([&] { return !warm.busy(); });
        check(warm.error().isEmpty() && warm.selectionAvailable());
        check(warm.selectionState().contains(QStringLiteral("Warm-up")));
        check(warm.suggestedMain().size() == 6);
        check(models.labProbabilities()->rowCount() == 49);
        check(models.adaptive()->rowCount() == 49);
        check(models.fixed()->rowCount() == 49);
        check(models.exact()->rowCount() == 49);
        check(models.calibration()->rowCount() == 60);
        const auto fairMain = models.labProbabilities()->data(
            models.labProbabilities()->index(0, 2), StatisticsTableModel::NumericRole).toDouble();
        check(fairMain == 6.0 / 49.0);
        check(models.labProbabilities()->headerData(1, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Selected main"));
        check(!warm.controlEligible());
        warm.close();

        stage = "dashboard stale";
        int staleCalls = 0;
        StatisticsLoader stale(StatisticsLoader::Job([&](std::stop_token) {
            if (++staleCalls == 1) {
                auto out = prepared("first good");
                out.selectionAvailable = true;
                out.suggestedMain = {1, 2, 3, 4, 5, 6};
                return out;
            }
            throw std::runtime_error("Injected reload failure");
        }), models);
        stale.reload();
        until([&] { return !stale.busy(); });
        check(stale.error().isEmpty() && stale.selectionAvailable());
        stale.reload();
        until([&] { return !stale.busy(); });
        check(stale.error().contains(QStringLiteral("Injected reload failure")));
        check(stale.selectionState().contains(QStringLiteral("stale")));
        check(stale.suggestedMain()[0].toInt() == 1);
        stale.close();

        stage = "opt-in fair controls";
        QTemporaryDir controlDirectory;
        check(controlDirectory.isValid());
        StatisticsLoader controls(std::filesystem::path(controlDirectory.path().toStdWString()),
            std::shared_ptr<UpdateProvider>{}, StatisticsLoader::Job([](std::stop_token) {
                auto out = prepared("controls fixture");
                out.controlEligible = true;
                out.controlHistoryLength = 401;
                out.controlObservedStatistic = 0;
                out.controlDigest = sha256("synthetic controls history");
                return out;
            }), models);
        controls.reload();
        until([&] { return !controls.busy(); });
        check(controls.controlEligible() && !controls.controlBusy());
        check(controls.startFairControls());
        check(controls.controlBusy());
        controls.cancelFairControls();
        until([&] { return !controls.controlBusy(); });
        controls.close();

        stage = "source import preview/accept and stale resolve";
        QTemporaryDir sourceDirectory;
        check(sourceDirectory.isValid());
        const auto sourceRoot = std::filesystem::path(sourceDirectory.path().toStdWString());
        const auto csvPath = sourceRoot / "synthetic.csv";
        const auto manifestPath = sourceRoot / "synthetic.json";
        const std::string csv = "draw_id,date,n1,n2,n3,n4,n5,n6,extra,era,provider_id,source_window\r\n"
            "03/001,2003-01-01,1,2,3,4,5,6,7,49-number,synthetic,fixture\r\n";
        {
            std::ofstream(csvPath, std::ios::binary) << csv;
            std::ofstream(manifestPath, std::ios::binary) <<
                "{\"schema\":\"marksix-csv-v1\",\"csvSha256\":\"" << sha256(csv) <<
                "\",\"sourceId\":\"qt-import-synthetic\",\"url\":\"https://example.invalid/qt-import\","
                "\"retrievedUtc\":\"2003-01-03T00:00:00Z\",\"lineage\":\"synthetic-test-only\","
                "\"status\":\"unverified\",\"extractionOrder\":\"unknown\"}";
        }
        StatisticsLoader sources(sourceRoot, models);
        const auto csvUrl = QUrl::fromLocalFile(QString::fromStdWString(csvPath.wstring()));
        const auto manifestUrl = QUrl::fromLocalFile(QString::fromStdWString(manifestPath.wstring()));
        check(sources.previewCsv(csvUrl, manifestUrl));
        until([&] { return sources.importPreviewReady(); });
        check(sources.sourceWorkStatus().contains(QStringLiteral("Preview 1 rows")));
        check(sources.acceptCsv());
        until([&] { return !sources.sourceWorkBusy() && !sources.busy() &&
            sources.sourceWorkStatus().contains(QStringLiteral("Import committed")); });
        check(Archive(sourceRoot).snapshot().records().size() == 1);
        check(!sources.importPreviewReady());
        check(sources.resolveConflict(0, QStringLiteral("reason"), QStringLiteral("evidence")));
        until([&] { return sources.sourceWorkStatus().contains(QStringLiteral("no unresolved conflict")); });
        check(Archive(sourceRoot).snapshot().records().size() == 1);
        auto correction = fixture(1);
        correction.extra = 8;
        { Archive conflictArchive(sourceRoot); conflictArchive.ingest({correction}); }
        sources.reload();
        until([&] { return !sources.busy(); });
        check(sources.error().isEmpty() && models.sources()->rowCount() == 2);
        const auto revisions = Archive(sourceRoot).revisions();
        const auto picked = std::find_if(revisions.begin(), revisions.end(),
            [](const StoredRevision& row) { return row.result.extra == 8; });
        check(picked != revisions.end());
        const int selected = int(std::distance(revisions.begin(), picked));
        check(sources.resolveConflict(selected, QStringLiteral("synthetic correction"),
            QStringLiteral("https://example.invalid/decision")));
        until([&] { return sources.sourceWorkStatus().contains(QStringLiteral("Conflict resolution recorded")); });
        until([&] { return !sources.busy(); });
        const auto resolved = Archive(sourceRoot).readState();
        check(resolved.resolutions.size() == 1);
        check(resolved.resolutions[0].selectedDigest == picked->digest);
        check(models.sources()->detailForId(qulonglong(selected)).contains(QStringLiteral("synthetic correction")));
        sources.cancelSourceWork();
        sources.close();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
