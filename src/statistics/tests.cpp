#include "forecast.hpp"
#include "coverage.hpp"
#include "presentation.hpp"
#include "worker.hpp"
#include <future>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>

using namespace marksix::statistics;
size_t testArchive();
size_t testAdaptive();
size_t testForecastLab();
size_t testAdaptiveLedger();
size_t testUpdatePlan();
size_t testUpdateWorker();
size_t testHkjcProvider();
void verifySavedHkjc(const std::filesystem::path& source, const std::filesystem::path& cache);
void officialReport(const std::filesystem::path& root);
void forecastLabOfficialReport(const std::filesystem::path& root);
void adaptiveOfficialReport(const std::filesystem::path& root, const std::filesystem::path& output);
void adaptiveOfficialBenchmark(const std::filesystem::path& root, const std::filesystem::path& destination);
void officialArchiveCheck(const std::filesystem::path& snapshotRoot, const std::filesystem::path& outputRoot);
namespace {
size_t checks = 0;
void check(bool condition, const char* message) {
    ++checks; if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, const char* message, double tolerance = 1e-12) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
template<class F> void rejects(F operation, const char* message) {
    bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
// Synthetic data only. These deliberately repetitive outcomes are not HKJC evidence.
Result fixture(size_t index = 0) {
    using namespace std::chrono;
    const auto date = year_month_day{sys_days{year{2003}/1/1} + days{int(index)}};
    const auto ordinal = (sys_days{date} - sys_days{date.year()/1/1}).count() + 1;
    char text[40]{}; Result r;
    std::snprintf(text, sizeof(text), "%04d-%02u-%02u", int(date.year()), unsigned(date.month()), unsigned(date.day())); r.date = text;
    std::snprintf(text, sizeof(text), "%02d/%03d", int(date.year()) % 100, int(ordinal)); r.id = text;
    r.main = {1, 2, 3, 4, 5, 6}; r.extra = 7;
    r.source = {"synthetic-test", "https://example.invalid/synthetic", "2026-09-25T00:00:00Z",
                sha256("synthetic-test-response"), "synthetic-not-real", Trust::OfficialRetrieval, "test-only-injected-evidence"};
    return r;
}
std::vector<Result> fixtures(size_t count) {
    std::vector<Result> out; for (size_t i = 0; i < count; ++i) out.push_back(fixture(i)); return out;
}
void domain() {
    near(double(validDate("2004-02-29")), 1, "Leap date accepted");
    for (auto d : {"2003-02-29", "2004-02-30", "2004-13-01", "2004-00-01", "2004-01-00", "04-01-01", "2004/01/01", "2004-1a-01"})
        check(!validDate(d), "Bad civil date rejected");
    check(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known answer");
    check(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 empty answer");
    auto r = fixture(); const auto hash = revisionDigest(r);
    std::reverse(r.main.begin(), r.main.end()); check(revisionDigest(r) == hash, "Unordered main canonical digest");
    check(!canonicalize(r).extractionOrder, "Sorted main does not invent order");
    for (int value : {0, 50, 1}) { auto bad = fixture(); bad.main[5] = value; rejects([&] { canonicalize(bad); }, "Invalid main rejected"); }
    for (int value : {0, 50, 1}) { auto bad = fixture(); bad.extra = value; rejects([&] { canonicalize(bad); }, "Invalid Extra rejected"); }
    r = fixture(); r.id = "02/001"; rejects([&] { canonicalize(r); }, "ID year contradiction");
    r.id = "03/000"; rejects([&] { canonicalize(r); }, "Zero draw ID");
    r = fixture(); r.era = static_cast<Era>(42); rejects([&] { canonicalize(r); }, "Unknown era");
    r.era = Era::EarlierUnreviewed; rejects([&] { canonicalize(r); }, "Era date contradiction");
    r = fixture(); r.date = "2002-07-03"; r.id = "02/052";
    rejects([&] { canonicalize(r); }, "Pre49 excluded from current format");
    r.era = Era::EarlierUnreviewed; check(!eligible(canonicalize(r)), "Earlier record quarantined");
    r = fixture(); r.date = "2002-07-04"; r.id = "02/053";
    check(eligible(canonicalize(r)), "First current draw eligible");
    r = fixture(); r.extractionOrder = std::array<int, 6>{6, 5, 4, 3, 2, 1};
    rejects([&] { canonicalize(r); }, "Order requires evidence");
    r.orderEvidence = "synthetic-order-only"; check(revisionDigest(r) != hash, "Actual order changes revision digest");
    r.extractionOrder = std::array<int, 6>{6, 5, 4, 3, 2, 8}; rejects([&] { canonicalize(r); }, "Order must match set");
    r = fixture(); r.source.verificationEvidence.clear(); rejects([&] { canonicalize(r); }, "Trust needs evidence");
    r.source.trust = Trust::Unverified; check(!eligible(canonicalize(r)), "Unverified ineligible");
    r = fixture(); r.source.retrievedUtc = "2026-09-25T24:00:00Z"; rejects([&] { canonicalize(r); }, "Bad UTC");
    r.source.retrievedUtc = "2026-09-25T00:00:00.1234567Z"; canonicalize(r);
    r.source.contentSha256 = "not-a-digest"; rejects([&] { canonicalize(r); }, "Bad provenance hash");
    r = fixture(); r.source.sourceId = "forged\nlabel"; rejects([&] { canonicalize(r); }, "Control characters rejected");
    const Snapshot a({fixture(1), fixture(0)}), b({fixture(0), fixture(1)});
    check(a.digest() == b.digest(), "Input order invariant snapshot");
    check(a.records().front().id == "03/001", "Snapshot chronology");
    r = fixture(); r.extra = 8;
    check(Snapshot({r, fixture(1)}).digest() != a.digest(), "Correction invalidates digest");
    r = fixture(); r.source.lineage = "different";
    check(revisionDigest(r) != hash, "Provenance affects digest");
    r = fixture(); r.unresolvedConflict = true;
    check(Snapshot({r}).select().empty(), "Conflict excluded");
    rejects([&] { Snapshot invalid({fixture(), fixture()}); }, "Duplicate snapshot identity");
    r = fixture(); r.date = "2003-01-02";
    rejects([&] { Snapshot invalid({fixture(), r}); }, "Same ID contradictory date rejected");
    rejects([&] { Snapshot invalid({}, true); }, "Completeness requires evidence");
    check(a.selectionDigest() != a.selectionDigest({"", "", 1}), "Filters are identified");
    rejects([&] { a.select({"2004-01-01", "2003-01-01", 0}); }, "Reversed window rejected");
}
void exactMath() {
    for (const auto scope : {Scope::Main, Scope::Extra, Scope::Any}) {
        const auto p = baseline(scope); double sum = 0;
        const double k = scope == Scope::Main ? 6 : scope == Scope::Extra ? 1 : 7;
        for (int i = 0; i < 49; ++i) { near(p.inclusion, k / 49, "All49 baseline"); near(p.inclusion + p.complement, 1, "Complement"); sum += p.inclusion; }
        near(sum, k, "Expected total");
        if (scope == Scope::Extra) check(!p.pair, "Extra pair unavailable");
        else near(*p.pair, (k / 49) * ((k - 1) / 48), "Without replacement pair");
    }
    near(sixSetProbability(), 1.0 / 13983816.0, "Six set combinatorics", 1e-22);
    rejects([] { baseline(static_cast<Scope>(42)); }, "Invalid scope");
}
void uncertainty() {
    check(!wilson(0, 0), "Empty Wilson unavailable");
    rejects([] { wilson(2, 1); }, "Invalid count rejected");
    auto interval = *wilson(0, 10); near(interval.low, 0, "Zero successes low"); near(interval.high, 0.2775327998628892, "Zero successes high");
    interval = *wilson(10, 10); near(interval.low, 0.7224672001371107, "All successes low"); near(interval.high, 1, "All successes high");
    interval = *wilson(5, 10); near(interval.low, 0.2365930905125640, "Middle low"); near(interval.high, 0.7634069094874361, "Middle high");
    for (size_t n = 1; n <= 100; ++n) for (size_t c = 0; c <= n; ++c) {
        const auto v = *wilson(c, n); check(v.low >= 0 && v.high <= 1 && v.low <= v.high, "Wilson bounded");
    }
    check(WilsonLabel.find("Not next-draw") != std::string_view::npos, "Interpretation metadata");
}
void descriptive() {
    auto rows = fixtures(3); rows[1].main = {1, 8, 9, 10, 11, 12}; rows[1].extra = 2;
    rows[2].main = {8, 9, 13, 14, 15, 16}; rows[2].extra = 1;
    const Snapshot snapshot(rows); auto main = analyze(snapshot, Scope::Main, {}, 2);
    check(main.draws == 3 && main.numbers[0].count == 2 && main.numbers[7].count == 2, "Hand-counted main");
    near(*main.numbers[0].observedRate, 2.0 / 3, "Main denominator");
    near(main.numbers[0].expectedCount, 18.0 / 49, "Expected count");
    check(*main.numbers[0].observedDrawsSinceLast == 1 && main.numbers[0].gapStatus == GapStatus::IncompleteCoverage, "Unknown coverage gap");
    check(*main.numbers[48].observedDrawsSinceLast == 3 && main.numbers[48].gapStatus == GapStatus::LeftCensored, "Never seen censored lower bound");
    check((*main.pairs)[7][8] == 2 && (*main.pairs)[8][7] == 0 && (*main.pairs)[0][0] == 0, "Unordered pairs no self");
    size_t totalPairs = 0; for (const auto& row : *main.pairs) for (size_t c : row) totalPairs += c;
    check(totalPairs == 45, "Three draws fifteen pairs each");
    check(!main.rolling[0].fullWindow && main.rolling[0].draws == 1 && main.rolling[1].fullWindow, "Partial rolling window marked");
    near(main.rolling[2].rates[0], 0.5, "Rolling eviction"); near(main.rolling[2].rates[7], 1, "Rolling appearance");
    auto extra = analyze(snapshot, Scope::Extra);
    check(!extra.pairs && extra.numbers[0].count == 1 && extra.numbers[1].count == 1 && extra.numbers[6].count == 1, "Extra separate");
    const auto any = analyze(snapshot, Scope::Any); check(any.numbers[0].count == 3 && (*any.pairs)[0][1] == 2, "Allseven includes Extra pairs");
    const auto last = analyze(snapshot, Scope::Main, {"", "", 1});
    check(last.draws == 1 && last.numbers[0].count == 0 && last.from == rows[2].date, "Last N");
    const auto date = analyze(snapshot, Scope::Main, {"2003-01-01", "2003-01-02", 1});
    check(date.draws == 1 && date.from == rows[1].date, "Dates before last N");
    const auto empty = analyze(snapshot, Scope::Main, {"2004-01-01", "", 0});
    check(empty.draws == 0 && !empty.numbers[0].observedRate && !empty.numbers[0].interval &&
          !empty.numbers[0].observedDrawsSinceLast && empty.numbers[0].gapStatus == GapStatus::NoData, "Empty values unavailable");
    const Snapshot known(rows, true, "synthetic complete inventory");
    check(analyze(known).numbers[0].gapStatus == GapStatus::ExactWithinVerifiedCoverage, "Known within-coverage gap");
    rows[1].source.trust = Trust::Unverified; rows[2].unresolvedConflict = true;
    auto old = fixture(); old.date = "2000-01-01"; old.id = "00/001"; old.era = Era::EarlierUnreviewed; rows.push_back(old);
    const auto filtered = analyze(Snapshot(rows, true, "synthetic inventory"));
    check(filtered.draws == 1 && filtered.excluded == 3 && !filtered.completenessKnown, "Trust/conflict/era exclusions and coverage");
    rejects([&] { analyze(snapshot, Scope::Main, {}, 0); }, "Zero rolling window");
}
void model() {
    const auto insufficient = forecast(Snapshot(fixtures(199)));
    check(!insufficient.modelAvailable && !insufficient.selection, "199 baseline only");
    for (const auto& q : insufficient.probabilities) near(q.main, 6.0 / 49, "Insufficient uniform");
    const auto prediction = forecast(Snapshot(fixtures(200)));
    check(prediction.modelAvailable && prediction.selection.has_value() && prediction.trainingDraws == 200, "200 model gate");
    near(prediction.probabilities[0].main, (200 + 600.0 / 49) / 300, "Main shrinkage");
    near(prediction.probabilities[6].extra, (200 + 100.0 / 49) / 300, "Extra shrinkage");
    double m = 0, e = 0;
    for (const auto& q : prediction.probabilities) {
        near(q.main + q.extra + q.absent, 1, "Category normalization");
        near(q.any() + q.absent, 1, "Any complement");
        check(q.main > 0 && q.extra > 0 && q.absent > 0, "Positive probabilities");
        m += q.main; e += q.extra;
    }
    near(m, 6, "Total main"); near(e, 1, "Total Extra");
    check(prediction.selection->main == std::array<int, 6>{1, 2, 3, 4, 5, 6} && prediction.selection->extra == 7, "Rank and ties");
    // Exact balanced marginal counts: every number appears six times as main, once as Extra in each 49-row cycle.
    auto balanced = fixtures(245);
    for (size_t i = 0; i < balanced.size(); ++i) {
        for (int n = 0; n < 6; ++n) balanced[i].main[n] = int((i + n) % 49) + 1;
        balanced[i].extra = int((i + 6) % 49) + 1;
    }
    const auto ties = forecast(Snapshot(balanced));
    check(ties.selection->main == std::array<int, 6>{1, 2, 3, 4, 5, 6} && ties.selection->extra == 7, "Balanced ties ascending");
    for (const auto& q : ties.probabilities) { near(q.main, 6.0 / 49, "Balanced shrinks to baseline"); near(q.extra, 1.0 / 49, "Extra remains unconditional"); }
    auto unverified = fixtures(200); unverified.back().source.trust = Trust::Unverified;
    check(!forecast(Snapshot(unverified)).modelAvailable, "Model eligibility gate");
    check(SelectionLabel.find("not a most-likely joint draw") != std::string_view::npos, "Selection limitation");
}
void coverageTests() {
    auto rows = fixtures(3); rows[1].source.trust = Trust::Unverified; rows[2].unresolvedConflict = true;
    rows[1].source.lineage = rows[0].source.lineage; rows[1].source.sourceId = "copy-of-same-upstream";
    auto old = fixture(); old.id = "00/001"; old.date = "2000-01-01"; old.era = Era::EarlierUnreviewed; rows.push_back(old);
    const Snapshot snapshot(rows); const auto summary = coverage(snapshot);
    check(summary.total == 4 && summary.eligibleCurrent49 == 1 && summary.earlierQuarantined == 1 && summary.conflicts == 1, "Coverage exclusions");
    check(summary.sourceStatuses[0] == 1 && summary.sourceStatuses[1] == 3 && summary.sourceStatuses[2] == 0, "Source status counts");
    check(!summary.expectedInventoryVerified && !summary.complete && summary.knownOmissions.empty(), "Unknown expected inventory no invented omissions");
    check(summary.sources.size() == 2 && !summary.sources[0].independentCorroborationEstablished &&
          !summary.sources[1].independentCorroborationEstablished, "Shared lineage not independent evidence");
    check(browseEra(snapshot, Era::EarlierUnreviewed).size() == 1 && browseEra(snapshot, Era::Current49).size() == 3, "Era browsing includes ineligible records");
    const auto missing = coverage(snapshot, {{"03/010", "2003-01-10", "synthetic expected inventory"}});
    check(missing.knownOmissions.size() == 1 && !missing.complete, "Evidence-backed known omission");
    rejects([&] { coverage(snapshot, {{"03/010", "2003-01-10", ""}}); }, "Omission needs evidence");
    rejects([&] { coverage(snapshot, {{"03/001", "2003-01-01", "synthetic"}}); }, "Present row not a missing draw");
    check(!coverage(Snapshot({})).complete, "Empty is not complete");
    check(coverage(Snapshot(fixtures(3), true, "synthetic expected inventory")).complete, "Explicit complete inventory");
    rows = fixtures(2); rows[0].source.retrievedUtc = "2026-09-25T00:00:00Z"; rows[1].source.retrievedUtc = "2026-09-25T00:00:00.1Z";
    check(coverage(Snapshot(rows)).latestRetrievalUtc == rows[1].source.retrievedUtc, "UTC fractions compare numerically");
}
void scoring() {
    const auto uniform = score(Probabilities{}, fixture());
    near(uniform.brier, 600.0 / 2401, "Independent uniform multiclass Brier");
    near(uniform.mainBrier, 258.0 / 2401, "Uniform main Brier");
    near(uniform.extraBrier, 48.0 / 2401, "Uniform Extra Brier");
    near(uniform.anyBrier, 294.0 / 2401, "Uniform any Brier");
    near(uniform.log, -(6 * std::log(6.0 / 49) + std::log(1.0 / 49) + 42 * std::log(42.0 / 49)) / 49, "Marginal log reference");
    // All numbers assigned a fixed category vector; independently count each observed category.
    Probabilities p{}; for (auto& q : p) q = {0.2, 0.1, 0.7};
    const auto hand = score(p, fixture());
    near(hand.brier, (6 * (0.64 + 0.01 + 0.49) + (0.04 + 0.81 + 0.49) + 42 * (0.04 + 0.01 + 0.09)) / 49, "Hand Brier");
    near(hand.log, -(6 * std::log(0.2) + std::log(0.1) + 42 * std::log(0.7)) / 49, "Hand log");
    p[0].main = 0; rejects([&] { score(p, fixture()); }, "Zero probability rejected");
    CalibrationBin empty; check(!empty.meanPrediction() && !empty.observedRate(), "Empty calibration unavailable");
}
void chronological() {
    auto rows = fixtures(205); const auto first = backtest(Snapshot(rows));
    check(first.draws.size() == 5 && first.draws[0].trainingDraws == 200 && first.draws[0].id == "03/201", "Walk forward starts201");
    check(first.evidence == Evidence::InsufficientTestHistory && !first.brierDifference.interval, "Short test no comparative interval");
    const auto f = forecast(Snapshot(fixtures(200)));
    for (size_t n = 0; n < 49; ++n) near(first.draws[0].prediction[n].main, f.probabilities[n].main, "Strictly prior training");
    rows.push_back(fixture(205)); const auto appended = backtest(Snapshot(rows));
    check(first.snapshotDigest != appended.snapshotDigest, "Append report identity changes");
    for (size_t i = 0; i < first.draws.size(); ++i) for (size_t n = 0; n < 49; ++n)
        near(first.draws[i].prediction[n].main, appended.draws[i].prediction[n].main, "Append leakage control", 0);
    rows.back().main = {40, 41, 42, 43, 44, 45}; rows.back().extra = 49;
    const auto changed = backtest(Snapshot(rows));
    for (size_t i = 0; i < appended.draws.size(); ++i) {
        for (size_t n = 0; n < 49; ++n) {
            near(changed.draws[i].prediction[n].main, appended.draws[i].prediction[n].main, "Future-change main leakage", 0);
            near(changed.draws[i].prediction[n].extra, appended.draws[i].prediction[n].extra, "Future-change Extra leakage", 0);
        }
        if (i + 1 < appended.draws.size()) near(changed.draws[i].model.brier, appended.draws[i].model.brier, "Earlier score preserved", 0);
    }
    check(changed.draws.back().model.brier != appended.draws.back().model.brier, "Changed target changes score not prediction");
    for (size_t scope = 0; scope < 3; ++scope) {
        uint64_t samples = 0, successes = 0;
        for (const auto& bin : first.modelCalibration[scope]) { samples += bin.samples; successes += bin.observed; }
        check(samples == 245 && successes == uint64_t(scope == 0 ? 30 : scope == 1 ? 5 : 35), "Calibration counts");
    }
    const auto improving = backtest(Snapshot(fixtures(400)));
    check(improving.evidence == Evidence::HistoricalHeldOutImprovement && improving.brierDifference.interval->low > 0, "Synthetic historical improvement gate");
    auto shifted = fixtures(400);
    // Balanced held-out outcomes following skewed training, rather than another
    // repeated set that a frequency model can learn during the test period.
    for (size_t i = 200; i < shifted.size(); ++i) {
        for (int n = 0; n < 6; ++n) shifted[i].main[n] = int((i + n) % 49) + 1;
        shifted[i].extra = int((i + 6) % 49) + 1;
    }
    const auto negative = backtest(Snapshot(shifted));
    check(negative.evidence == Evidence::NoDemonstratedAdvantage && negative.brierDifference.meanDifference < 0, "Retain negative result");
    std::stop_source stop; stop.request_stop(); rejects([&] { backtest(Snapshot(rows), stop.get_token()); }, "Backtest cancellation");
}
void presentationTests() {
    ViewData d; d.snapshot=std::make_shared<const Snapshot>(fixtures(205));
    d.coverage=coverage(*d.snapshot);d.analysis=analyze(*d.snapshot);d.forecast=forecast(*d.snapshot);d.backtest=backtest(*d.snapshot);
    auto p=present(d,Page::Numbers);
    check(p.rows.size()==49&&p.chart.size()==49,"All numbers have table and chart values");
    check(p.columns[5]=="95% Wilson low"&&p.explanation.find("Not next-draw odds")!=std::string::npos,"Interval assumptions visible");
    check(p.summary.find("N=205")!=std::string::npos&&p.summary.find("2003-01-01")!=std::string::npos,"Denominator and dates visible");
    for(size_t n=0;n<49;++n){near(*p.rows[n][2].numeric,*d.analysis.numbers[n].observedRate,"Table equals analysis",0);near(p.chart[n],*p.rows[n][2].numeric,"Chart equals table",0);}
    sortRows(p,0,true);check(p.rows.front()[0].text=="49"&&p.identities.front()==48,"Numeric descending sort preserves identity");
    sortRows(p,0,false);check(p.rows[9][0].text=="10"&&p.identities[9]==9,"Numeric ascending is not lexical");
    check(p.rows.back()[8].text=="At least; left-censored","Never-observed censoring visible");
    p=present(d,Page::Rolling,7);check(p.rows.size()==205&&p.rows.front()[5].text=="Incomplete initial window","Initial rolling window marked");
    for(size_t i=0;i<p.rows.size();++i)near(p.chart[i],*p.rows[i][4].numeric,"Rolling chart equals table",0);
    check(present(d,Page::Pairs).rows.size()==1176,"All unordered pairs displayed");
    d.analysis=analyze(*d.snapshot,Scope::Extra);check(present(d,Page::Pairs).rows.empty(),"Extra pairs unavailable in GUI");
    p=present(d,Page::Probability);check(p.rows.size()==49,"All exact baselines displayed");
    for(const auto& r:p.rows){near(*r[1].numeric+*r[2].numeric,1,"Main complement");near(*r[3].numeric+*r[4].numeric,1,"Extra complement");near(*r[5].numeric+*r[6].numeric,1,"Any complement");}
    p=present(d,Page::Forecasts);check(p.summary.find("Insufficient held-out evidence")!=std::string::npos,"Insufficient evaluation visible");
    for(size_t n=0;n<49;++n){near(*p.rows[n][1].numeric,d.forecast.probabilities[n].main,"Forecast mapping",0);near(*p.rows[n][1].numeric+*p.rows[n][3].numeric,1,"Forecast complement");near(*p.rows[n][2].numeric,6.0/49,"Adjacent exact baseline",0);}
    d.backtest.evidence=Evidence::NoDemonstratedAdvantage;
    p=present(d,Page::Forecasts);check(p.summary.find("NO DEMONSTRATED PREDICTIVE ADVANTAGE")!=std::string::npos,"Negative evidence visible in summary");
    check(p.explanation.find("not a most-likely joint draw")!=std::string::npos&&p.explanation.find(d.snapshot->digest())!=std::string::npos,"Heuristic and immutable identity visible");
    auto rows=fixtures(2);rows[0].source.trust=Trust::Unverified;rows[1].unresolvedConflict=true;
    d.snapshot=std::make_shared<const Snapshot>(rows);d.coverage=coverage(*d.snapshot);d.analysis=analyze(*d.snapshot);d.forecast=forecast(*d.snapshot);d.backtest=backtest(*d.snapshot);
    p=present(d,Page::History);check(p.rows.size()==2&&p.rows[0][5].text=="Unverified import"&&p.rows[1][6].text=="UNRESOLVED","Ineligible history inspectable");
    check(d.analysis.draws==0&&present(d,Page::Numbers).chart.empty(),"Ineligible history excluded from chart");
    p=present(d,Page::Forecasts);check(p.summary.find("Insufficient training")!=std::string::npos&&!d.forecast.selection,"Empty eligible set has baseline not suggestion");
    p=present(d,Page::Calibration);check(p.rows.size()==60,"Calibration bins shown");
    for(const auto& r:p.rows)check(r[3].text=="0"&&r[4].text=="Unavailable"&&r[5].text=="Unavailable","Empty bins not fabricated rates");
    std::stop_source stop;stop.request_stop();rejects([&]{analyze(*d.snapshot,Scope::Main,{},100,stop.get_token());},"Analysis cancellation");
}
void workerTests() {
    using namespace std::chrono_literals;
    auto wait=[](auto& future){if(future.wait_for(5s)!=std::future_status::ready)throw std::runtime_error("Worker test timed out");};
    auto result=[&](LatestWorker<int>& worker){const auto end=std::chrono::steady_clock::now()+5s;while(worker.busy()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);check(!worker.busy(),"Worker completes");return worker.poll();};
    LatestWorker<int> worker;std::promise<void> started,release;auto entered=started.get_future();auto gate=release.get_future().share();
    worker.submit([&](std::stop_token){started.set_value();gate.wait_for(5s);return -1;});wait(entered);
    uint64_t latest=0;for(int i=0;i<100;++i)latest=worker.submit([i](std::stop_token){return i;});release.set_value();
    auto out=result(worker);check(out&&out->generation==latest&&out->value&&*out->value==99,"Only latest filter generation is published");check(!worker.poll(),"Worker result consumed once");
    worker.submit([](std::stop_token)->int{throw std::runtime_error("fixture failure");});out=result(worker);check(out&&!out->value&&out->error=="fixture failure","Worker error reaches view");
    std::promise<void> cancelStarted,cancelled;auto cancelEntry=cancelStarted.get_future();auto cancelDone=cancelled.get_future();
    worker.submit([&](std::stop_token stop){cancelStarted.set_value();const auto end=std::chrono::steady_clock::now()+5s;while(!stop.stop_requested()&&std::chrono::steady_clock::now()<end)std::this_thread::yield();cancelled.set_value();return -2;});
    wait(cancelEntry);worker.cancel();wait(cancelDone);check(!worker.busy()&&!worker.poll(),"Mode exit cancels without publishing stale data");
    std::promise<void> closeStarted;auto closeEntry=closeStarted.get_future();bool exited=false;
    worker.submit([&](std::stop_token stop){closeStarted.set_value();const auto end=std::chrono::steady_clock::now()+5s;while(!stop.stop_requested()&&std::chrono::steady_clock::now()<end)std::this_thread::yield();exited=true;return 1;});
    wait(closeEntry);worker.close();check(exited&&!worker.poll(),"Close joins worker before view destruction");rejects([&]{worker.submit([](std::stop_token){return 1;});},"Closed worker rejects jobs");worker.close();
}
void resampling() {
    check(!bootstrap({}).interval && !bootstrap(std::vector<double>(199, 1)).interval, "Bootstrap minimum gate");
    for (double constant : {-1.0, 0.0, 1.0}) {
        const auto c = bootstrap(std::vector<double>(203, constant));
        near(c.meanDifference, constant, "Mean sign convention"); near(c.interval->low, constant, "Constant lower tail with remainder");
        near(c.interval->high, constant, "Constant upper tail with remainder");
    }
    std::vector<double> varied; for (int i = 0; i < 203; ++i) varied.push_back(double(i % 7 - 3));
    const auto a = bootstrap(varied), b = bootstrap(varied);
    near(a.interval->low, b.interval->low, "Deterministic lower percentile", 0);
    near(a.interval->high, b.interval->high, "Deterministic upper percentile", 0);
    check(a.interval->low <= a.interval->high && a.interval->low >= -3 && a.interval->high <= 3, "Resampling bounded");
    // Independent, direct draw-by-draw reference (production uses prefix sums).
    std::mt19937_64 generator(BootstrapSeed); std::vector<double> reference;
    for (size_t replicate = 0; replicate < 2000; ++replicate) {
        std::vector<double> sampled;
        while (sampled.size() < varied.size()) {
            const uint64_t choices = varied.size() - 19;
            uint64_t raw; do { raw = generator(); } while (raw < (uint64_t(0) - choices) % choices);
            const size_t start = size_t(raw % choices);
            for (size_t j = 0; j < 20 && sampled.size() < varied.size(); ++j) sampled.push_back(varied[start + j]);
        }
        reference.push_back(std::accumulate(sampled.begin(), sampled.end(), 0.0) / double(sampled.size()));
    }
    std::sort(reference.begin(), reference.end());
    near(a.interval->low, reference[49] * 0.025 + reference[50] * 0.975, "Independent lower percentile");
    near(a.interval->high, reference[1949] * 0.975 + reference[1950] * 0.025, "Independent upper percentile");
    rejects([] { bootstrap({std::numeric_limits<double>::quiet_NaN()}); }, "NaN difference rejected");
    std::stop_source stop; stop.request_stop(); rejects([&] { bootstrap(varied, stop.get_token()); }, "Bootstrap cancellation");
}
}
size_t testLearningProgress();
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--hkjc-saved-check") { verifySavedHkjc(argv[2], {}); return 0; }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--hkjc-cache-check") { verifySavedHkjc(argv[2], argv[3]); return 0; }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--official-report") { officialReport(argv[2]); return 0; }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--forecast-lab-report") { forecastLabOfficialReport(argv[2]); return 0; }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--adaptive-report") { adaptiveOfficialReport(argv[2],argv[3]); return 0; }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--adaptive-benchmark") { adaptiveOfficialBenchmark(argv[2],argv[3]); return 0; }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--official-archive-check") { officialArchiveCheck(argv[2], argv[3]); return 0; }
        if (argc != 1) throw std::invalid_argument("Usage: MarkSixStatisticsTests [--official-report AUDITED_SNAPSHOT_DIRECTORY | --official-archive-check AUDITED_SNAPSHOT_DIRECTORY NEW_ISOLATED_ROOT | --adaptive-report AUDITED_SNAPSHOT_DIRECTORY NEW_REPORT_FILE | --adaptive-benchmark AUDITED_SNAPSHOT_DIRECTORY NEW_ISOLATED_ROOT]");
        for (const auto& [name, test] : std::vector<std::pair<const char*, std::function<void()>>>{
                 {"domain/provenance/digests", domain}, {"exact probabilities", exactMath}, {"Wilson intervals", uncertainty},
                 {"descriptive analysis", descriptive}, {"coverage/era browsing", coverageTests}, {"fixed shrinkage/ranking", model}, {"scoring/calibration", scoring},
                 {"chronological leakage/evidence", chronological}, {"paired block bootstrap", resampling},
                 {"table/chart/forecast presentation", presentationTests}, {"worker supersession/cancellation/lifetime", workerTests}}) {
            test(); std::cout << "PASS " << name << '\n';
        }
        checks += testAdaptive();
        checks += testForecastLab();
        checks += testAdaptiveLedger();
        checks += testLearningProgress();
        checks += testArchive();
        checks += testUpdatePlan();
        checks += testUpdateWorker();
        checks += testHkjcProvider();
        std::cout << "STATISTICS PASS checks=" << checks << " fixtures=synthetic network=none physics=none\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "STATISTICS FAIL: " << e.what() << '\n'; return 1; }
}
