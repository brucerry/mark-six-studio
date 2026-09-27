// Read-only regression/report adapter for ONE independently audited local snapshot.
// Not linked into the application, not a general import/provider or trust override.
#include "forecast.hpp"
#include "adaptive_ledger.hpp"
#include "forecast_lab.hpp"
#define NOMINMAX
#include <Windows.h>
#include <psapi.h>
#include <chrono>
#include <fstream>
#include "import.hpp"
#include "coverage.hpp"
#include <sqlite3.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <regex>
#include <stdexcept>
using namespace marksix::statistics;
namespace {
constexpr auto ManifestHash = "a82eb630dac350c0c4f4986b8b6bef6aea6fd335a6b90adcdb85780a91346479";
constexpr auto CsvHash = "c196f44bd10e904cc0ea3437e22f6be24f5768b83261a57d46774726e75e6ff8";
struct Window { std::string retrieved, hash; };
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Snapshot auditedSnapshot(const std::filesystem::path& root) {
    const auto manifest = readBounded(root / "manifest.json");
    const auto csv = readBounded(root / "all-results.csv");
    require(sha256(manifest) == ManifestHash && sha256(csv) == CsvHash, "Only the pinned, independently audited official snapshot is accepted by this regression adapter");
    require(sha256(readBounded(root / "source.json")) == "90d14ab926c9945f75e04f7c0ea570baf7f1ea3dffd79095db5fa09da086ef52", "Source receipt changed");
    require(sha256(readBounded(root / "query.graphql")) == "156c024a7a77406c6bf2127dcb5b029ba83b692f501aa3635911f7bdc029724d", "Public query changed");
    sqlite3* db = nullptr; require(sqlite3_open(":memory:", &db) == SQLITE_OK, "Cannot parse audited manifest");
    sqlite3_stmt* query = nullptr; std::map<std::string, Window> windows;
    try {
        require(sqlite3_prepare_v2(db, "SELECT json_extract(value,'$.file'),json_extract(value,'$.retrievedUtc'),json_extract(value,'$.sha256') FROM json_each(?,'$.windows')", -1, &query, nullptr) == SQLITE_OK, "Cannot prepare audited manifest");
        sqlite3_bind_text(query, 1, manifest.data(), int(manifest.size()), SQLITE_TRANSIENT);
        const auto text = [&](int n) { const auto* value = sqlite3_column_text(query, n); require(value != nullptr, "Missing audited receipt field"); return std::string(reinterpret_cast<const char*>(value)); };
        int rc;
        while ((rc = sqlite3_step(query)) == SQLITE_ROW) {
            const auto file = text(0), retrieved = text(1), hash = text(2);
            require(std::regex_match(file, std::regex("raw/[0-9]{8}-[0-9]{8}\\.json")), "Unsafe raw response path");
            require(sha256(readBounded(root / file, 2 * 1024 * 1024)) == hash, "Audited raw response changed");
            windows.emplace(file.substr(4, 17), Window{retrieved, hash});
        }
        require(rc == SQLITE_DONE && windows.size() == 135, "Incomplete audited window inventory");
        sqlite3_finalize(query); query = nullptr; sqlite3_close(db); db = nullptr;
    } catch (...) { sqlite3_finalize(query); if (db) sqlite3_close(db); throw; }
    const std::string importMeta = std::string("{\"schema\":\"marksix-csv-v1\",\"csvSha256\":\"") + CsvHash +
        "\",\"sourceId\":\"hkjc-results\",\"url\":\"https://bet.hkjc.com/en/marksix/results\","
        "\"retrievedUtc\":\"2026-09-25T08:57:31.1280248Z\",\"lineage\":\"HKJC direct\",\"status\":\"unverified\",\"extractionOrder\":\"unknown\"}";
    auto rows = parseImport(csv, importMeta); const auto table = parseCsv(csv);
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& window = windows.at(table[i + 1][11]);
        auto& source = rows[i].source; source.retrievedUtc = window.retrieved; source.contentSha256 = window.hash;
        source.trust = Trust::OfficialRetrieval;
        source.verificationEvidence = std::string("audited-local-manifest:") + ManifestHash + ";provider-id=" + table[i + 1][10];
    }
    require(rows.size() == 4391, "Audited row count changed"); return Snapshot(std::move(rows));
}
void lossRow(const char* name, const Loss& loss) {
    std::cout << "| " << name << " | " << loss.brier << " | " << loss.log << " | " << loss.mainBrier << " | " << loss.extraBrier << " | " << loss.anyBrier << " |\n";
}
void comparison(const char* name, const Comparison& c) {
    std::cout << "- " << name << " baseline-minus-model: " << c.meanDifference;
    if (c.interval) std::cout << "; paired 95% interval [" << c.interval->low << ", " << c.interval->high << "]";
    else std::cout << "; interval unavailable: fewer than 200 test draws";
    std::cout << '\n';
}
}
void forecastLabOfficialReport(const std::filesystem::path& root) {
    const auto began = std::chrono::steady_clock::now();
    const auto snapshot = auditedSnapshot(root);
    const auto replay = replayLab(snapshot);
    require(replay.available && replay.adaptive.draws == 3380 && replay.steps.size() == 3379,
            "Pinned Forecast Lab replay incomplete");
    const auto evaluation = evaluateLab(replay);
    require(evaluation.commonTargets == 2980 && evaluation.selector.targets == 2980,
            "Forecast Lab outer denominator changed");
    const auto repeat = replayLab(snapshot);
    require(repeat.next && replay.next &&
            repeat.adaptive.prefixDigest == replay.adaptive.prefixDigest &&
            repeat.next->decision.candidate == replay.next->decision.candidate &&
            repeat.next->selections[repeat.next->decision.candidate].main ==
                replay.next->selections[replay.next->decision.candidate].main,
            "Forecast Lab independent replay differs");
    const auto controls = labControls(replay.adaptive.draws,
        evaluation.selector.meanHits() - 36.0/49.0, snapshot.digest());
    require(controls.complete && controls.completed == 199,
            "Forecast Lab fair controls incomplete");
    std::array<size_t, 15> chosen{};
    for (const auto& step : replay.steps)
        if (step.frozen.completedDraws >= 400) ++chosen[step.frozen.decision.candidate];
    PROCESS_MEMORY_COUNTERS memory{}; memory.cb = sizeof memory;
    require(K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory) != 0,
            "Cannot measure Forecast Lab process memory");
    const auto millis = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - began).count();
    const auto stats = std::minmax_element(controls.nullStatistics.begin(),
                                           controls.nullStatistics.end());
    const double nullMean = std::accumulate(controls.nullStatistics.begin(),
        controls.nullStatistics.end(), 0.0) / controls.completed;
    std::cout << std::setprecision(17) << "FORECAST_LAB_V1_AUDITED_DEVELOPMENT_REPORT\n"
        << "snapshot=" << snapshot.digest() << "\nselection=" << snapshot.selectionDigest()
        << "\nmanifest=" << ManifestHash << "\nprotocol=" << labProtocolDigest()
        << "\neligible=" << replay.adaptive.draws << "\nexcluded="
        << snapshot.records().size() - replay.adaptive.draws
        << "\nouter_targets=" << evaluation.commonTargets
        << "\nselector_mean_hits=" << evaluation.selector.meanHits()
        << "\nadaptive_mean_hits=" << evaluation.adaptive.meanHits()
        << "\nfixed_mean_hits=" << evaluation.fixed.meanHits()
        << "\nuniform_realized_mean_hits=" << evaluation.uniform.meanHits()
        << "\nfair_expected_mean_hits=" << evaluation.fairExpectedMainHits
        << "\nselector_log=" << evaluation.selector.meanLoss.log
        << "\nadaptive_log=" << evaluation.adaptive.meanLoss.log
        << "\nfixed_log=" << evaluation.fixed.meanLoss.log
        << "\nselector_brier=" << evaluation.selector.meanLoss.brier
        << "\nadaptive_brier=" << evaluation.adaptive.meanLoss.brier
        << "\nfixed_brier=" << evaluation.fixed.meanLoss.brier
        << "\nselector_minus_adaptive_hits=" << evaluation.versusAdaptive.meanHitDifference
        << "\nselector_minus_fixed_hits=" << evaluation.versusFixed.meanHitDifference
        << "\nselector_minus_uniform_hits=" << evaluation.versusUniform.meanHitDifference;
    if (evaluation.versusAdaptive.hitInterval)
        std::cout << "\nselector_minus_adaptive_95_low=" << evaluation.versusAdaptive.hitInterval->low
                  << "\nselector_minus_adaptive_95_high=" << evaluation.versusAdaptive.hitInterval->high;
    std::cout << "\nhit_histogram=";
    for (size_t hit = 0; hit <= 6; ++hit)
        std::cout << (hit ? "," : "") << evaluation.selector.exactly(hit);
    std::cout << "\nchosen_targets=";
    for (size_t j = 0; j < chosen.size(); ++j)
        std::cout << (j ? "," : "") << labCandidates()[j].id << ':' << chosen[j];
    std::cout << "\nnext_model=" << labCandidates()[replay.next->decision.candidate].id
              << "\nnext_fallback=" << replay.next->decision.fallback
              << "\nnext_cutoff=" << replay.next->cutoff << "\nnext_main=";
    for (int number : replay.next->selections[replay.next->decision.candidate].main)
        std::cout << number << ',';
    std::cout << "\ncontrols_completed=" << controls.completed
              << "\ncontrol_master_seed=" << controls.masterSeed
              << "\ncontrol_null_min=" << *stats.first
              << "\ncontrol_null_mean=" << nullMean
              << "\ncontrol_null_max=" << *stats.second
              << "\ncontrol_right_tail=" << controls.tailFraction
              << "\ncontrol_simulation_se=" << controls.simulationStdError
              << "\nwhole_run_milliseconds=" << millis
              << "\npeak_working_set_bytes=" << memory.PeakWorkingSetSize
              << "\ninterpretation=Inspected development replay and full-procedure fair controls; no fresh historical holdout or guaranteed next-draw advantage.\n";
    for (size_t i = 0; i < controls.completed; ++i)
        std::cout << "control_" << i + 1 << '=' << controls.historyHashes[i]
                  << ',' << controls.nullStatistics[i] << '\n';
}
void officialReport(const std::filesystem::path& root) {
    const auto snapshot = auditedSnapshot(root); const auto result = backtest(snapshot); const auto next = forecast(snapshot);
    const auto analysis = analyze(snapshot);
    const auto summary = coverage(snapshot);
    require(summary.total == 4391 && summary.eligibleCurrent49 == 3380 && summary.earlierQuarantined == 1011 &&
            summary.sourceStatuses[1] == 4391 && summary.conflicts == 0 && !summary.complete && summary.sources.size() == 1 &&
            browseEra(snapshot, Era::EarlierUnreviewed).size() == 1011, "Real coverage/era browsing regression");
    require(result.eligibleDraws == 3380 && result.excluded == 1011 && result.draws.size() == 3180, "Real snapshot eligibility regression");
    const auto repeat = backtest(snapshot);
    require(repeat.snapshotDigest == result.snapshotDigest && repeat.meanModel.brier == result.meanModel.brier &&
            repeat.meanModel.log == result.meanModel.log && repeat.brierDifference.interval->low == result.brierDifference.interval->low &&
            repeat.brierDifference.interval->high == result.brierDifference.interval->high, "Real report replay mismatch");
    std::cout << std::setprecision(17);
    std::cout << "# HKJC historical model comparison - fixed v1\n\n"
        << "Source: https://bet.hkjc.com/en/marksix/results ; local collection 2026-09-25.\n\n"
        << "Snapshot SHA-256: `" << snapshot.digest() << "`\n\nSelection SHA-256: `" << result.selectionDigest << "`\n\n"
        << "Audited collection manifest SHA-256: `" << ManifestHash << "`\n\n"
        << "4,391 retrieved rows; 3,380 eligible current-49 draws, 2002-07-04 through 2026-09-22. "
        << "1,011 earlier-era rows excluded pending exact pool-boundary review. Zero current-era conflicts/unverified rows in this pinned snapshot. "
        << "Independent expected draw inventory is unknown; no all-history completeness claim. Sorted main sets are not extraction order.\n\n"
        << "Model: `" << ModelVersion << "`; lambda=100, minimum training=200, expanding history, no tuning. "
        << "Test draws: " << result.draws.size() << ", " << result.draws.front().date << " (" << result.draws.front().id << ") through "
        << result.draws.back().date << " (" << result.draws.back().id << "). "
        << "Each prediction uses strictly earlier eligible records. Retrospective current corrected records, not an as-published historical forecast.\n\n"
        << "| Method | Mean marginal 3-category Brier | Mean marginal log loss | Main binary Brier | Extra binary Brier | Any binary Brier |\n"
        << "|---|---:|---:|---:|---:|---:|\n";
    lossRow("Uniform fair-independent baseline", result.meanUniform); lossRow("Fixed frequency shrinkage", result.meanModel);
    std::cout << '\n'; comparison("Primary Brier", result.brierDifference); comparison("Secondary marginal log loss", result.logDifference);
    std::cout << "\nUncertainty: paired draw-level non-wrapping moving blocks of 20, 2,000 replicates; "
        << "mt19937_64 seed " << BootstrapSeed << ", rejection-bounded uniform starts; final block truncated to test length. "
        << "95% percentile endpoints interpolate ranks p*(1999). Approximate, temporal-dependence/nonstationarity sensitive; not 49 independent samples per draw.\n\n";
    if (result.evidence == Evidence::HistoricalHeldOutImprovement)
        std::cout << "Evidence: historical held-out primary-score improvement; no established future or winning-odds advantage.\n\n";
    else std::cout << "Evidence: NO DEMONSTRATED PREDICTIVE ADVANTAGE. Negative results retained without parameter retuning.\n\n";
    std::cout << "## Calibration (fixed bins [lower,upper), last bin includes 1)\n\n"
        << "| Method | Scope | Bin | Samples | Mean predicted | Observed rate |\n|---|---|---|---:|---:|---:|\n";
    for (int method = 0; method < 2; ++method) for (int scope = 0; scope < 3; ++scope) for (int bin = 0; bin < 10; ++bin) {
        const auto& b = (method ? result.modelCalibration : result.uniformCalibration)[scope][bin];
        std::cout << "| " << (method ? "Model" : "Baseline") << " | " << (scope == 0 ? "Main" : scope == 1 ? "Extra" : "Any")
            << " | " << bin << "/10 to " << (bin + 1) << "/10 | " << b.samples << " | ";
        if (b.samples) std::cout << *b.meanPrediction() << " | " << *b.observedRate(); else std::cout << "unavailable | unavailable";
        std::cout << " |\n";
    }
    std::cout << "\n## All-number estimates, next draw after data cutoff 2026-09-22\n\n"
        << "No upcoming draw identifier/schedule is asserted. Freshness beyond the collection is unknown. "
        << "Baseline main=6/49, Extra=1/49, any=7/49; complements=43/49,48/49,42/49. "
        << "Exact six-main-set baseline=1/13,983,816. Marginals cannot be multiplied into a joint winning likelihood.\n\n"
        << "| Number | Main observed count / 3380 | Empirical main rate | Model main | Model Extra | Model any | Model absent |\n"
        << "|---:|---:|---:|---:|---:|---:|---:|\n";
    for (size_t n = 0; n < 49; ++n) {
        const auto& q = next.probabilities[n]; std::cout << "| " << n + 1 << " | " << analysis.numbers[n].count << " | "
            << *analysis.numbers[n].observedRate << " | " << q.main << " | " << q.extra << " | " << q.any() << " | " << q.absent << " |\n";
    }
    std::cout << "\n" << SelectionLabel << "\n\nExperimental selection: ";
    for (int n : next.selection->main) std::cout << n << ' ';
    std::cout << "; distinct Extra suggestion: " << next.selection->extra << ".\n\n"
        << "No affiliation, wagering integration, fairness certification or guaranteed prediction. "
        << "This diagnostic does not modify an application database, simulation history or any physical model. "
        << "Exact same-build replay of the identified snapshot passed.\n";
}
void officialArchiveCheck(const std::filesystem::path& snapshotRoot, const std::filesystem::path& outputRoot) {
    const auto original = auditedSnapshot(snapshotRoot);
    require(!std::filesystem::exists(outputRoot) && std::filesystem::create_directory(outputRoot),
            "Real archive check requires a new, nonexistent isolated output directory");
    {
        Archive archive(outputRoot);
        const auto result = archive.ingest(original.records());
        require(result.inserted == 4391 && result.conflicts == 0 && archive.snapshot().digest() == original.digest(),
                "Real SQLite import differs from audited snapshot");
        const auto version = archive.version(); const auto duplicate = archive.ingest(original.records());
        require(duplicate.inserted == 0 && duplicate.duplicate == 4391 && archive.version() == version,
                "Real SQLite reimport is not idempotent");
    }
    Archive reopened(outputRoot); const auto restored = reopened.snapshot(); const auto summary = coverage(restored);
    require(restored.digest() == original.digest() && summary.total == 4391 && summary.eligibleCurrent49 == 3380 &&
            summary.earlierQuarantined == 1011 && !summary.complete, "Real SQLite restart/coverage differs");
    const auto expected = backtest(original), actual = backtest(restored);
    require(expected.meanModel.brier == actual.meanModel.brier && expected.brierDifference.interval->low == actual.brierDifference.interval->low,
            "Persisted real snapshot changes model report");
    std::cout << "REAL_ARCHIVE PASS rows=4391 eligible49=3380 earlierQuarantined=1011 reimport=no-op restart=identical model=identical\n"
              << "Snapshot: " << restored.digest() << "\nDatabase: " << reopened.path().string() << '\n';
}

void adaptiveOfficialReport(const std::filesystem::path& root, const std::filesystem::path& output) {
    require(!std::filesystem::exists(output), "Adaptive report requires a fresh output path");
    require(adaptiveProtocolDigest()=="ec44250351655a86a80b8fe32a58ab5c06501e328408fddda8a4b210a2222de2", "Frozen protocol changed before scoring");
    const auto snapshot=auditedSnapshot(root);
    const auto start=std::chrono::steady_clock::now(); const auto replay=replayAdaptive(snapshot);
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    const auto repeat=replayAdaptive(snapshot);
    require(replay.available && replay.state.draws==3380 && replay.excluded==1011 && replay.steps.size()==3379 &&
        encodeAdaptiveState(replay.state)==encodeAdaptiveState(repeat.state),"Real adaptive replay/count regression");
    for(size_t i=0;i<replay.steps.size();++i)require(replay.steps[i].adaptiveLoss.log==repeat.steps[i].adaptiveLoss.log &&
        replay.steps[i].adaptiveLoss.brier==repeat.steps[i].adaptiveLoss.brier,"Real per-step replay mismatch");
    const auto fixed=backtest(snapshot);const auto evaluation=evaluateAdaptive(replay,fixed);
    require(evaluation.all.adaptive.draws==3379 && evaluation.common.adaptive.draws==3180 &&
        evaluation.common.fixed.mean.log==fixed.meanModel.log,"Common window must preserve fixed-v1 reference");
    std::ofstream out(output);require(bool(out),"Cannot create report");out<<std::setprecision(17);
    out<<"# Adaptive v1 saved-history evaluation\n\nProtocol frozen before scoring: `"<<adaptiveProtocolDigest()<<"`\n\n"
       <<"Snapshot: `"<<snapshot.digest()<<"`\n\nAudited manifest: `"<<ManifestHash<<"`\n\n"
       <<"4,391 archived; 3,380 eligible, 1,011 earlier-era exclusions; seed 02/053 on 2002-07-04. Cutoff 2026-09-22. "
       <<"Coverage completeness and later freshness unknown. Current corrected records are historical replay, not as-published forecasts. "
       <<"No protocol retuning after observing these results. Fixed-v1 backtest remains unchanged.\n\n"
       <<"Initial in-memory replay: "<<elapsed<<" ms; processed 3380 draws. Same-build repeat state and all per-step losses exactly matched.\n\n";
    for(const auto* window:{&evaluation.all,&evaluation.common}){
        const bool common=window==&evaluation.common;
        out<<"## "<<(common?"Common window (minimum 200 training draws)":"All scored draws (from second eligible draw)")<<"\n\n"
           <<window->firstDate<<" ("<<window->firstId<<") to "<<window->lastDate<<" ("<<window->lastId<<").\n\n"
           <<"| Model | Draws | Marginal log | Category Brier | Main Brier | Extra Brier | Any Brier | Main hits total | Extra matches |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
        for(int m=0;m<(common?3:2);++m){const auto& d=m==0?window->adaptive:m==1?window->uniform:window->fixed;
            out<<"| "<<(m==0?"Adaptive":m==1?"Uniform":"Fixed v1")<<" | "<<d.draws<<" | "<<d.mean.log<<" | "<<d.mean.brier<<" | "<<d.mean.mainBrier<<" | "<<d.mean.extraBrier<<" | "<<d.mean.anyBrier<<" | "<<d.mainHits<<" | "<<d.extraMatches<<" |\n";}
        out<<"\nPositive comparator-minus-adaptive favors adaptive; negative favors comparator.\n\n";
        for(int i=0;i<(common?4:2);++i){const auto& c=i==0?window->uniformMinusAdaptiveLog:i==1?window->uniformMinusAdaptiveBrier:i==2?window->fixedMinusAdaptiveLog:window->fixedMinusAdaptiveBrier;
            out<<"- "<<(i==0?"Uniform log":i==1?"Uniform Brier":i==2?"Fixed log":"Fixed Brier")<<": "<<c.meanDifference;
            if(c.interval)out<<"; descriptive 95% interval ["<<c.interval->low<<", "<<c.interval->high<<"]";out<<"\n";}
        out<<"\n### Calibration\n\n| Model | Scope | Bin | Samples | Mean predicted | Observed rate |\n|---|---|---:|---:|---:|---:|\n";
        for(int m=0;m<(common?3:2);++m)for(int scope=0;scope<3;++scope)for(int bin=0;bin<10;++bin){
            const auto& d=m==0?window->adaptive:m==1?window->uniform:window->fixed;const auto& b=d.calibration[scope][bin];
            out<<"| "<<(m==0?"Adaptive":m==1?"Uniform":"Fixed")<<" | "<<(scope==0?"Main":scope==1?"Extra":"Any")<<" | "<<bin<<" | "<<b.samples<<" | ";
            if(b.samples)out<<*b.meanPrediction()<<" | "<<*b.observedRate();else out<<"unavailable | unavailable";out<<" |\n";}
    }
    out<<"\n## Interpretation\n\n"<<evaluation.evidence<<" "
       <<"Intervals use paired draw-level moving blocks (20 draws, 2,000 replicates), not 49 independent observations per draw. "
       <<"They are descriptive development diagnostics, not selection-adjusted confirmation. Hit counts use deterministic tie-breaking and are not winning-odds estimates. "
       <<"Uniform fair-independent main probability stays 6/49. A changing estimate does not establish predictability.\n\n"
       <<"No network requests, source writes, simulation writes, or physical tests.\n";
    require(bool(out),"Report write failed");
    std::cout<<"ADAPTIVE_REPORT PASS draws=3380 scored=3379 common=3180 replay_ms="<<elapsed<<" output="<<output.string()<<'\n';
}

void adaptiveOfficialBenchmark(const std::filesystem::path& root,const std::filesystem::path& destination){
    require(!std::filesystem::exists(destination),"Benchmark requires new isolated directory");
    const auto snapshot=auditedSnapshot(root);auto prefix=snapshot.select();prefix.resize(prefix.size()-2);
    const auto clean=replayAdaptive(snapshot);std::filesystem::create_directories(destination);
    std::ofstream out(destination/"benchmark.txt");require(bool(out),"Cannot create benchmark evidence");
    AdaptiveLedger ledger(destination);const auto measure=[&](const char* label,const Snapshot& data,uint64_t version,size_t expected){
        const auto start=std::chrono::steady_clock::now();const auto result=ledger.reconcile(data,version,"2026-09-26T00:00:00Z");
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        require(result.current&&result.processed==expected,"Unexpected benchmark learning count");
        out<<label<<" processed="<<result.processed<<" training="<<result.current->state.draws<<" elapsed_ms="<<ms<<'\n';
        std::cout<<"ADAPTIVE_BENCH "<<label<<" processed="<<result.processed<<" training="<<result.current->state.draws<<" elapsed_ms="<<ms<<'\n';
    };
    measure("initial-ledger",Snapshot(prefix),1,3377);measure("append-two",snapshot,2,2);measure("identical-reload",snapshot,2,0);
    require(encodeAdaptiveState(ledger.head()->state)==encodeAdaptiveState(clean.state),"Real append differs from clean replay");
    out<<"Real append state exactly matches clean replay. Timings include validation/SQLite; no-op still validates the prefix. Hardware-specific, not a performance guarantee.\n";
    require(bool(out),"Benchmark output failed");
}
