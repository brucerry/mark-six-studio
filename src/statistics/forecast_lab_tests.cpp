#include "forecast_lab.hpp"
#include "forecast_lab_storage.hpp"
#include "forecast_lab_ledger.hpp"
#include "forecast_lab_control_worker.hpp"
#include "archive.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <sqlite3.h>
#include <iostream>
#include <stdexcept>

using namespace marksix::statistics;
namespace {
size_t checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
void near(double actual, double expected, const char* message) {
    check(std::isfinite(actual) && std::abs(actual - expected) < 1e-12, message);
}
Result sample(const char* date = "2002-07-04", const char* id = "02/053") {
    Result r;
    r.date = date; r.id = id; r.main = {1,2,3,4,5,6}; r.extra = 7;
    r.source = {"lab-test", "https://example.invalid/lab", "2026-09-27T00:00:00Z",
                sha256("synthetic-lab"), "fixture", Trust::OfficialRetrieval, "synthetic-only"};
    return r;
}
}
size_t testForecastLab() {
    const auto& registry = labCandidates();
    check(registry.size() == 15 && registry[0].kind == LabKind::Uniform &&
          registry[1].kind == LabKind::Adaptive && registry[2].kind == LabKind::Fixed,
          "Forecast Lab registry and unchanged reference identities");
    check(registry[3].id == "mix-h25-p100-e025" &&
          registry[14].id == "mix-h400-p400-e100", "Registry enumeration is stable");
    check(labProtocolDigest() == "760587d774a0b281361c4e9b7c60884e39e0fd25327ea9dc23d9cd466126f388",
          "Canonical registry protocol digest");
    const auto& candidate = registry[3];
    const auto state = labSeed(candidate, sample());
    check(state.draws == 1 && state.lastDate == "2002-07-04", "Verified first draw seeds the model");
    const auto p = labPredict(state);
    double main = 0, extra = 0;
    for (size_t i = 0; i < 49; ++i) {
        near(p[i].main + p[i].extra + p[i].absent, 1.0, "Category probabilities sum to one");
        main += p[i].main; extra += p[i].extra;
        near(p[i].main, (2.0/3.0) * 6.0/49 + (1.0/3.0) * ((i < 6 ? 1.0 : 0.0) + 100 * 6.0/49)/101,
             "Independent smoothed main probability");
        near(p[i].extra, (2.0/3.0) / 49 + (1.0/3.0) * ((i == 6 ? 1.0 : 0.0) + 100.0/49)/101,
             "Independent smoothed Extra probability");
    }
    near(main, 6, "Main inclusion sums to six"); near(extra, 1, "Extra sums to one");
    check(labRank(p).main == std::array<int,6>{1,2,3,4,5,6} && labRank(p).extra == 7,
          "Deterministic ranked first six and distinct Extra");
    check(labRank(Probabilities{}).main == std::array<int,6>{1,2,3,4,5,6},
          "Stable number-ascending ties");
    auto a = state, b = state;
    labObserve(a, sample("2002-07-05", "02/054"));
    auto revisedMetadata = sample("2002-07-05", "02/054");
    revisedMetadata.source.retrievedUtc = "2026-09-27T01:00:00Z";
    labObserve(b, revisedMetadata);
    check(a.prefixDigest == b.prefixDigest && a.logWeights == b.logWeights,
          "Source metadata refresh does not retrain or change value identity");
    const double effective = 1 + std::exp(-std::log(2.0)/25);
    near(a.counts[0].n, 2, "Expanding count after update");
    near(a.counts[1].n, effective, "Half-life count after update");
    check(a.logWeights[0] < 0 && a.logWeights[1] == 0 && a.logWeights[2] == 0,
          "Once-per-result log-loss update favors matching experts");
    bool rejected = false;
    try { labSeed(registry[3], sample("2002-07-05", "02/054")); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Wrong first draw rejected");
    rejected = false;
    try { labObserve(a, sample("2002-07-05", "02/054")); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Duplicate observation rejected");
    std::vector<LabScoreRow> history;
    for (size_t ordinal = 201; ordinal <= 500; ++ordinal) {
        LabScoreRow row;
        row.ordinal = ordinal;
        row.available.fill(true);
        row.mainHits[ordinal <= 400 ? 3 : 14] = ordinal <= 400 ? 1 : 2;
        history.push_back(row);
    }
    check(labChoose(history, 399).fallback, "Warm-up ends only at 400 observations");
    const auto first = labChoose(history, 400);
    check(!first.fallback && first.candidate == 3 && first.validationFirst == 201 &&
          first.validationLast == 400 && first.validationTargets == 200,
          "First validation uses targets 201 through 400 exactly");
    check(labChoose(history, 450).candidate == 3,
          "Choice remains frozen through the following 100 targets");
    const auto next = labChoose(history, 500);
    check(!next.fallback && next.candidate == 14 && next.validationFirst == 301 &&
          next.validationLast == 500, "Next boundary uses the previous 200 targets");
    auto shortHistory = history;
    shortHistory.erase(shortHistory.begin() + 199);
    check(labChoose(shortHistory, 400).fallback, "Insufficient inner coverage uses Adaptive");
    shortHistory = history;
    shortHistory.erase(shortHistory.begin() + 100);
    rejected = false;
    try { labChoose(shortHistory, 400); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Internal validation gap is rejected explicitly");
    auto tied = history;
    for (auto& row : tied) row.mainHits.fill(0);
    check(labChoose(tied, 400).fallback, "No historical improvement uses Adaptive");
    for (auto& row : tied) { row.mainHits[3] = 1; row.mainHits[4] = 1; }
    check(labChoose(tied, 400).candidate == 3, "Stable candidate ID breaks equal scores");
    for (auto& row : tied) row.logLoss[4] = -0.01;
    check(labChoose(tied, 400).candidate == 4, "Lower log loss breaks equal hit scores");
    const auto seed = sample();
    const auto second = sample("2002-07-05", "02/054");
    auto third = sample("2002-07-06", "02/055");
    third.main = {2, 8, 13, 21, 34, 49}; third.extra = 1;
    auto appended = labStart(seed);
    check(appended.next && appended.next->targetOrdinal == 2 &&
          appended.next->decision.fallback && !appended.next->available[2],
          "Seed has only prior information and fixed baseline is gated");
    const auto firstForecast = *appended.next;
    labAppend(appended, second);
    const auto secondForecast = *appended.next;
    labAppend(appended, third);
    auto alteredThird = third;
    alteredThird.main = {7, 12, 18, 29, 35, 47}; alteredThird.extra = 3;
    auto changedFuture = labStart(seed);
    labAppend(changedFuture, second);
    labAppend(changedFuture, alteredThird);
    check(appended.steps[0].frozen.prefixDigest == firstForecast.prefixDigest &&
          appended.steps[1].frozen.prefixDigest == secondForecast.prefixDigest &&
          changedFuture.steps[0].frozen.selections[1].main == appended.steps[0].frozen.selections[1].main &&
          changedFuture.steps[1].frozen.selections[1].main == appended.steps[1].frozen.selections[1].main &&
          changedFuture.steps[1].frozen.decision.candidate == appended.steps[1].frozen.decision.candidate,
          "Later outcome changes cannot revise prior predictions or selections");
    const auto full = replayLab(Snapshot({seed, second, third}));
    check(full.available && full.steps.size() == appended.steps.size() &&
          full.adaptive.prefixDigest == appended.adaptive.prefixDigest &&
          full.next->selections[1].main == appended.next->selections[1].main &&
          full.scoreHistory[1].logLoss == appended.scoreHistory[1].logLoss,
          "Strict append is identical to clean chronological replay");
    bool appendRejected = false;
    try { labAppend(appended, third); }
    catch (const std::invalid_argument&) { appendRejected = true; }
    check(appendRejected, "Duplicate target cannot be trained twice");
    const auto restored = decodeLabCheckpoint(encodeLabCheckpoint(appended));
    check(restored.available && restored.adaptive.prefixDigest == appended.adaptive.prefixDigest &&
          restored.next->selections[1].main == appended.next->selections[1].main,
          "Versioned checkpoint restores the next unscored forecast");
    check(decodeLabForecast(encodeLabForecast(*appended.next)).selections[1].main ==
          appended.next->selections[1].main &&
          decodeLabResult(encodeLabResult(third)).main == canonicalize(third).main,
          "Immutable forecast and attributed outcome round-trip");
    LabReplay tiny;
    tiny.available = true;
    LabStep preOuter;
    preOuter.frozen.completedDraws = 399;
    tiny.steps.push_back(preOuter);
    for (int target = 0; target < 2; ++target) {
        LabStep row;
        row.actual = target ? third : second;
        row.frozen.completedDraws = size_t(400 + target);
        row.frozen.decision.candidate = 3;
        row.frozen.available.fill(true);
        row.scores.available.fill(true);
        row.scores.mainHits.fill(0);
        row.scores.mainHits[3] = target ? 2 : 1;
        row.scores.mainHits[1] = 1;
        row.scores.mainHits[2] = target ? 0 : 2;
        row.scores.mainHits[0] = target ? 1 : 0;
        for (size_t j = 0; j < 15; ++j) {
            row.frozen.probabilities[j] = Probabilities{};
            row.scores.losses[j] = score(Probabilities{}, row.actual);
        }
        tiny.steps.push_back(row);
    }
    const auto metrics = evaluateLab(tiny);
    near(metrics.selector.meanHits(), 1.5, "Independent two-target selector hit mean");
    check(metrics.commonTargets == 2 && metrics.selector.hitHistogram[1] == 1 &&
          metrics.selector.hitHistogram[2] == 1 && metrics.selector.hitHistogram[0] == 0,
          "Full hit histogram retains zero and observed cells");
    check(metrics.selector.atLeast(3) == 0 && metrics.selector.atLeast(4) == 0 &&
          metrics.selector.exactly(5) == 0 && metrics.selector.exactly(6) == 0,
          "Threshold and exact-hit counts exclude incomplete pre-outer targets");
    near(metrics.versusAdaptive.meanHitDifference, 0.5,
         "Paired selected-minus-Adaptive hit difference");
    near(metrics.versusFixed.meanHitDifference, 0.5,
         "Paired selected-minus-fixed hit difference");
    near(metrics.fairExpectedMainHits, 36.0 / 49.0,
         "Analytic fair expectation is separate from realized uniform selections");
    check(metrics.uniform.hitSum == 1 && metrics.candidates[3].hitSum == 3 &&
          metrics.selector.calibration[0][1].samples == 98,
          "Realized uniform and calibration denominator use common targets");
    check(!metrics.versusAdaptive.hitInterval,
          "Insufficient paired target count has no descriptive interval");
    LabReplay negative;
    negative.available = true;
    for (size_t i = 0; i < 200; ++i) {
        auto row = tiny.steps[1];
        row.frozen.completedDraws = 400 + i;
        row.scores.mainHits[3] = 0;
        row.scores.mainHits[1] = 1;
        negative.steps.push_back(std::move(row));
    }
    const auto negativeMetrics = evaluateLab(negative);
    near(negativeMetrics.versusAdaptive.meanHitDifference, -1,
         "Negative paired difference is retained");
    check(negativeMetrics.versusAdaptive.hitInterval &&
          std::abs(negativeMetrics.versusAdaptive.hitInterval->low + 1) < 1e-12 &&
          std::abs(negativeMetrics.versusAdaptive.hitInterval->high + 1) < 1e-12,
          "Seeded paired interval retains negative differences");
    check(negativeMetrics.evidence.find("development") != std::string::npos &&
          negativeMetrics.candidates[3].targets == 200,
          "Candidate exploration does not become selector confirmation");
    near(metrics.versusAdaptive.meanLogDifference, 0,
         "Zero paired log difference is retained without an improvement claim");
    const auto fairA = labControls(401, 0, sha256("fixture-history"), 1);
    const auto fairB = labControls(401, 0, sha256("fixture-history"), 1);
    check(!fairA.complete && fairA.completed == 1 && fairA.historyHashes == fairB.historyHashes &&
          fairA.nullStatistics == fairB.nullStatistics,
          "Synthetic fair control is deterministic and partial work is not complete");
    near(fairA.tailFraction,
         (fairA.nullStatistics[0] >= 0 ? 2.0 : 1.0) / 2,
         "Monte Carlo tail uses the predeclared plus-one arithmetic");
    std::stop_source cancelledControl;
    cancelledControl.request_stop();
    const auto cancelledReport = labControls(401, 0, sha256("fixture-history"), 1,
                                             cancelledControl.get_token());
    check(cancelledReport.cancelled && !cancelledReport.complete && cancelledReport.completed == 0,
          "Cancelled control cannot be reported complete");
    const auto ledgerRoot = std::filesystem::temp_directory_path() /
        ("marksix-forecast-lab-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    constexpr const char* now = "2026-09-27T03:00:00Z";
    int64_t originalLineage = 0;
    {
        LabLedger ledger(ledgerRoot);
        const auto initial = ledger.reconcile(Snapshot({seed}), 1, now);
        check(initial.available && initial.added == 0 && initial.lineage.completedDraws == 1,
              "Durable seed stores checkpoint and unscored next forecast");
        originalLineage = initial.lineage.id;
        const auto pending = ledger.forecasts(originalLineage);
        check(pending.size() == 1 && pending[0].forecast.targetOrdinal == 2 && !pending[0].outcome,
              "Pending immutable forecast is independently readable");
        const auto noOp = ledger.reconcile(Snapshot({seed}), 1, now);
        check(noOp.reused && noOp.added == 0 && noOp.lineage.id == originalLineage,
              "No-op reload never observes a result twice");
        const auto append = ledger.reconcile(Snapshot({seed, second}), 2, now);
        check(append.available && append.added == 1 && append.lineage.id == originalLineage,
              "Strict append resumes the existing checkpoint");
        const auto page = ledger.forecasts(originalLineage);
        check(page.size() == 2 && page[0].outcome && page[0].outcome->main == second.main &&
              !page[1].outcome && page[1].forecast.targetOrdinal == 3,
              "Outcome attaches to prior forecast and next remains pending");
        auto metadata = second;
        metadata.source.retrievedUtc = "2026-09-27T04:00:00Z";
        check(ledger.reconcile(Snapshot({seed, metadata}), 3, now).reused,
              "Metadata-only source revision does not relearn values");
        std::stop_source stop;
        stop.request_stop();
        bool wasCancelled = false;
        try { (void)ledger.reconcile(Snapshot({seed, second, third}), 4, now, stop.get_token()); }
        catch (const std::runtime_error&) { wasCancelled = true; }
        check(wasCancelled && ledger.head()->completedDraws == 2,
              "Cancelled append leaves the last complete checkpoint current");
    }
    {
        LabLedger ledger(ledgerRoot);
        const auto resumed = ledger.reconcile(Snapshot({seed, second, third}), 4, now);
        check(resumed.available && resumed.added == 1 && !resumed.revised &&
              resumed.lineage.id == originalLineage &&
              resumed.replay.adaptive.prefixDigest == appended.adaptive.prefixDigest,
              "Restart resumes remaining batch and matches clean replay");
        auto correction = second;
        correction.main = {1,2,3,4,5,8};
        const auto revised = ledger.reconcile(Snapshot({seed, correction, third}), 5, now);
        check(revised.available && revised.revised && revised.lineage.id != originalLineage &&
              ledger.lineages().size() == 2 && ledger.forecasts(originalLineage)[0].outcome->main == second.main,
              "Correction makes a new lineage without rewriting previous evidence");
    }
    const auto workerRoot = ledgerRoot / "control-worker";
    LabControlReport cachedFixture;
    cachedFixture.protocolDigest = labProtocolDigest();
    cachedFixture.sourceDigest = sha256("cached-control-fixture");
    cachedFixture.historyLength = 401;
    cachedFixture.observedStatistic = 0;
    cachedFixture.completed = LabControlRuns;
    cachedFixture.complete = true;
    cachedFixture.historyHashes.assign(LabControlRuns, sha256("synthetic-fixture-history"));
    cachedFixture.nullStatistics.assign(LabControlRuns, 0.25);
    cachedFixture.tailFraction = 1;
    {
        LabLedger ledger(workerRoot);
        ledger.saveControl(cachedFixture);
        check(ledger.control(cachedFixture.sourceDigest, 401)->historyHashes.size() == LabControlRuns,
              "Complete control cache survives storage round-trip");
    }
    {
        LabControlWorker worker(workerRoot);
        const auto cached = worker.start(401, 0, cachedFixture.sourceDigest).get();
        check(cached.complete && worker.progress().completed == LabControlRuns &&
              worker.page(0, 100).size() == 100 && worker.page(100, 100).size() == 99,
              "Completed controls resume from keyed cache with bounded pages");
        auto cancelledFuture = worker.start(401, 0, sha256("cancel-control-fixture"));
        worker.cancel();
        const auto partial = cancelledFuture.get();
        check(partial.cancelled && !partial.complete && partial.completed < LabControlRuns,
              "Control worker cancellation does not publish a complete cache");
    }
    {
        LabLedger ledger(ledgerRoot / "prefetch");
        (void)ledger.reconcile(Snapshot({seed}), 1, now);
        ArchiveState before{Snapshot({seed})};
        before.version = 1;
        before.revisions.push_back({revisionDigest(seed), seed, true});
        UpdateAttempt attempt;
        attempt.id = 42; attempt.sourceId = second.source.sourceId;
        attempt.status = "pending"; attempt.startedUtc = "2026-09-27T02:59:00Z";
        attempt.baseVersion = 1;
        before.attempts.push_back(attempt);
        check(ledger.prepareFetch(before, 42, now) > 0,
              "Pre-fetch boundary saves the already pending Forecast Lab target");
        auto freshSecond = second, freshThird = third;
        freshSecond.source.retrievedUtc = "2026-09-27T03:05:00Z";
        freshThird.source.retrievedUtc = "2026-09-27T03:06:00Z";
        ArchiveState after{Snapshot({seed, freshSecond, freshThird})};
        after.version = 2;
        attempt.status = "succeeded"; attempt.finishedUtc = "2026-09-27T03:10:00Z";
        after.attempts.push_back(attempt);
        after.revisions.push_back({revisionDigest(seed), seed, true});
        after.revisions.push_back({revisionDigest(freshSecond), freshSecond, true});
        after.revisions.push_back({revisionDigest(freshThird), freshThird, true});
        const auto updated = ledger.reconcile(after.snapshot, after.version,
                                              "2026-09-27T03:11:00Z");
        ledger.annotateFetched(after, "2026-09-27T03:11:00Z");
        ledger.annotateFetched(after, "2026-09-27T03:11:00Z");
        const auto historyPage = ledger.forecasts(updated.lineage.id);
        check(historyPage.size() == 3 &&
              historyPage[0].provenance.find("locally-pre-fetch") == 0 &&
              historyPage[1].provenance == "historical-replay" &&
              historyPage[2].provenance == "pending-unverified",
              "Only the pre-saved first batch target gains limited local provenance");
    }
    const auto labDb = ledgerRoot / "statistics" / "forecast_lab.sqlite3";
    sqlite3* tamper = nullptr;
    check(sqlite3_open(labDb.string().c_str(), &tamper) == SQLITE_OK,
          "Fixture database opens for controlled corruption test");
    check(sqlite3_exec(tamper,
          "DROP TRIGGER immutable_lab_checkpoints_UPDATE;"
          "UPDATE checkpoints SET checksum='invalid' WHERE lineage=(SELECT lineage FROM head) "
          "AND ordinal=(SELECT ordinal FROM head);", nullptr, nullptr, nullptr) == SQLITE_OK,
          "Fixture checksum tampering is isolated to the temporary database");
    sqlite3_close(tamper);
    bool corruptionRejected = false;
    try { LabLedger damaged(ledgerRoot); }
    catch (const std::runtime_error&) { corruptionRejected = true; }
    check(corruptionRejected && std::filesystem::exists(labDb),
          "Corrupt active checkpoint is rejected and preserved for recovery");
    std::cout << "PASS Forecast Lab candidate protocol checks=" << checks
              << " protocol=" << labProtocolDigest() << '\n';
    return checks;
}
