#include "archive.hpp"
#include "import.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace marksix::statistics;
namespace {
size_t checks = 0;
void check(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F operation, const char* message) {
    bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; } check(rejected, message);
}
Result row(int n = 1) {
    Result r; r.id = "03/00" + std::to_string(n); r.date = "2003-01-0" + std::to_string(n);
    r.main = {1,2,3,4,5,6}; r.extra = 7;
    r.source = {"synthetic-test", "https://example.invalid/synthetic", "2026-09-25T00:00:00Z", sha256("synthetic"),
                "synthetic-not-real", Trust::OfficialRetrieval, "test-only-injected-evidence"};
    return r;
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary); if (!input) throw std::runtime_error("Cannot read fixture");
    return std::string(std::istreambuf_iterator<char>(input), {});
}
void mutateDatabase(const std::filesystem::path& path, const char* sql) {
    sqlite3* db = nullptr; const auto name = path.u8string();
    if (sqlite3_open(reinterpret_cast<const char*>(name.c_str()), &db) != SQLITE_OK) throw std::runtime_error("Cannot open test database");
    const int status = sqlite3_exec(db, sql, nullptr, nullptr, nullptr); sqlite3_close(db);
    if (status != SQLITE_OK) throw std::runtime_error("Cannot construct test database");
}
std::string manifest(const std::string& csv, const std::string& status = "unverified") {
    return "{\"schema\":\"marksix-csv-v1\",\"csvSha256\":\"" + sha256(csv) +
        "\",\"sourceId\":\"synthetic-test\",\"url\":\"https://example.invalid/fixture\","
        "\"retrievedUtc\":\"2026-09-25T00:00:00Z\",\"lineage\":\"synthetic-only\",\"status\":\"" + status + "\",\"extractionOrder\":\"unknown\"}";
}
void imports(const std::filesystem::path& root) {
    const std::string header = "draw_id,date,n1,n2,n3,n4,n5,n6,extra,era,provider_id,source_window\r\n";
    const std::string csv = header + "\"03/001\",2003-01-01,1,2,3,4,5,6,7,49-number,fixture,20030101-20030331\r\n";
    check(parseCsv("a,\"b,b\",\"c\"\"d\"\r\n")[0] == std::vector<std::string>{"a","b,b","c\"d"}, "Quoted CSV commas/escaping");
    check(parseCsv("a,\"b\nc\"\n")[0][1] == "b\nc", "Quoted embedded newline");
    check(parseCsv("a,b,")[0].size() == 3, "Trailing empty field");
    for (const auto* input : {"a,\"unterminated", "a,\"closed\"x", "a,ba\"d", "a,b\r"})
        rejects([&] { parseCsv(input); }, "Malformed quoting rejected");
    rejects([] { parseCsv(std::string("a,\0x", 4)); }, "Embedded NUL rejected");
    rejects([] { parseCsv("a,\xc0\xaf"); }, "Invalid UTF8 rejected");
    rejects([] { parseCsv(std::string(MaxImportBytes + 1, 'x')); }, "Oversized CSV rejected");
    rejects([] { parseCsv(std::string(2049, 'x')); }, "Oversized field rejected");
    rejects([] { parseManifest("{\"a\":\"b\",\"a\":\"c\"}"); }, "Duplicate JSON keys rejected");
    rejects([] { parseManifest("{\"a\":null}"); }, "Wrong manifest types rejected");
    rejects([] { parseManifest("[]"); }, "Array manifest rejected");
    for (const auto* status : {"official", "OfficialRetrieval", "corroborated"})
        rejects([&] { parseImport(csv, manifest(csv, status)); }, "Source status forgery rejected");
    rejects([&] { parseImport(csv + "\n", manifest(csv)); }, "Manifest digest mismatch rejected");
    auto rows = parseImport(csv, manifest(csv));
    check(rows.size() == 1 && rows[0].source.trust == Trust::Unverified && !rows[0].extractionOrder, "Import never manufactures trust/order");
    const std::string malformed = header + "03/001,2003-01-01,1,2,3,4,5,6,6,49-number,p,w\n";
    rejects([&] { parseImport(malformed, manifest(malformed)); }, "Malformed batch diagnosed");
    Archive archive(root);
    auto preview = previewImport(archive, csv, manifest(csv));
    check(archive.snapshot().records().empty() && preview.rows.size() == 1, "Preview is nonmutating");
    const auto accepted = acceptImport(archive, preview);
    check(accepted.inserted == 1 && archive.snapshot().select().empty(), "Unverified import browsable but excluded");
    rejects([&] { acceptImport(archive, preview); }, "Stale preview rejected transactionally");
    preview = previewImport(archive, csv, manifest(csv));
    check(preview.duplicates == 1 && acceptImport(archive, preview).inserted == 0, "Preview duplicates/idempotent accept");
    auto forged = preview; forged.rows[0].source.trust = Trust::OfficialRetrieval;
    rejects([&] { acceptImport(archive, forged); }, "Edited preview cannot promote trust");
    const std::string duplicate = csv + csv.substr(header.size());
    auto duplicatePreview = previewImport(archive, duplicate, manifest(duplicate));
    check(duplicatePreview.duplicates == 1, "In-batch duplicate counted");
    const std::string corrected = header + "03/001,2003-01-01,1,2,3,4,5,6,8,49-number,p,w\n";
    auto conflict = previewImport(archive, corrected, manifest(corrected));
    check(conflict.conflicts == 1 && acceptImport(archive, conflict).conflicts == 1, "Conflict preview equals acceptance");
    const auto before = archive.snapshot().digest();
    rejects([&] { previewImport(archive, malformed, manifest(malformed)); }, "Malformed preview cannot publish");
    check(archive.snapshot().digest() == before, "Bad preview preserves prior dataset");
    const std::string early = header + "00/001,2000-01-01,1,2,3,4,5,6,7,earlier-era-review-required,p,w\n";
    auto earlier = previewImport(archive, early, manifest(early));
    check(earlier.earlierQuarantined == 1, "Earlier era visibly quarantined");
    acceptImport(archive, earlier); check(archive.snapshot().select().empty(), "Earlier cannot contaminate model");
}
void updates(const std::filesystem::path& root) {
    const UpdatePolicy policy{"synthetic-test", "fixture-v1", {"2003-01-01", "2003-01-09"}, 3, 3};
    Archive archive(root);
    auto plan = [&] { return planUpdate(policy, archive.snapshot(), archive.committedWindows()); };
    auto begin = [&] { return archive.beginUpdate(plan(), archive.version()); };
    auto responses = [&](const UpdatePlan& p, std::vector<Result> rows) {
        std::vector<FetchedWindow> windows;
        for (const auto& range : p.requests) {
            FetchedWindow window{range, sha256("synthetic"), {}};
            for (const auto& r : rows) if (r.date >= range.from && r.date <= range.through) window.rows.push_back(r);
            windows.push_back(std::move(window));
        }
        return windows;
    };
    const auto initial = plan(); const auto id = begin();
    check(archive.committedWindows().empty() && archive.snapshot().records().empty(), "Attempt alone grants no coverage or rows");
    check(archive.updateAttempts().back().status == "pending" && !archive.updateAttempts().back().startedUtc.empty(), "Attempt timestamp distinct from success");
    const auto windows = responses(initial, {row(1), row(4)});
    const auto version = archive.version();
    // Fail at the second receipt: rows, dataset version and first receipt already staged.
    mutateDatabase(archive.path(), "CREATE TRIGGER fail_receipt BEFORE INSERT ON fetched_windows WHEN NEW.ordinal=1 BEGIN SELECT RAISE(ABORT,'receipt rollback fixture'); END");
    rejects([&] { archive.commitUpdate(id, windows); }, "Receipt failure rolls back whole publication");
    check(archive.revisions().empty() && archive.committedWindows().empty() && archive.version() == version, "No partial data/version/receipts");
    check(archive.updateAttempts().back().status == "pending" && archive.updateAttempts().back().finishedUtc.empty(), "No false success after rollback");
    mutateDatabase(archive.path(), "DROP TRIGGER fail_receipt");
    check(plan().requests == initial.requests, "Interrupted backfill leaves all windows eligible");
    auto missing = windows; missing.pop_back();
    rejects([&] { archive.commitUpdate(id, missing); }, "Missing even an empty window rejected");
    auto extra = windows; extra.push_back(windows.back());
    rejects([&] { archive.commitUpdate(id, extra); }, "Extra response window rejected");
    auto reversed = windows; std::reverse(reversed.begin(), reversed.end());
    rejects([&] { archive.commitUpdate(id, reversed); }, "Reordered responses rejected");
    auto wrong = windows; wrong[0].range.through = "2003-01-02";
    rejects([&] { archive.commitUpdate(id, wrong); }, "Response range mismatch rejected");
    wrong = windows; wrong[0].responseSha256 = "bad";
    rejects([&] { archive.commitUpdate(id, wrong); }, "Bad response digest rejected");
    wrong = windows; wrong[0].rows[0].source.sourceId = "other";
    rejects([&] { archive.commitUpdate(id, wrong); }, "Wrong source rejected");
    wrong = windows; wrong[0].rows[0].source.contentSha256 = sha256("other response");
    rejects([&] { archive.commitUpdate(id, wrong); }, "Wrong response provenance rejected");
    wrong = windows; wrong[0].rows.push_back(row(4));
    rejects([&] { archive.commitUpdate(id, wrong); }, "Out of window result rejected");
    wrong = windows; wrong[0].rows.push_back(row(1));
    rejects([&] { archive.commitUpdate(id, wrong); }, "Duplicate fetched draw rejected");
    wrong = windows; wrong[0].rows[0].extra = 1;
    rejects([&] { archive.commitUpdate(id, wrong); }, "Malformed response row rejected");
    wrong = windows; wrong[0].rows[0].source.trust = Trust::Unverified;
    rejects([&] { archive.commitUpdate(id, wrong); }, "Unverified input cannot grant fetched coverage");
    std::stop_source stop; stop.request_stop();
    rejects([&] { archive.commitUpdate(id, windows, stop.get_token()); }, "Precommit cancellation rejected");
    check(archive.revisions().empty() && archive.committedWindows().empty() && archive.version() == version, "All invalid responses preserve accepted state");
    const auto outcome = archive.commitUpdate(id, windows);
    check(outcome.comparison.added == 2 && outcome.ingestion.inserted == 2, "New results published with accurate counts");
    check(archive.committedWindows().size() == 3, "Valid empty window gets receipt too");
    const auto attempts = archive.updateAttempts();
    check(attempts.back().status == "succeeded" && !attempts.back().finishedUtc.empty() && attempts.back().added == 2, "Successful attempt time and counts durable");
    check(!archive.finishUpdate(id, true, "late cancellation"), "Cancellation after commit cannot claim rollback");
    rejects([&] { archive.commitUpdate(id, windows); }, "Completed attempt cannot recommit");
    const auto next = plan();
    check(next.requests == std::vector<DateRange>{{"2003-01-07", "2003-01-09"}}, "Successful coverage prevents routine full backfill");
    // Explicit same-range plan checks timestamp-only and content-hash-only differences.
    auto overlap = initial; overlap.snapshotDigest = archive.snapshot().digest();
    auto newer = windows;
    for (auto& window : newer) {
        window.responseSha256 = sha256("new response identical outcomes");
        for (auto& r : window.rows) { r.source.contentSha256 = window.responseSha256; r.source.retrievedUtc = "2026-09-26T00:00:00Z"; }
    }
    const auto beforeVersion = archive.version(); const auto beforeDigest = archive.snapshot().digest();
    const auto sameId = archive.beginUpdate(overlap, beforeVersion);
    const auto noChange = archive.commitUpdate(sameId, newer);
    check(noChange.comparison.unchanged == 2 && noChange.ingestion.inserted == 0, "Retrieval-only differences are no-change checks");
    check(archive.version() == beforeVersion && archive.snapshot().digest() == beforeDigest && archive.revisions().size() == 2, "No-change does not manufacture revisions or cache invalidation");
    check(archive.updateAttempts().back().unchanged == 2 && archive.committedWindows().size() == 6, "No-change still records successful check receipts");
    {
        Archive restart(root);
        check(restart.updateAttempts().back().id == sameId && restart.committedWindows().size() == 6 && restart.snapshot().digest() == beforeDigest, "Restart restores receipts and attempts independently from draw dates");
    }
    const auto failed = begin(); const auto receiptCount = archive.committedWindows().size();
    check(archive.finishUpdate(failed, false, "synthetic schema failure"), "Failure recorded separately");
    check(archive.updateAttempts().back().status == "failed" && archive.committedWindows().size() == receiptCount, "Failure cannot advance receipts");
    rejects([&] { archive.commitUpdate(failed, responses(next, {})); }, "Failed attempt cannot commit");
    const auto cancelled = begin(); check(archive.finishUpdate(cancelled, true, "user cancellation"), "Cancellation recorded");
    check(archive.updateAttempts().back().status == "cancelled" && !archive.finishUpdate(cancelled, false, "late failure"), "Terminal status immutable through API");
    rejects([&] { archive.finishUpdate(-1, true, "missing attempt"); }, "Unknown attempt rejected");
    const auto obsolete = begin(); const auto current = begin();
    rejects([&] { archive.commitUpdate(obsolete, responses(next, {})); }, "New attempt supersedes older worker even without dataset changes");
    archive.commitUpdate(current, responses(next, {}));
    check(archive.version() == beforeVersion, "Successful empty response leaves dataset unchanged");
    const auto stale = begin();
    { Archive other(root); other.ingest({row(8)}); }
    rejects([&] { archive.commitUpdate(stale, responses(next, {})); }, "Another connection's import invalidates staged update");
    rejects([&] { archive.beginUpdate(overlap, beforeVersion); }, "Stale plan rejected before attempt creation");
    auto badPlan = plan(); badPlan.requests.push_back(badPlan.requests.front());
    rejects([&] { archive.beginUpdate(badPlan, archive.version()); }, "Overlapping plan rejected");
    badPlan = plan(); badPlan.snapshotDigest = sha256("not the dataset");
    rejects([&] { archive.beginUpdate(badPlan, archive.version()); }, "Mismatched plan snapshot rejected");
    rejects([&] { archive.beginUpdate(plan(), archive.version(), stop.get_token()); }, "Cancelled planning publishes no attempt");
    // Correction is retained and excluded, never silently selected.
    auto correction = row(8); correction.extra = 9; const auto correctionPlan = plan();
    const auto corrected = archive.commitUpdate(begin(), responses(correctionPlan, {correction}));
    check(corrected.comparison.conflicts == 1 && corrected.ingestion.conflicts == 1 && archive.snapshot().select().size() == 2, "Correction retained under conflict policy");
    const auto correctedVersion = archive.version(); const auto revisionCount = archive.revisions().size();
    correction.source.retrievedUtc = "2026-09-27T00:00:00Z";
    const auto repeated = archive.commitUpdate(begin(), responses(plan(), {correction}));
    check(repeated.comparison.conflicts == 1 && repeated.ingestion.inserted == 0 && archive.version() == correctedVersion && archive.revisions().size() == revisionCount, "Repeated known conflict is not a new revision or auto-resolution");
    auto imported = row(9); imported.source.trust = Trust::Unverified; archive.ingest({imported});
    const auto verified = archive.commitUpdate(begin(), responses(plan(), {row(9)}));
    check(verified.comparison.needsVerification == 1 && archive.snapshot().select().size() == 3, "Matching imported draw requires and receives actual provider verification");
    // Failure at final success marker rolls back all receipts and accepted changes.
    const auto finalPlan = plan(); const auto finalId = begin(); const auto finalDigest = archive.snapshot().digest();
    const auto finalVersion = archive.version(); const auto finalReceipts = archive.committedWindows().size();
    mutateDatabase(archive.path(), "CREATE TRIGGER fail_success BEFORE UPDATE ON update_attempts WHEN NEW.status='succeeded' BEGIN SELECT RAISE(ABORT,'final publication failure'); END");
    rejects([&] { archive.commitUpdate(finalId, responses(finalPlan, {row(7)})); }, "Failure at final success metadata rolls back");
    check(archive.snapshot().digest() == finalDigest && archive.version() == finalVersion && archive.committedWindows().size() == finalReceipts && archive.updateAttempts().back().status == "pending", "Final failure preserves all accepted state");
    mutateDatabase(archive.path(), "DROP TRIGGER fail_success");
    archive.finishUpdate(finalId, false, "synthetic save failure");
    std::cout << "PASS durable update receipts/atomic publication/stale attempts (synthetic only)\n";
}
void updateMigration(const std::filesystem::path& root) {
    std::string digest; uint64_t version = 0;
    { Archive original(root); original.ingest({row(1), row(2)}); digest = original.snapshot().digest(); version = original.version(); }
    const auto db = root / "statistics/results.sqlite3";
    mutateDatabase(db, "DROP TABLE fetched_windows; DROP TABLE update_requests; DROP TABLE update_attempts; PRAGMA user_version=2");
    {
        Archive migrated(root);
        check(migrated.snapshot().digest() == digest && migrated.version() == version, "Schema 2 migration preserves dataset identity/version");
        check(migrated.committedWindows().empty() && migrated.updateAttempts().empty(), "Legacy verified rows do not manufacture receipts or update times");
    }
    size_t backups = 0;
    for (const auto& entry : std::filesystem::directory_iterator(db.parent_path())) {
        if (entry.path().filename().string().find(".before-v3-") == std::string::npos) continue;
        ++backups; const auto recovery = root / "recovered-v2";
        std::filesystem::create_directories(recovery / "statistics");
        std::filesystem::copy_file(entry.path(), recovery / "statistics/results.sqlite3");
        Archive recovered(recovery);
        check(recovered.snapshot().digest() == digest && recovered.version() == version, "Pre-v3 backup recovers schema 2 dataset");
    }
    check(backups == 1, "Schema 2 migration creates one recoverable backup");
    { Archive reopened(root); check(reopened.committedWindows().empty(), "Schema 3 restart needs no migration"); }
    size_t afterRestart = 0;
    for (const auto& entry : std::filesystem::directory_iterator(db.parent_path()))
        if (entry.path().filename().string().find(".before-v3-") != std::string::npos) ++afterRestart;
    check(afterRestart == backups, "No repeated backups on normal restart");
    // Preserve a conflicting table in an invalid old schema; do not overwrite it to force upgrade.
    mutateDatabase(db, "DROP TABLE fetched_windows; DROP TABLE update_requests; PRAGMA user_version=2");
    const auto before = bytes(db);
    rejects([&] { Archive broken(root); }, "Inconsistent schema 2 migration rejected");
    check(bytes(db) == before, "Failed schema 2 upgrade byte-preserves original");
}
// SQLite's test-process-only hooks deliver cancellation at deterministic write
// boundaries. No production hook, sleep-based race or extra Archive API is needed.
struct CancellationProbe { std::stop_source stop; std::string table; bool duringCommit = false; size_t hits = 0; };
CancellationProbe* openingProbe = nullptr;
int cancellationExtension(sqlite3* db, char**, const sqlite3_api_routines*) {
    sqlite3_update_hook(db, [](void* context, int, const char*, const char* table, sqlite3_int64) {
        auto& probe = *static_cast<CancellationProbe*>(context);
        if (probe.table == table) { ++probe.hits; probe.stop.request_stop(); }
    }, openingProbe);
    sqlite3_commit_hook(db, [](void* context) {
        auto& probe = *static_cast<CancellationProbe*>(context);
        if (probe.duringCommit) { ++probe.hits; probe.stop.request_stop(); }
        return 0; // Let the actual commit win this race.
    }, openingProbe);
    return SQLITE_OK;
}
struct CancellationRegistration {
    explicit CancellationRegistration(CancellationProbe& probe) {
        openingProbe = &probe;
        if (sqlite3_auto_extension(reinterpret_cast<void(*)()>(cancellationExtension)) != SQLITE_OK)
            throw std::runtime_error("Cannot register test cancellation hook");
    }
    ~CancellationRegistration() { sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(cancellationExtension)); openingProbe = nullptr; }
};
void updateCancellation(const std::filesystem::path& root) {
    CancellationProbe probe; CancellationRegistration registration(probe); Archive archive(root);
    const UpdatePolicy policy{"synthetic-test", "fixture-v1", {"2003-01-01", "2003-01-03"}, 3, 3};
    const auto plan = planUpdate(policy, archive.snapshot(), {});
    const auto version = archive.version();
    const std::vector<FetchedWindow> windows{{plan.requests[0], sha256("synthetic"), {row(1)}}};
    for (const auto* table : {"revisions", "fetched_windows", "update_attempts"}) {
        probe.stop = std::stop_source{}; probe.table.clear(); probe.hits = 0;
        const auto attempt = archive.beginUpdate(plan, version); probe.table = table;
        rejects([&] { archive.commitUpdate(attempt, windows, probe.stop.get_token()); }, "In-transaction cancellation rolls back");
        check(probe.hits > 0 && probe.stop.stop_requested(), "Cancellation reached requested write boundary");
        check(archive.revisions().empty() && archive.committedWindows().empty() && archive.version() == version && archive.updateAttempts().back().status == "pending",
              "Cancellation after staged rows/receipts/success marker preserves all accepted state");
        probe.table.clear(); archive.finishUpdate(attempt, true, "deterministic cancellation fixture");
    }
    probe.stop = std::stop_source{}; probe.hits = 0;
    const auto attempt = archive.beginUpdate(plan, version); probe.duringCommit = true;
    const auto outcome = archive.commitUpdate(attempt, windows, probe.stop.get_token()); probe.duringCommit = false;
    check(probe.hits == 1 && probe.stop.stop_requested() && outcome.ingestion.inserted == 1, "Commit-winning cancellation race returns committed outcome");
    check(!archive.finishUpdate(attempt, true, "late cancellation") && archive.updateAttempts().back().status == "succeeded" && archive.committedWindows().size() == 1,
          "Late cancellation leaves truthful successful metadata and receipt");
    std::cout << "PASS deterministic cancellation at row/receipt/success/commit boundaries\n";
}
void resolutionCancellation(const std::filesystem::path& root) {
    CancellationProbe probe; CancellationRegistration registration(probe); Archive archive(root);
    auto correction = row(); correction.extra = 8; archive.ingest({row(), correction});
    const auto version = archive.version(); const auto digest = revisionDigest(correction);
    const auto before = archive.snapshot().digest();
    rejects([&] { archive.resolve(drawKey(correction), digest, "synthetic decision", "fixture evidence", {}, version - 1); }, "Stale conflict inspection rejected inside transaction");
    for (const auto* table : {"decisions", "draws", "dataset_versions"}) {
        probe.stop = std::stop_source{}; probe.table = table; probe.hits = 0;
        rejects([&] { archive.resolve(drawKey(correction), digest, "synthetic decision", "fixture evidence", probe.stop.get_token(), version); }, "Midtransaction resolution cancellation rolls back");
        check(probe.hits > 0, "Resolution cancellation reached requested write boundary");
        check(archive.version() == version && archive.resolutions().empty() && archive.snapshot().digest() == before,
              "Cancelled resolution preserves conflict, active revision, audit and version");
    }
    probe.table.clear(); probe.stop = std::stop_source{}; probe.stop.request_stop();
    rejects([&] { archive.resolve(drawKey(correction), digest, "synthetic decision", "fixture evidence", probe.stop.get_token(), version); }, "Precancelled resolution rejected");
    probe.stop = std::stop_source{}; probe.duringCommit = true; probe.hits = 0;
    archive.resolve(drawKey(correction), digest, "synthetic decision", "fixture evidence", probe.stop.get_token(), version);
    probe.duringCommit = false;
    const auto state = archive.readState();
    check(probe.hits == 1 && probe.stop.stop_requested() && state.resolutions.size() == 1 && state.version == version + 1,
          "Commit-winning resolution retains decision and coherent view version");
    check(state.snapshot.records()[0].extra == 8 && !state.snapshot.records()[0].unresolvedConflict && state.revisions.size() == 2,
          "Commit-winning resolution keeps both revisions and selected active result");
    std::cout << "PASS deterministic conflict-resolution stale/cancellation/commit checks\n";
}
}
size_t testArchive() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("marksix-statistics-test-" + unique);
    check(std::filesystem::create_directory(root), "Fresh isolated fixture root");
    const auto sentinel = root / "preserved.msx";
    // Inert preservation sentinel, not claimed to be a playable recording.
    { std::ofstream file(sentinel, std::ios::binary); file << "synthetic simulation-history preservation sentinel"; }
    const auto original = bytes(sentinel); std::string acceptedDigest;
    {
        Archive archive(root); check(archive.snapshot().records().empty(), "New independent empty archive");
        const auto inserted = archive.ingest({row(1),row(2),row(3)});
        check(inserted.inserted == 3 && archive.snapshot().select().size() == 3, "Transaction accepted");
        acceptedDigest = archive.snapshot().digest(); const auto version = archive.version();
        const auto duplicate = archive.ingest({row(1),row(2),row(3)});
        check(duplicate.inserted == 0 && duplicate.duplicate == 3 && archive.version() == version, "Idempotent import");
        auto invalid = row(5); invalid.extra = 1;
        rejects([&] { archive.ingest({row(4),invalid}); }, "Malformed batch rejects atomically");
        check(archive.snapshot().digest() == acceptedDigest, "Malformed batch preserved snapshot");
        // Mid-transaction failure after the first new row, without a production failpoint.
        mutateDatabase(archive.path(), "CREATE TRIGGER test_abort BEFORE INSERT ON revisions WHEN NEW.published_id='03/005' BEGIN SELECT RAISE(ABORT,'test rollback'); END");
        rejects([&] { archive.ingest({row(4),row(5)}); }, "Midtransaction rollback");
        check(archive.revisions().size() == 3 && archive.version() == version, "No partial revisions/version");
        mutateDatabase(archive.path(), "DROP TRIGGER test_abort");
        auto correction = row(); correction.extra = 8;
        const auto conflict = archive.ingest({correction});
        check(conflict.conflicts == 1 && archive.snapshot().select().size() == 2, "Conflict quarantines draw");
        check(archive.revisions().size() == 4 && archive.snapshot().digest() != acceptedDigest, "Conflict retained and cache invalidated");
        rejects([&] { archive.resolve(drawKey(correction), revisionDigest(correction), "", "evidence"); }, "Empty resolution rejected");
        archive.resolve(drawKey(correction), revisionDigest(correction), "Synthetic source correction", "synthetic-reference-only");
        check(archive.snapshot().select().size() == 3 && archive.snapshot().records()[0].extra == 8, "Explicit correction");
        check(archive.revisions().size() == 4 && archive.resolutions().size() == 1, "Old revision and decision retained");
        check(archive.version() > version, "Correction version changed");
        auto mirror = correction; mirror.source.sourceId = "synthetic-shared-mirror"; mirror.source.trust = Trust::Unverified;
        archive.ingest({mirror});
        check(archive.snapshot().records()[0].source.trust == Trust::OfficialRetrieval, "Unverified mirror cannot downgrade/upgrade trust");
        std::stop_source stop; stop.request_stop(); const auto before = archive.snapshot().digest();
        rejects([&] { archive.ingest({row(4)}, stop.get_token()); }, "Cancelled import");
        check(archive.snapshot().digest() == before, "Cancelled import preserved"); acceptedDigest = before;
    }
    { Archive reopened(root); check(reopened.snapshot().digest() == acceptedDigest && reopened.resolutions().size() == 1, "Restart persistence"); }
    check(bytes(sentinel) == original, "Simulation bytes untouched");
    const auto db = root / "statistics/results.sqlite3";
    mutateDatabase(db, "DROP TABLE fetched_windows; DROP TABLE update_requests; DROP TABLE update_attempts; DROP TABLE dataset_versions; PRAGMA user_version=1");
    { Archive migrated(root); check(migrated.snapshot().digest() == acceptedDigest, "Migration preserves data"); }
    bool backupFound = false;
    for (const auto& entry : std::filesystem::directory_iterator(db.parent_path())) if (entry.path().filename().string().find(".before-v3-") != std::string::npos) {
        // SQLite backup preserves database contents, not header change counters.
        // Recover a separate copy and verify all active rows/decisions survive.
        const auto recovery = root / "recovery";
        std::filesystem::create_directories(recovery / "statistics");
        std::filesystem::copy_file(entry.path(), recovery / "statistics/results.sqlite3");
        Archive recovered(recovery);
        check(recovered.snapshot().digest() == acceptedDigest && recovered.resolutions().size() == 1,
              "Backup actually recovers source data and decisions"); backupFound = true;
    }
    check(backupFound, "Migration backup exists");
    mutateDatabase(db, "PRAGMA user_version=99"); const auto future = bytes(db);
    rejects([&] { Archive unsupported(root); }, "Future schema rejected"); check(bytes(db) == future, "Future schema unchanged");
    const auto brokenRoot = root / "failed-upgrade";
    { Archive initial(brokenRoot); initial.ingest({row()}); }
    const auto broken = brokenRoot / "statistics/results.sqlite3";
    mutateDatabase(broken, "DROP TABLE fetched_windows; DROP TABLE update_requests; DROP TABLE update_attempts; DROP TABLE dataset_versions; DROP TABLE sources; PRAGMA user_version=1");
    const auto beforeFailure = bytes(broken);
    rejects([&] { Archive invalid(brokenRoot); }, "Broken migration rejects");
    check(bytes(broken) == beforeFailure, "Migration failure rollback preserves original");
    check(bytes(sentinel) == original, "Migration cannot touch simulation files");
    imports(root / "imports");
    updates(root / "updates");
    updateMigration(root / "update-migration");
    updateCancellation(root / "update-cancellation");
    resolutionCancellation(root / "resolution-cancellation");
    std::cout << "PASS archive transactions/revisions/migrations; retained fixtures: " << root.string() << '\n';
    return checks;
}
