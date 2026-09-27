#include "adaptive.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>

using namespace marksix::statistics;
namespace {
size_t checks = 0;
void check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
void near(double actual, double expected, const char* message, double tolerance = 1e-12) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
template<class F> void rejects(F f, const char* message) {
    bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; } check(rejected, message);
}
// Dates/seed identity are protocol fixtures; numbers are deliberately synthetic.
Result row(size_t index) {
    using namespace std::chrono;
    const auto date = year_month_day{sys_days{year{2002}/7/4} + days{int(index)}};
    const auto ordinal = int(date.year()) == 2002 ? int(index) + 53 : int((sys_days{date} - sys_days{date.year()/1/1}).count()) + 1;
    char text[40]{}; Result r;
    std::snprintf(text, sizeof(text), "%04d-%02u-%02u", int(date.year()), unsigned(date.month()), unsigned(date.day())); r.date = text;
    std::snprintf(text, sizeof(text), "%02d/%03d", int(date.year()) % 100, ordinal); r.id = text;
    r.main = {1, 2, 3, 4, 5, 6}; r.extra = 7;
    r.source = {"adaptive-synthetic", "https://example.invalid/adaptive", "2026-09-25T00:00:00Z",
        sha256("synthetic"), "not-real-draws", Trust::OfficialRetrieval, "test-only-evidence"};
    return r;
}
void equal(const Probabilities& a, const Probabilities& b) {
    for (size_t i = 0; i < 49; ++i) {
        check(a[i].main == b[i].main && a[i].extra == b[i].extra && a[i].absent == b[i].absent, "Bit reproducible prediction");
    }
}
void invariants(const AdaptivePrediction& p) {
    double main = 0, extra = 0;
    for (const auto& q : p.probabilities) {
        near(q.main + q.extra + q.absent, 1, "Adaptive category sum");
        check(q.main >= 3.0/49 && q.extra >= 0.5/49 && q.absent >= 21.0/49, "Uniform lower bounds");
        main += q.main; extra += q.extra;
    }
    near(main, 6, "Adaptive main total"); near(extra, 1, "Adaptive Extra total");
    near(std::accumulate(p.weights.begin(), p.weights.end(), 0.0), 1, "Weight total");
    check(std::find(p.selection.main.begin(), p.selection.main.end(), p.selection.extra) == p.selection.main.end(), "Distinct Extra");
}
void eligibility() {
    check(!replayAdaptive(Snapshot({})).available, "Empty unavailable");
    check(!replayAdaptive(Snapshot({row(1)})).available, "Missing exact seed unavailable");
    auto seed = row(0); seed.source.trust = Trust::Unverified;
    check(!replayAdaptive(Snapshot({seed, row(1)})).available, "Unverified seed unavailable");
    seed = row(0); seed.unresolvedConflict = true;
    check(!replayAdaptive(Snapshot({seed, row(1)})).available, "Conflicting seed unavailable");
    auto earlier = row(0); earlier.date = "2002-07-02"; earlier.id = "02/052"; earlier.era = Era::EarlierUnreviewed;
    auto unverified = row(2); unverified.source.trust = Trust::Unverified;
    auto conflict = row(3); conflict.unresolvedConflict = true;
    auto replay = replayAdaptive(Snapshot({row(1), conflict, earlier, row(0), unverified}));
    check(replay.available && replay.excluded == 3 && replay.steps.size() == 1, "Era/trust/conflict excluded and order canonicalized");
    auto only = replayAdaptive(Snapshot({row(0)}));
    check(only.available && only.steps.empty() && only.next && only.next->trainingDraws == 1 && only.next->warmup, "Seed-only prediction without scoring seed");
    auto sameDate = row(1); sameDate.date = row(0).date;
    rejects([&] { replayAdaptive(Snapshot({row(0), sameDate})); }, "Ambiguous same-date ordering rejected");
    rejects([&] { Snapshot({row(0), row(0)}); }, "Duplicate identity rejected");
    std::stop_source stop; stop.request_stop();
    rejects([&] { replayAdaptive(Snapshot({row(0)}), stop.get_token()); }, "Replay cancellation");
}
// Independent closed-form 2/3-draw calculation, without production score/count helpers.
void reference() {
    auto state = seedAdaptive(row(0)); auto second = predictAdaptive(state);
    invariants(second);
    check(second.selection.main == row(0).main && second.selection.extra == 7, "Stable ascending tied suggestions");
    for (double w : second.weights) near(w, 0.25, "Equal initial weights");
    const std::array<double, 3> u{6.0/49, 1.0/49, 42.0/49};
    double uniformLoss = 0, expertLoss = 0, mixtureLoss = 0;
    for (size_t i = 0; i < 49; ++i) {
        const size_t category = i < 6 ? 0 : i == 6 ? 1 : 2;
        const double p = (1 + 100*u[category])/101;
        uniformLoss -= std::log(u[category])/49;
        expertLoss -= std::log(p)/49;
        mixtureLoss -= std::log(0.625*u[category] + 0.375*p)/49;
        near(second.probabilities[i].main, 0.625*u[0] + 0.375*((i < 6 ? 1 : 0) + 100*u[0])/101, "Independent second main");
        near(second.probabilities[i].extra, 0.625*u[1] + 0.375*((i == 6 ? 1 : 0) + 100*u[1])/101, "Independent second Extra");
    }
    auto step = observeAdaptive(state, row(1));
    near(step.uniformLoss.log, uniformLoss, "Independent uniform loss");
    near(step.adaptiveLoss.log, mixtureLoss, "Score saved mixture before learning");
    near(step.expertLogLoss[1], expertLoss, "Independent expert loss");
    const double uniformWeight = std::exp(-uniformLoss)/(std::exp(-uniformLoss) + 3*std::exp(-expertLoss));
    near(step.weightsAfter[0], uniformWeight, "Independent post-observation weight");
    check(step.weightsAfter[0] < 0.25 && step.weightsAfter[1] > 0.25, "Better expert gains weight");
    check(step.before.draws == 1 && step.after.draws == 2, "Before/after step retained");
    auto third = predictAdaptive(state); invariants(third);
    const std::array<double, 3> effective{2, 1 + std::pow(2.0, -1.0/50), 1 + std::pow(2.0, -1.0/200)};
    for (size_t i = 0; i < 49; ++i) {
        std::array<double, 3> expected{};
        for (size_t c = 0; c < 3; ++c) {
            expected[c] = (0.5 + 0.5*uniformWeight)*u[c];
            for (double n : effective) {
                const bool observed = c == (i < 6 ? 0u : i == 6 ? 1u : 2u);
                expected[c] += 0.5*(1-uniformWeight)/3*((observed ? n : 0) + 100*u[c])/(n+100);
            }
        }
        near(third.probabilities[i].main, expected[0], "Independent third main");
        near(third.probabilities[i].extra, expected[1], "Independent third Extra");
        near(third.probabilities[i].absent, expected[2], "Independent third absent");
    }
    for (size_t j = 0; j < 3; ++j) near(state.counts[j].n, effective[j], "Independent decay effective counts");
}
void replay() {
    std::vector<Result> rows;
    for (size_t i = 0; i < 205; ++i) {
        auto r = row(i);
        for (size_t j = 0; j < 6; ++j) r.main[j] = int((i*7+j)%49)+1;
        r.extra = int((i*7+6)%49)+1; rows.push_back(r);
    }
    auto whole = replayAdaptive(Snapshot(rows));
    check(whole.steps.size() == 204 && !whole.next->warmup, "Score from second draw and leave warm-up");
    check(whole.steps[198].prediction.warmup && !whole.steps[199].prediction.warmup, "Exact warm-up boundary");
    auto state = seedAdaptive(rows.front());
    for (size_t i = 1; i < rows.size(); ++i) {
        equal(predictAdaptive(state).probabilities, whole.steps[i-1].prediction.probabilities);
        auto step = observeAdaptive(state, rows[i]); invariants(step.prediction);
        check(step.after.prefixDigest == whole.steps[i-1].after.prefixDigest, "Incremental replay prefix agreement");
    }
    equal(predictAdaptive(state).probabilities, whole.next->probabilities);
    auto prefixRows = rows; prefixRows.resize(150);
    auto partial = replayAdaptive(Snapshot(prefixRows));
    equal(partial.next->probabilities, whole.steps[149].prediction.probabilities);
    rows[150].main = {8,9,10,11,12,13}; rows[150].extra = 14;
    auto revised = replayAdaptive(Snapshot(rows));
    for (size_t i = 0; i <= 149; ++i) equal(revised.steps[i].prediction.probabilities, whole.steps[i].prediction.probabilities);
    check(revised.steps[149].after.prefixDigest != whole.steps[149].after.prefixDigest, "Correction affects only subsequent learning");
    for (auto& r : rows) r.source.retrievedUtc = "2026-09-25T01:00:00Z";
    auto refreshed = replayAdaptive(Snapshot(rows));
    check(refreshed.snapshotDigest != revised.snapshotDigest && refreshed.state.prefixDigest == revised.state.prefixDigest, "Metadata refresh preserves learned identity");
    equal(refreshed.next->probabilities, revised.next->probabilities);
    auto before = predictAdaptive(state);
    rejects([&] { observeAdaptive(state, rows.front()); }, "Backwards observation rejected");
    equal(before.probabilities, predictAdaptive(state).probabilities);
}
void invalid() {
    const auto state = seedAdaptive(row(0));
    auto bad = state; bad.counts[0].main[0] = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { predictAdaptive(bad); }, "NaN count rejected");
    bad = state; bad.counts[1].n = 0; rejects([&] { predictAdaptive(bad); }, "Invalid effective count rejected");
    bad = state; bad.logWeights[0] = 1; rejects([&] { predictAdaptive(bad); }, "Unnormalized weight rejected");
    bad = state; bad.logWeights[0] = -std::numeric_limits<double>::infinity(); rejects([&] { predictAdaptive(bad); }, "Infinite weight rejected");
    bad = state; bad.lastId = "xx/000"; rejects([&] { predictAdaptive(bad); }, "Invalid identity rejected");
    bad = state; bad.prefixDigest = "bad"; rejects([&] { predictAdaptive(bad); }, "Invalid prefix rejected");
    bad = state; bad.counts[0].extra[0] = 1; rejects([&] { predictAdaptive(bad); }, "Overlapping categories rejected");
    bad = state; bad.counts[0].main[0] = 0; rejects([&] { predictAdaptive(bad); }, "Invalid main total rejected");
}
void evaluation() {
    const Snapshot seed({row(0)});
    const auto empty = evaluateAdaptive(replayAdaptive(seed), backtest(seed));
    check(empty.all.adaptive.draws == 0 && !empty.all.uniformMinusAdaptiveLog.interval, "Seed-only has no evaluation");
    std::vector<Result> rows;
    for (size_t i = 0; i < 405; ++i) {
        auto r = row(i);
        // Alternating disjoint blocks deliberately make recently learned frequencies fail.
        const int offset = i%2 == 0 ? 0 : 20;
        for (auto& n : r.main) n += offset; r.extra += offset; rows.push_back(r);
    }
    const Snapshot snapshot(rows);
    const auto fixed = backtest(snapshot);
    const auto replay = replayAdaptive(snapshot);
    const auto result = evaluateAdaptive(replay, fixed);
    check(result.all.adaptive.draws == 404 && result.all.uniform.draws == 404 && result.all.fixed.draws == 0, "All window starts at draw two");
    check(result.common.adaptive.draws == 205 && result.common.uniform.draws == 205 && result.common.fixed.draws == 205, "Common denominators start at 201");
    check(result.all.firstId == rows[1].id && result.common.firstId == rows[200].id, "Window identities");
    near(result.common.fixed.mean.log, fixed.meanModel.log, "Unchanged fixed log output");
    near(result.common.fixed.mean.brier, fixed.meanModel.brier, "Unchanged fixed Brier output");
    std::vector<double> logs, briers;
    for (const auto& step : replay.steps) {
        logs.push_back(step.uniformLoss.log - step.adaptiveLoss.log);
        briers.push_back(step.uniformLoss.brier - step.adaptiveLoss.brier);
    }
    const auto logReference = bootstrap(logs), brierReference = bootstrap(briers);
    near(result.all.uniformMinusAdaptiveLog.meanDifference, logReference.meanDifference, "Paired log sign/denominator");
    near(result.all.uniformMinusAdaptiveLog.interval->low, logReference.interval->low, "Descriptive paired interval");
    near(result.all.uniformMinusAdaptiveBrier.interval->high, brierReference.interval->high, "Paired Brier interval");
    near(result.common.fixedMinusAdaptiveLog.meanDifference,
         result.common.fixed.mean.log - result.common.adaptive.mean.log, "Fixed minus adaptive sign");
    for (const auto* d : {&result.all.adaptive, &result.all.uniform, &result.common.adaptive, &result.common.fixed}) {
        for (size_t s = 0; s < 3; ++s) {
            uint64_t samples = 0, observed = 0;
            for (const auto& b : d->calibration[s]) { samples += b.samples; observed += b.observed; }
            check(samples == 49*d->draws, "Calibration 49 trials per draw");
            check(observed == (s == 0 ? 6 : s == 1 ? 1 : 7)*d->draws, "Calibration scope outcomes");
        }
    }
    check(result.all.uniform.mainHits == 202*6 && result.all.uniform.extraMatches == 202, "Hit diagnostics fixed tie ranking");
    check(result.evidence.starts_with("No demonstrated predictive advantage"), "No replay-based advantage promotion");
    rows.resize(2); const Snapshot tiny(rows);
    const auto negative = evaluateAdaptive(replayAdaptive(tiny), backtest(tiny));
    check(negative.all.uniformMinusAdaptiveLog.meanDifference < 0 && !negative.all.uniformMinusAdaptiveLog.interval, "Negative result retained with insufficient interval");
    auto wrong = fixed; wrong.snapshotDigest = "wrong";
    rejects([&] { evaluateAdaptive(replay, wrong); }, "Mismatched comparison snapshot rejected");
    wrong = fixed; wrong.draws[0].id = "02/001";
    rejects([&] { evaluateAdaptive(replay, wrong); }, "Mismatched common target rejected");
    std::stop_source stop; stop.request_stop();
    rejects([&] { evaluateAdaptive(replay, fixed, stop.get_token()); }, "Evaluation cancellation");
}
}
size_t testAdaptive() {
    check(adaptiveProtocolDigest() == "ec44250351655a86a80b8fe32a58ab5c06501e328408fddda8a4b210a2222de2",
          "Frozen pre-evaluation protocol digest; a rule change needs a new version");
    eligibility(); reference(); replay(); invalid(); evaluation();
    std::cout << "PASS adaptive protocol/independent reference/leakage checks=" << checks
              << " protocol=" << adaptiveProtocolDigest() << '\n';
    return checks;
}
