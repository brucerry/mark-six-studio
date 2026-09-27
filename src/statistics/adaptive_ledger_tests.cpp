#include "adaptive_ledger.hpp"
#include "learning_progress.hpp"
#include "archive.hpp"
#include <sqlite3.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace marksix::statistics;
namespace {
size_t checks = 0;
void check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f, const char* message) {
    bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; } check(rejected,message);
}
Result row(int i) {
    Result r; r.id = "02/05" + std::to_string(i+3); r.date = "2002-07-0" + std::to_string(i+4);
    r.main = {1,2,3,4,5,6}; r.extra = i+7;
    r.source = {"ledger-fixture", "https://example.invalid/ledger", "2026-09-25T00:00:00Z", sha256("synthetic"),
        "synthetic-not-real", Trust::OfficialRetrieval, "test-only-evidence"}; return r;
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary); if (!in) throw std::runtime_error("Fixture missing");
    return std::string(std::istreambuf_iterator<char>(in),{});
}
bool sql(const std::filesystem::path& path, const char* command) {
    sqlite3* db = nullptr; const auto name = path.u8string();
    if (sqlite3_open(reinterpret_cast<const char*>(name.c_str()),&db) != SQLITE_OK) throw std::runtime_error("Cannot open ledger fixture");
    const int rc = sqlite3_exec(db,command,nullptr,nullptr,nullptr); sqlite3_close(db); return rc == SQLITE_OK;
}
constexpr const char* Now = "2026-09-25T15:00:00Z";
void incremental(const std::filesystem::path& root) {
    const Snapshot first({row(0)}), full({row(0),row(1),row(2),row(3)});
    const auto reference = replayAdaptive(full); const auto initial = replayAdaptive(first);
    int64_t id = 0; std::string oldPrediction;
    {
        AdaptiveLedger ledger(root);
        check(ledger.reconcileAppend(first,1,Now).status == AdaptiveReconcileStatus::NeedsReplay, "Empty ledger requests initial replay");
        id = ledger.publishReplay(initial,1,Now,"Incremental fixture",0);
        oldPrediction = encodeAdaptiveState(ledger.predictions(id).front().before);
        const auto unchangedBytes = bytes(ledger.path()); size_t calls = 0;
        auto unchanged = ledger.reconcileAppend(first,1,Now,{},[&](size_t) { ++calls; });
        check(unchanged.status == AdaptiveReconcileStatus::Current && unchanged.current && unchanged.processed == 0 && calls == 0, "Identical reload performs no learning");
        check(bytes(ledger.path()) == unchangedBytes, "No-op preserves ledger bytes");
        std::stop_source stop;
        rejects([&] { ledger.reconcileAppend(full,4,Now,stop.get_token(),[&](size_t n) { if (n == 2) stop.request_stop(); }); }, "Cancellation after first written row rolls back batch");
        check(ledger.head()->state.draws == 1 && ledger.predictions(id).size() == 1 && !ledger.predictions(id).front().outcome, "Cancelled partial append leaves pending checkpoint untouched");
        check(bytes(ledger.path()) == unchangedBytes, "Cancelled append restores original database bytes");
        std::vector<size_t> progress;
        const auto added = ledger.reconcileAppend(full,4,Now,{},[&](size_t n) { progress.push_back(n); });
        check(added.processed == 3 && progress == std::vector<size_t>{1,2,3}, "Only three new draws learned exactly once");
        check(added.current && added.current->lineage.id == id && ledger.lineages().size() == 1, "Append keeps compatible lineage");
        check(encodeAdaptiveState(added.current->state) == encodeAdaptiveState(reference.state), "Append exactly equals clean replay");
        check(added.snapshotDigest == full.digest() && added.sourceVersion == 4, "Current source identity returned separately from lineage origin");
        check(encodeAdaptiveState(ledger.predictions(id).front().before) == oldPrediction, "Original pending prediction unchanged after outcome attachment");
        const auto completedBytes = bytes(ledger.path());
        const auto again = ledger.reconcileAppend(full,4,Now);
        check(again.processed == 0 && again.current && bytes(ledger.path()) == completedBytes, "Repeated batch learns zero draws and makes no writes");
    }
    {
        AdaptiveLedger ledger(root);
        check(encodeAdaptiveState(ledger.head()->state) == encodeAdaptiveState(reference.state), "Append checkpoint survives restart exactly");
        auto refreshed = full.records(); for (auto& r : refreshed) r.source.retrievedUtc = "2026-09-26T00:00:00Z";
        const Snapshot metadata(refreshed); const auto before = bytes(ledger.path());
        const auto noLearning = ledger.reconcileAppend(metadata,5,"2026-09-26T01:00:00Z");
        check(noLearning.processed == 0 && noLearning.current && noLearning.snapshotDigest == metadata.digest(), "Freshness-only changes do not retrain");
        check(bytes(ledger.path()) == before, "Freshness metadata does not rewrite model evidence");
        auto changed = full.records(); changed[1].extra = 20;
        const auto incompatible = ledger.reconcileAppend(Snapshot(changed),6,Now);
        check(incompatible.status == AdaptiveReconcileStatus::NeedsReplay && !incompatible.current && incompatible.processed == 0, "Correction cannot reuse incompatible state");
        const auto deleted = ledger.reconcileAppend(first,7,Now);
        check(deleted.status == AdaptiveReconcileStatus::NeedsReplay && !deleted.current, "Shorter history not current");
        const auto missing = ledger.reconcileAppend(Snapshot({row(1)}),8,Now);
        check(missing.status == AdaptiveReconcileStatus::Unavailable && !missing.current, "Missing seed never exposes old current prediction");
        check(bytes(ledger.path()) == before, "Incompatible snapshots preserve old lineage");
        auto next = full.records(); next.push_back(row(4));
        const Snapshot extended(next);
        const auto afterRestart = ledger.reconcileAppend(extended,9,"2026-09-26T02:00:00Z");
        check(afterRestart.processed == 1 && afterRestart.current && encodeAdaptiveState(afterRestart.current->state) == encodeAdaptiveState(replayAdaptive(extended).state), "Restart resumes one new draw without replay");
        const auto records = ledger.predictions(id);
        check(records.size() == 5 && !records.back().outcome, "One pending forecast after resumed batch");
        for (size_t i = 0; i < reference.steps.size(); ++i) {
            check(records[i].outcome && records[i].outcome->adaptiveLoss.log == reference.steps[i].adaptiveLoss.log, "Incremental scores equal replay");
        }
    }
}
void storage(const std::filesystem::path& root) {
    const Snapshot snapshot({row(0),row(1),row(2)}); const auto replay = replayAdaptive(snapshot);
    const auto packed = encodeAdaptiveState(replay.state);
    check(encodeAdaptiveState(decodeAdaptiveState(packed)) == packed, "Lossless checkpoint round trip");
    rejects([&] { decodeAdaptiveState(packed.substr(0,packed.size()-1)); }, "Truncated state rejected");
    rejects([&] { decodeAdaptiveState(packed+"x"); }, "Trailing state rejected");
    rejects([&] { decodeAdaptiveState(std::string(40000,'x')); }, "Oversized state rejected");
    auto bad = packed; bad[0] = char(-1); rejects([&] { decodeAdaptiveState(bad); }, "Unbounded string rejected");
    { Archive archive(root); archive.ingest({row(0),row(1),row(2)}); }
    const auto sourcePath = root/"statistics"/"results.sqlite3", historyPath = root/"simulation-history-fixture.txt";
    { std::ofstream fixture(historyPath,std::ios::binary); fixture << "simulation history sentinel: must not change\n"; }
    const auto sourceBefore = bytes(sourcePath), historyBefore = bytes(historyPath);
    int64_t id = 0; std::filesystem::path path;
    {
        AdaptiveLedger ledger(root); path = ledger.path(); check(!ledger.head(), "Initially empty ledger");
        id = ledger.publishReplay(replay,3,Now,"Initial synthetic replay",0);
        check(id > 0 && ledger.lineages().size() == 1, "Published one lineage");
        const auto head = ledger.head(); check(head && head->lineage.id == id && encodeAdaptiveState(head->state) == packed, "Checkpoint publication exact");
        auto records = ledger.predictions(id);
        check(records.size() == 3 && records[0].outcome && records[1].outcome && !records[2].outcome, "Two scored immutable predictions and pending next");
        check(records[0].provenance == "historical-replay" && records[2].provenance == "pending-unbound", "No manufactured pre-fetch claim");
        check(records[0].outcomeRevision == revisionDigest(row(1)), "Outcome source revision retained");
        check(records[0].createdUtc == Now && head->lineage.sourceVersion == 3, "Creation and source version retained");
        for (size_t i = 0; i < replay.steps.size(); ++i) {
            check(encodeAdaptiveState(records[i].before) == encodeAdaptiveState(replay.steps[i].before), "Before-state exact");
            check(encodeAdaptiveState(records[i].outcome->after) == encodeAdaptiveState(replay.steps[i].after), "After-state exact");
            check(records[i].outcome->adaptiveLoss.log == replay.steps[i].adaptiveLoss.log, "Stored score exact");
            for (size_t j = 0; j < 49; ++j) check(records[i].prediction.probabilities[j].main == replay.steps[i].prediction.probabilities[j].main &&
                records[i].prediction.probabilities[j].extra == replay.steps[i].prediction.probabilities[j].extra, "Stored marginal double exact");
        }
        check(ledger.predictions(id,1,1).front().before.draws == 2, "Stable page offset");
        rejects([&] { ledger.predictions(id,0,201); }, "Bounded prediction page");
        rejects([&] { ledger.publishReplay(replay,3,Now,"Stale caller",0); }, "Head compare-and-swap");
        auto broken = replay; broken.steps[1].after.logWeights[0] -= 0.25;
        rejects([&] { ledger.publishReplay(broken,3,Now,"Malformed chain",id); }, "Mid-publication mismatch rolls back");
        check(ledger.lineages().size() == 1 && ledger.head()->lineage.id == id, "Failed replay preserves committed lineage");
        check(sql(path,"CREATE TRIGGER inject_failure BEFORE INSERT ON outcomes BEGIN SELECT RAISE(ABORT,'Injected write failure'); END;"), "Install isolated SQLite failure");
        rejects([&] { ledger.publishReplay(replay,3,Now,"Disk-like write failure",id); }, "SQLite transaction failure rolls back");
        check(sql(path,"DROP TRIGGER inject_failure;"), "Remove isolated failure");
        check(ledger.lineages().size() == 1, "No partial lineage survived");
        std::stop_source stop; stop.request_stop();
        rejects([&] { ledger.publishReplay(replay,3,Now,"Cancelled",id,stop.get_token()); }, "Cancelled publication rejected");
        check(!sql(path,"UPDATE predictions SET prefix='modified';"), "Prediction updates prohibited");
        check(!sql(path,"DELETE FROM outcomes;"), "Score deletion prohibited");
        check(!sql(path,"UPDATE lineages SET reason='rewritten';"), "Lineage immutable");
        check(!sql(path,"INSERT INTO predictions SELECT * FROM predictions LIMIT 1;"), "Unique prediction step key");
        check(!sql(path,"INSERT INTO outcomes SELECT * FROM outcomes LIMIT 1;"), "Unique score revision key");
        const auto nextId = ledger.publishReplay(replay,4,Now,"Separate retained replay",id);
        check(nextId != id && ledger.lineages().size() == 2 && ledger.predictions(id).size() == 3, "Old immutable lineage inspectable");
    }
    {
        AdaptiveLedger restarted(root);
        check(encodeAdaptiveState(restarted.head()->state) == packed, "Restart checkpoint precision");
        check(restarted.predictions(id).front().outcomeRevision == revisionDigest(row(1)), "Restart preserves attachments");
    }
    check(bytes(sourcePath) == sourceBefore && bytes(historyPath) == historyBefore, "Source and simulation history byte preservation");
    check(sql(path,"PRAGMA user_version=999;"), "Unknown schema fixture"); const auto unknown = bytes(path);
    rejects([&] { AdaptiveLedger rejected(root); }, "Unknown schema rejected without repair");
    check(bytes(path) == unknown, "Unknown schema file preserved");
    check(sql(path,"PRAGMA user_version=1; DROP TRIGGER immutable_predictions_UPDATE; UPDATE predictions SET state=x'00';"), "Corrupt active checkpoint fixture");
    const auto corrupt = bytes(path);
    rejects([&] { AdaptiveLedger rejected(root); }, "Corrupt checkpoint rejected");
    check(bytes(path) == corrupt, "Corrupt ledger preserved");
}
void corrections(const std::filesystem::path& root) {
    const std::vector<Result> baseline{row(0),row(1),row(2),row(3)};
    for (int scenario = 0; scenario < 7; ++scenario) {
        auto original = baseline, revised = baseline;
        if (scenario == 0) revised[1].extra = 20;
        if (scenario == 1) original.erase(original.begin()+1);
        if (scenario == 2) revised.erase(revised.begin()+1);
        if (scenario == 3) revised[1].source.trust = Trust::Unverified;
        if (scenario == 4) revised[1].unresolvedConflict = true;
        if (scenario == 5) original[1].unresolvedConflict = true;
        AdaptiveLedger ledger(root/std::to_string(scenario));
        const auto initial = ledger.reconcile(Snapshot(original),1,Now);
        check(initial.current && initial.status == AdaptiveReconcileStatus::Current, "Initial reconciliation creates replay");
        const auto oldId = initial.current->lineage.id;
        const auto oldRecords = ledger.predictions(oldId);
        if (scenario == 6) {
            check(sql(ledger.path(),"INSERT INTO protocols VALUES('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','synthetic prior protocol fixture'); DROP TRIGGER immutable_lineages_UPDATE; UPDATE lineages SET protocol='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'; CREATE TRIGGER immutable_lineages_UPDATE BEFORE UPDATE ON lineages BEGIN SELECT RAISE(ABORT,'Immutable adaptive record'); END;"), "Construct isolated protocol-version fixture");
        }
        const Snapshot changed(revised); const auto expected = replayAdaptive(changed);
        const auto updated = ledger.reconcile(changed,2,"2026-09-26T00:00:00Z");
        check(updated.current && updated.current->lineage.id != oldId && ledger.lineages().size() == 2, "Revision creates distinct retained lineage");
        check(encodeAdaptiveState(updated.current->state) == encodeAdaptiveState(expected.state), "Revised lineage equals clean replay");
        check(updated.processed == expected.steps.size(), "Revision replay learning count");
        const auto preserved = ledger.predictions(oldId);
        check(preserved.size() == oldRecords.size(), "Old prediction count preserved");
        for (size_t i = 0; i < preserved.size(); ++i) {
            check(encodeAdaptiveState(preserved[i].before) == encodeAdaptiveState(oldRecords[i].before) &&
                  preserved[i].createdUtc == oldRecords[i].createdUtc && preserved[i].outcomeRevision == oldRecords[i].outcomeRevision, "Old prediction and score attachment identities immutable");
        }
        const auto again = ledger.reconcile(changed,2,Now);
        check(again.processed == 0 && again.current->lineage.id == updated.current->lineage.id, "Corrected history subsequently no-op");
    }
    AdaptiveLedger ledger(root/"race"); const Snapshot base(baseline);
    const auto first = ledger.reconcile(base,1,Now); auto corrected = baseline; corrected[1].extra = 20;
    auto appended = baseline; appended.push_back(row(4)); const Snapshot extension(appended);
    rejects([&] { ledger.reconcile(Snapshot(corrected),2,Now,{},[&](size_t) {
        AdaptiveLedger other(root/"race"); other.reconcile(extension,3,Now);
    }); }, "Concurrent same-lineage append invalidates replay expected-prefix");
    check(ledger.lineages().size() == 1 && ledger.head()->lineage.id == first.current->lineage.id && ledger.head()->state.draws == 5, "Racing replay cannot overwrite winning append");
    std::stop_source stop;
    rejects([&] { ledger.reconcile(Snapshot(corrected),4,Now,stop.get_token(),[&](size_t) { stop.request_stop(); }); }, "Cancelled revision never activates stale output");
    check(ledger.lineages().size() == 1 && ledger.head()->state.draws == 5, "Cancellation retains prior complete lineage");
}
void provenance(const std::filesystem::path& root) {
    // Coherent injected archive states exercise the trusted internal boundary.
    // Real UpdateWorker wiring is a separate task; these are not live HKJC draws.
    const auto archiveState = [](std::vector<Result> rows, uint64_t version, bool finished) {
        ArchiveState state{Snapshot(rows),{},{},{},version};
        for (const auto& r : rows) state.revisions.push_back({revisionDigest(r),r,true});
        UpdateAttempt job; job.id = 42; job.sourceId = "ledger-fixture"; job.baseVersion = 1;
        job.startedUtc = "2026-09-26T00:00:00Z"; job.status = finished ? "succeeded" : "pending";
        if (finished) job.finishedUtc = "2026-09-26T00:00:20Z";
        state.attempts.push_back(job); return state;
    };
    for (int scenario = 0; scenario < 7; ++scenario) {
        AdaptiveLedger ledger(root/std::to_string(scenario));
        auto before = archiveState({row(0)},1,false);
        if (scenario == 2) { auto old = row(1); old.source.trust = Trust::Unverified; before.revisions.push_back({revisionDigest(old),old,false}); }
        const auto initial = ledger.reconcile(before.snapshot,1,Now); const auto id = initial.current->lineage.id;
        const auto pending = encodeAdaptiveState(ledger.predictions(id).front().before);
        const auto boundary = ledger.prepareFetch(before,42,"2026-09-26T00:00:01Z"); check(boundary > 0, "Durable pre-network boundary");
        auto one = row(1), two = row(2);
        one.source.retrievedUtc = "2026-09-26T00:00:10Z"; two.source.retrievedUtc = one.source.retrievedUtc;
        if (scenario == 1) one.source.retrievedUtc = "2026-09-26T00:00:01.500Z"; // Same second does not qualify.
        auto after = archiveState({row(0),one,two},2,true);
        if (scenario == 3) { auto earlier = row(1); after.revisions.push_back({revisionDigest(earlier),earlier,false}); }
        if (scenario == 4) after.version = 3; // An intervening source transaction cannot be ignored.
        if (scenario == 5) after.attempts[0].status = "failed";
        if (scenario == 6) after.attempts[0].sourceId = "unrelated-source";
        const auto updated = ledger.reconcileFetched(after,boundary,"2026-09-26T00:00:30Z");
        check(updated.current && updated.processed == 2, "Fetched batch still reconciles");
        auto records = ledger.predictions(id);
        check(records[0].provenance == (scenario == 0 ? "locally-pre-fetch" : "historical-replay"), "Conservative first retrieval classification");
        check(records[1].provenance == "historical-replay" && records[2].provenance == "pending-unbound", "Only pre-existing pending forecast can qualify in batch");
        const auto checkpoint=encodeAdaptiveState(ledger.head()->state);const auto progress=readLearningProgress(ledger,id);
        check(progress.pending==1&&progress.evidence[1].points.size()==size_t(scenario==0)&&progress.evidence[0].points.size()==size_t(scenario==0?1:2),"Progress splits effective ledger provenance and excludes pending");
        check(encodeAdaptiveState(ledger.head()->state)==checkpoint,"Progress reading never updates checkpoint");
        check(encodeAdaptiveState(records[0].before) == pending, "Provenance never changes original forecast");
        if (scenario == 0) {
            check(records[0].provenanceDetail.find("Not certified before the real-world draw") != std::string::npos, "No pre-draw certification");
            const auto again = ledger.reconcileFetched(after,boundary,"2026-09-26T00:00:30Z");
            check(again.processed == 0 && ledger.predictions(id)[0].provenance == "locally-pre-fetch", "Idempotent provenance retry");
            check(!sql(ledger.path(),"DELETE FROM prefetch_annotations;"), "Immutable pre-fetch evidence");
            auto revised = after.snapshot.records(); revised[1].extra = 20;
            const auto correction = ledger.reconcile(Snapshot(revised),3,"2026-09-26T00:01:00Z");
            const auto changed = ledger.predictions(correction.current->lineage.id);
            check(changed[0].provenance == "historical-replay" && changed[0].outcomeRevision != records[0].outcomeRevision, "Correction scores belong to revised replay");
            check(ledger.predictions(id)[0].outcomeRevision == records[0].outcomeRevision && ledger.predictions(id)[0].provenance == "locally-pre-fetch", "Correction preserves original annotated score");
            AdaptiveLedger reopened(root/std::to_string(scenario));
            check(reopened.predictions(id)[0].provenance == "locally-pre-fetch", "Provenance survives restart");
        }
    }
    AdaptiveLedger uncertain(root/"uncertain"); auto before = archiveState({row(0)},1,false);
    const auto initial = uncertain.reconcile(before.snapshot,1,Now); const auto id = initial.current->lineage.id;
    const auto boundary = uncertain.prepareFetch(before,42,"2026-09-26T00:00:01Z");
    auto later = row(2); later.source.retrievedUtc = "2026-09-26T00:00:10Z";
    auto after = archiveState({row(0),later},2,true);
    const auto result = uncertain.reconcileFetched(after,boundary,"2026-09-26T00:00:30Z");
    check(result.current->lineage.id != id, "Missing next identity uses separate replay");
    const auto unbound = uncertain.predictions(id).front();
    check(!unbound.outcome && unbound.provenance == "pending-unbound" && unbound.provenanceDetail.find("unscored") != std::string::npos, "Ambiguous original pending forecast remains unscored with explanation");
}
}
size_t testAdaptiveLedger() {
    const auto root = std::filesystem::temp_directory_path()/("marksix-adaptive-ledger-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    storage(root);
    incremental(root/"incremental");
    corrections(root/"corrections");
    provenance(root/"provenance");
    std::cout << "PASS adaptive ledger/precision/immutability/rollback/isolation checks=" << checks << " fixtures=" << root.string() << '\n';
    return checks;
}
