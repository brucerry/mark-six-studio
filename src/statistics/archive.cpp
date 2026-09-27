#include "archive.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <stdexcept>

namespace marksix::statistics {
namespace {
constexpr int ApplicationId = 0x4d365354, Schema = 3;
void sqlCheck(int code, sqlite3* db) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
        throw std::runtime_error(std::string("Statistics database: ") + sqlite3_errmsg(db));
}
void exec(sqlite3* db, const char* sql) { sqlCheck(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), db); }
struct Statement {
    sqlite3* db; sqlite3_stmt* value = nullptr;
    Statement(sqlite3* connection, const char* sql) : db(connection) {
        sqlCheck(sqlite3_prepare_v2(db, sql, -1, &value, nullptr), db);
    }
    ~Statement() { sqlite3_finalize(value); }
    Statement(const Statement&) = delete;
    void bind(int at, const std::string& s) { sqlCheck(sqlite3_bind_text(value, at, s.data(), int(s.size()), SQLITE_TRANSIENT), db); }
    void bind(int at, int n) { sqlCheck(sqlite3_bind_int(value, at, n), db); }
    void bind(int at, int64_t n) { sqlCheck(sqlite3_bind_int64(value, at, n), db); }
    bool step() { const int rc = sqlite3_step(value); sqlCheck(rc, db); return rc == SQLITE_ROW; }
    std::string text(int col) const {
        const auto* s = sqlite3_column_text(value, col);
        return s ? std::string(reinterpret_cast<const char*>(s), size_t(sqlite3_column_bytes(value, col))) : std::string{};
    }
    int integer(int col) const { return sqlite3_column_int(value, col); }
    int64_t integer64(int col) const { return sqlite3_column_int64(value, col); }
};
struct Transaction {
    sqlite3* db; bool committed = false;
    explicit Transaction(sqlite3* connection, bool readOnly = false) : db(connection) { exec(db, readOnly ? "BEGIN" : "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { exec(db, "COMMIT"); committed = true; }
};
int scalar(sqlite3* db, const char* sql) { Statement s(db, sql); if (!s.step()) throw std::runtime_error("Missing database metadata"); return s.integer(0); }
std::string utf8(const std::filesystem::path& path) {
    const auto u = path.u8string(); return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
void backup(sqlite3* db, const std::filesystem::path& path) {
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), ULONG(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Cannot identify migration backup");
    const std::string id = sha256(std::string_view(reinterpret_cast<const char*>(random.data()), random.size())).substr(0, 24);
    auto destination = path; destination += ".before-v3-" + id + ".sqlite3";
    const HANDLE reserved = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reserved == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot reserve migration backup");
    CloseHandle(reserved);
    sqlite3* copy = nullptr; int status = sqlite3_open_v2(utf8(destination).c_str(), &copy, SQLITE_OPEN_READWRITE, nullptr);
    if (status != SQLITE_OK) { if (copy) sqlite3_close(copy); throw std::runtime_error("Cannot open migration backup"); }
    auto* task = sqlite3_backup_init(copy, "main", db, "main");
    if (!task) { sqlite3_close(copy); throw std::runtime_error("Cannot start migration backup"); }
    status = sqlite3_backup_step(task, -1); const int finished = sqlite3_backup_finish(task);
    sqlite3_close(copy);
    if (status != SQLITE_DONE || finished != SQLITE_OK) throw std::runtime_error("Migration backup failed; original unchanged");
}
const char* BaseSchema = R"sql(
CREATE TABLE sources(source_key TEXT PRIMARY KEY, source_id TEXT NOT NULL, url TEXT NOT NULL,
 lineage TEXT NOT NULL, trust INTEGER NOT NULL, evidence TEXT NOT NULL);
CREATE TABLE eras(name TEXT PRIMARY KEY, reviewed INTEGER NOT NULL, first_date TEXT NOT NULL, evidence TEXT NOT NULL);
INSERT INTO eras VALUES('current49',1,'2002-07-04','https://bet.hkjc.com/en/marksix/results');
INSERT INTO eras VALUES('earlier-unreviewed',0,'','Exact boundaries pending; quarantine');
CREATE TABLE revisions(digest TEXT PRIMARY KEY, draw_key TEXT NOT NULL, published_id TEXT NOT NULL,
 draw_date TEXT NOT NULL, era INTEGER NOT NULL, n1 INTEGER NOT NULL,n2 INTEGER NOT NULL,n3 INTEGER NOT NULL,
 n4 INTEGER NOT NULL,n5 INTEGER NOT NULL,n6 INTEGER NOT NULL,extra INTEGER NOT NULL,
 source_key TEXT NOT NULL REFERENCES sources(source_key), retrieved_utc TEXT NOT NULL,
 content_hash TEXT NOT NULL, extraction_order TEXT NOT NULL, order_evidence TEXT NOT NULL);
CREATE TABLE draws(draw_key TEXT PRIMARY KEY, active_digest TEXT NOT NULL REFERENCES revisions(digest),
 conflict INTEGER NOT NULL CHECK(conflict IN (0,1)));
CREATE TABLE import_batches(digest TEXT PRIMARY KEY, accepted_utc TEXT NOT NULL, rows INTEGER NOT NULL);
CREATE TABLE decisions(sequence INTEGER PRIMARY KEY, draw_key TEXT NOT NULL,
 selected_digest TEXT NOT NULL REFERENCES revisions(digest), reason TEXT NOT NULL, evidence TEXT NOT NULL, decided_utc TEXT NOT NULL);
CREATE TRIGGER immutable_revisions_update BEFORE UPDATE ON revisions BEGIN SELECT RAISE(ABORT,'Immutable revision'); END;
CREATE TRIGGER immutable_revisions_delete BEFORE DELETE ON revisions BEGIN SELECT RAISE(ABORT,'Immutable revision'); END;
)sql";
const char* UpdateSchema = R"sql(
CREATE TABLE update_attempts(id INTEGER PRIMARY KEY, source_id TEXT NOT NULL, policy_version TEXT NOT NULL,
 base_version INTEGER NOT NULL, snapshot_digest TEXT NOT NULL, started_utc TEXT NOT NULL,
 finished_utc TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'pending'
 CHECK(status IN ('pending','succeeded','failed','cancelled')), detail TEXT NOT NULL DEFAULT '',
 added INTEGER NOT NULL DEFAULT 0, unchanged INTEGER NOT NULL DEFAULT 0,
 needs_verification INTEGER NOT NULL DEFAULT 0, conflicts INTEGER NOT NULL DEFAULT 0);
CREATE INDEX update_attempt_source ON update_attempts(source_id,id);
CREATE TABLE update_requests(attempt_id INTEGER NOT NULL REFERENCES update_attempts(id),
 ordinal INTEGER NOT NULL, date_from TEXT NOT NULL, date_through TEXT NOT NULL,
 PRIMARY KEY(attempt_id,ordinal));
CREATE TABLE fetched_windows(attempt_id INTEGER NOT NULL, ordinal INTEGER NOT NULL,
 response_sha256 TEXT NOT NULL, PRIMARY KEY(attempt_id,ordinal),
 FOREIGN KEY(attempt_id,ordinal) REFERENCES update_requests(attempt_id,ordinal));
CREATE TRIGGER immutable_windows_update BEFORE UPDATE ON fetched_windows BEGIN SELECT RAISE(ABORT,'Immutable receipt'); END;
CREATE TRIGGER immutable_windows_delete BEFORE DELETE ON fetched_windows BEGIN SELECT RAISE(ABORT,'Immutable receipt'); END;
)sql";
const char* RevisionQuery = R"sql(
SELECT r.digest,r.published_id,r.draw_date,r.era,r.n1,r.n2,r.n3,r.n4,r.n5,r.n6,r.extra,
 s.source_id,s.url,r.retrieved_utc,r.content_hash,s.lineage,s.trust,s.evidence,r.extraction_order,r.order_evidence,
 d.conflict,d.active_digest=r.digest
FROM revisions r JOIN sources s ON s.source_key=r.source_key JOIN draws d ON d.draw_key=r.draw_key
)sql";
Result readResult(const Statement& s) {
    Result r; r.id = s.text(1); r.date = s.text(2); r.era = static_cast<Era>(s.integer(3));
    for (int n = 0; n < 6; ++n) r.main[n] = s.integer(4 + n); r.extra = s.integer(10);
    r.source = {s.text(11), s.text(12), s.text(13), s.text(14), s.text(15), static_cast<Trust>(s.integer(16)), s.text(17)};
    const auto order = s.text(18);
    if (!order.empty()) {
        std::istringstream input(order); std::array<int, 6> numbers{};
        for (int& n : numbers) if (!(input >> n)) throw std::runtime_error("Corrupt stored order");
        std::string remainder; if (input >> remainder) throw std::runtime_error("Corrupt stored order tail");
        r.extractionOrder = numbers;
    }
    r.orderEvidence = s.text(19); r = canonicalize(std::move(r));
    if (revisionDigest(r) != s.text(0)) throw std::runtime_error("Stored revision digest mismatch");
    r.unresolvedConflict = s.integer(20) != 0;
    return r;
}
bool sameOutcome(const Result& a, const Result& b) {
    return a.id == b.id && a.date == b.date && a.era == b.era && a.main == b.main && a.extra == b.extra &&
        (!a.extractionOrder || !b.extractionOrder || a.extractionOrder == b.extractionOrder);
}
int trustRank(Trust trust) { return trust == Trust::OfficialRetrieval ? 2 : trust == Trust::CorroboratedPublic ? 1 : 0; }
void inert(const std::string& text) {
    if (text.empty() || text.size() > 2048 || std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::invalid_argument("Resolution requires bounded nonempty reason and evidence");
}
void updateCheck(bool condition, const char* message) { if (!condition) throw std::invalid_argument(message); }
void updateCancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Statistics update cancelled before commit");
}
bool digestValid(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
void updateRange(const DateRange& range) {
    using namespace std::chrono;
    updateCheck(validDate(range.from) && validDate(range.through) && range.from <= range.through, "Invalid update window");
    const auto day = [](const std::string& text) {
        return sys_days{year{std::stoi(text.substr(0,4))}/unsigned(std::stoi(text.substr(5,2)))/unsigned(std::stoi(text.substr(8,2)))};
    };
    updateCheck((day(range.through) - day(range.from)).count() < 366, "Update window exceeds safety bound");
}
}
std::string drawKey(const Result& r) { return r.date.substr(0, 4) + ":" + r.id; }
Archive::Archive(const std::filesystem::path& root) : path_(root / "statistics" / "results.sqlite3") {
    std::filesystem::create_directories(path_.parent_path());
    try {
        sqlCheck(sqlite3_open_v2(utf8(path_).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr), db_);
        sqlite3_busy_timeout(db_, 5000);
        sqlite3_db_config(db_, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        sqlite3_db_config(db_, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
        sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, 16 * 1024 * 1024);
        exec(db_, "PRAGMA foreign_keys=ON");
        const int version = scalar(db_, "PRAGMA user_version"), app = scalar(db_, "PRAGMA application_id");
        if (version < 0 || version > Schema || (version && app != ApplicationId) || (!version && app && app != ApplicationId))
            throw std::runtime_error("Unsupported statistics database schema/application; file retained unchanged");
        if (!version) {
            if (scalar(db_, "SELECT count(*) FROM sqlite_master WHERE type='table'") != 0)
                throw std::runtime_error("Unrecognized nonempty database; file retained unchanged");
            Transaction tx(db_); exec(db_, BaseSchema);
            exec(db_, "CREATE TABLE dataset_versions(version INTEGER PRIMARY KEY, digest TEXT NOT NULL, created_utc TEXT NOT NULL)");
            const auto pragma = "PRAGMA application_id=" + std::to_string(ApplicationId); exec(db_, pragma.c_str());
            exec(db_, UpdateSchema);
            exec(db_, "PRAGMA user_version=3"); versionChanged(); tx.commit();
        } else if (version < Schema) {
            backup(db_, path_); Transaction tx(db_);
            if (version == 1) {
                exec(db_, "CREATE TABLE dataset_versions(version INTEGER PRIMARY KEY, digest TEXT NOT NULL, created_utc TEXT NOT NULL)");
                versionChanged();
            }
            exec(db_, UpdateSchema); snapshot();
            exec(db_, "PRAGMA user_version=3"); tx.commit();
        }
        // Read-only validation on open catches unsupported/corrupt rows before publication.
        snapshot();
    } catch (...) { if (db_) sqlite3_close(db_); db_ = nullptr; throw; }
}
Archive::~Archive() { if (db_) sqlite3_close(db_); }
ArchiveState Archive::readState() const {
    Transaction tx(db_, true); ArchiveState state{snapshot(), revisions(), updateAttempts(), resolutions(), version()}; tx.commit(); return state;
}
std::vector<StoredRevision> Archive::revisions() const {
    Statement query(db_, (std::string(RevisionQuery) + " ORDER BY r.draw_date,r.published_id,r.digest").c_str());
    std::vector<StoredRevision> out;
    while (query.step()) {
        if (out.size() >= 200000) throw std::runtime_error("Archive revision limit exceeded");
        out.push_back({query.text(0), readResult(query), query.integer(21) != 0});
    }
    return out;
}
Snapshot Archive::snapshot() const {
    Statement query(db_, (std::string(RevisionQuery) + " WHERE d.active_digest=r.digest ORDER BY r.draw_date,r.published_id").c_str());
    std::vector<Result> rows;
    while (query.step()) { if (rows.size() >= 100000) throw std::runtime_error("Archive draw limit exceeded"); rows.push_back(readResult(query)); }
    return Snapshot(std::move(rows)); // No independent expected inventory is currently established.
}
uint64_t Archive::version() const {
    Statement query(db_, "SELECT coalesce(max(version),0) FROM dataset_versions"); query.step();
    return uint64_t(query.integer64(0));
}
void Archive::versionChanged() {
    const auto digest = snapshot().digest();
    Statement insert(db_, "INSERT INTO dataset_versions(digest,created_utc) VALUES(?,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
    insert.bind(1, digest); insert.step();
}
ImportOutcome Archive::ingest(std::vector<Result> rows, std::stop_token stop, std::optional<uint64_t> expectedVersion) {
    Transaction tx(db_);
    if (expectedVersion && version() != *expectedVersion) throw std::runtime_error("Archive changed since preview; preview again");
    auto out = ingestInTransaction(std::move(rows), stop); tx.commit(); return out;
}
ImportOutcome Archive::ingestInTransaction(std::vector<Result> rows, std::stop_token stop) {
    if (rows.size() > 100000) throw std::invalid_argument("Import batch too large");
    std::vector<std::string> hashes;
    for (auto& r : rows) {
        if (stop.stop_requested()) throw std::runtime_error("Statistics import cancelled");
        r = canonicalize(std::move(r));
        if (r.unresolvedConflict) throw std::invalid_argument("Input may not forge conflict state");
        hashes.push_back(revisionDigest(r));
    }
    std::sort(hashes.begin(), hashes.end()); std::string batchBytes = "statistics-batch-v1:";
    for (const auto& hash : hashes) batchBytes += hash;
    const auto batchDigest = sha256(batchBytes); ImportOutcome out;
    for (const auto& r : rows) {
        if (stop.stop_requested()) throw std::runtime_error("Statistics import cancelled");
        const auto digest = revisionDigest(r), key = drawKey(r);
        Statement exists(db_, "SELECT 1 FROM revisions WHERE digest=?"); exists.bind(1, digest);
        if (exists.step()) { ++out.duplicate; continue; }
        std::optional<Result> active;
        {
            Statement before(db_, (std::string(RevisionQuery) + " WHERE d.draw_key=? AND d.active_digest=r.digest").c_str());
            before.bind(1, key); if (before.step()) active = readResult(before);
        }
        const auto& p = r.source;
        // Provenance identity is length-delimited via a validated revision projection.
        auto sourceProjection = r; sourceProjection.id = "03/001"; sourceProjection.date = "2003-01-01";
        sourceProjection.era = Era::Current49; sourceProjection.main = {1,2,3,4,5,6}; sourceProjection.extra = 7;
        sourceProjection.extractionOrder.reset(); sourceProjection.orderEvidence.clear();
        const auto sourceKey = revisionDigest(sourceProjection);
        Statement source(db_, "INSERT OR IGNORE INTO sources VALUES(?,?,?,?,?,?)");
        source.bind(1, sourceKey); source.bind(2, p.sourceId); source.bind(3, p.url); source.bind(4, p.lineage);
        source.bind(5, int(p.trust)); source.bind(6, p.verificationEvidence); source.step();
        Statement insert(db_, "INSERT INTO revisions VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        insert.bind(1, digest); insert.bind(2, key); insert.bind(3, r.id); insert.bind(4, r.date); insert.bind(5, int(r.era));
        for (int n = 0; n < 6; ++n) insert.bind(6 + n, r.main[n]); insert.bind(12, r.extra);
        insert.bind(13, sourceKey); insert.bind(14, p.retrievedUtc); insert.bind(15, p.contentSha256);
        std::string order; if (r.extractionOrder) for (int n : *r.extractionOrder) order += std::to_string(n) + " ";
        insert.bind(16, order); insert.bind(17, r.orderEvidence); insert.step(); ++out.inserted;
        if (!active) {
            Statement decision(db_, "INSERT INTO draws VALUES(?,?,0)"); decision.bind(1, key); decision.bind(2, digest); decision.step();
        } else if (!sameOutcome(*active, r)) {
            Statement conflict(db_, "UPDATE draws SET conflict=1 WHERE draw_key=?"); conflict.bind(1, key); conflict.step(); ++out.conflicts;
        } else if (!active->unresolvedConflict && trustRank(r.source.trust) > trustRank(active->source.trust)) {
            Statement promote(db_, "UPDATE draws SET active_digest=? WHERE draw_key=?"); promote.bind(1, digest); promote.bind(2, key); promote.step();
        }
    }
    if (out.inserted) {
        Statement batch(db_, "INSERT OR IGNORE INTO import_batches VALUES(?,strftime('%Y-%m-%dT%H:%M:%fZ','now'),?)");
        batch.bind(1, batchDigest); batch.bind(2, int(rows.size())); batch.step(); versionChanged();
    }
    if (stop.stop_requested()) throw std::runtime_error("Statistics import cancelled");
    out.version = version(); return out;
}
namespace {
void validateUpdatePlan(const UpdatePlan& plan, std::stop_token stop) {
    updateCancelled(stop); inert(plan.sourceId); inert(plan.policyVersion);
    updateCheck(plan.sourceId.size() <= 128 && plan.policyVersion.size() <= 128 && digestValid(plan.snapshotDigest), "Invalid update plan identity");
    updateCheck(!plan.requests.empty() && plan.requests.size() <= 36600, "Invalid update request count");
    std::string previous;
    for (const auto& range : plan.requests) {
        updateCancelled(stop); updateRange(range);
        updateCheck(previous.empty() || range.from > previous, "Update requests must be ordered and disjoint");
        previous = range.through;
    }
}
void writeUpdateRequests(sqlite3* db, int64_t id, const UpdatePlan& plan, std::stop_token stop) {
    for (size_t n = 0; n < plan.requests.size(); ++n) {
        updateCancelled(stop);
        Statement request(db, "INSERT INTO update_requests VALUES(?,?,?,?)");
        request.bind(1, id); request.bind(2, int(n)); request.bind(3, plan.requests[n].from); request.bind(4, plan.requests[n].through); request.step();
    }
}
}
int64_t Archive::beginUpdate(const UpdatePlan& plan, uint64_t expectedVersion, std::stop_token stop) {
    validateUpdatePlan(plan, stop); Transaction tx(db_);
    updateCheck(version() == expectedVersion && snapshot().digest() == plan.snapshotDigest, "Archive changed since update planning");
    Statement insert(db_, "INSERT INTO update_attempts(source_id,policy_version,base_version,snapshot_digest,started_utc) VALUES(?,?,?,?,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
    insert.bind(1, plan.sourceId); insert.bind(2, plan.policyVersion); insert.bind(3, int64_t(expectedVersion));
    insert.bind(4, plan.snapshotDigest); insert.step(); const auto id = int64_t(sqlite3_last_insert_rowid(db_));
    writeUpdateRequests(db_, id, plan, stop);
    updateCancelled(stop); tx.commit(); return id;
}
int64_t Archive::startUpdate(const std::string& sourceId, const std::string& policyVersion, std::stop_token stop) {
    updateCancelled(stop); inert(sourceId); inert(policyVersion);
    updateCheck(sourceId.size() <= 128 && policyVersion.size() <= 128, "Invalid update source/policy");
    Transaction tx(db_);
    Statement insert(db_, "INSERT INTO update_attempts(source_id,policy_version,base_version,snapshot_digest,started_utc) VALUES(?,?,?,?,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
    insert.bind(1, sourceId); insert.bind(2, policyVersion); insert.bind(3, int64_t(version())); insert.bind(4, snapshot().digest()); insert.step();
    const auto id = int64_t(sqlite3_last_insert_rowid(db_)); updateCancelled(stop); tx.commit(); return id;
}
void Archive::setUpdatePlan(int64_t attempt, const UpdatePlan& plan, std::stop_token stop) {
    validateUpdatePlan(plan, stop); Transaction tx(db_);
    Statement query(db_, "SELECT source_id,policy_version,base_version,snapshot_digest,status,id=(SELECT max(id) FROM update_attempts b WHERE b.source_id=a.source_id), (SELECT count(*) FROM update_requests WHERE attempt_id=a.id) FROM update_attempts a WHERE id=?");
    query.bind(1, attempt); updateCheck(query.step(), "Unknown update attempt");
    updateCheck(query.text(0) == plan.sourceId && query.text(1) == plan.policyVersion, "Plan source/policy changed during update");
    updateCheck(query.text(4) == "pending" && query.integer(5) && !query.integer(6), "Update already planned, completed or superseded");
    updateCheck(uint64_t(query.integer64(2)) == version() && query.text(3) == plan.snapshotDigest && snapshot().digest() == plan.snapshotDigest,
                "Archive changed during source check; replan");
    writeUpdateRequests(db_, attempt, plan, stop); updateCancelled(stop); tx.commit();
}
UpdateOutcome Archive::commitUpdate(int64_t attempt, const std::vector<FetchedWindow>& windows, std::stop_token stop) {
    updateCancelled(stop); updateCheck(!windows.empty() && windows.size() <= 36600, "Invalid fetched window count");
    Transaction tx(db_); std::string sourceId;
    {
        Statement query(db_, "SELECT source_id,base_version,snapshot_digest,status,id=(SELECT max(id) FROM update_attempts b WHERE b.source_id=a.source_id) FROM update_attempts a WHERE id=?");
        query.bind(1, attempt);
        updateCheck(query.step(), "Unknown update attempt"); sourceId = query.text(0);
        updateCheck(query.text(3) == "pending" && query.integer(4), "Update attempt completed or superseded");
        updateCheck(uint64_t(query.integer64(1)) == version() && query.text(2) == snapshot().digest(), "Archive changed during update; replan");
    }
    std::vector<Result> rows;
    {
        Statement requests(db_, "SELECT date_from,date_through FROM update_requests WHERE attempt_id=? ORDER BY ordinal"); requests.bind(1, attempt);
        for (const auto& window : windows) {
            updateCancelled(stop);
            updateCheck(requests.step() && window.range == DateRange{requests.text(0), requests.text(1)}, "Fetched windows do not exactly match planned requests");
            updateCheck(digestValid(window.responseSha256), "Invalid response digest");
            updateCheck(window.rows.size() <= 100000 - rows.size(), "Update result limit exceeded");
            for (const auto& raw : window.rows) {
                updateCancelled(stop); auto row = canonicalize(raw);
                updateCheck(row.source.sourceId == sourceId && row.source.contentSha256 == window.responseSha256,
                            "Result provenance does not match fetched response");
                updateCheck(row.date >= window.range.from && row.date <= window.range.through, "Result outside fetched window");
                rows.push_back(std::move(row));
            }
        }
        updateCheck(!requests.step(), "Missing fetched windows");
    }
    UpdateOutcome out; out.comparison = compareUpdate(snapshot(), rows, stop);
    // Retain genuinely different conflicting outcomes, not a new revision on every overlap check.
    std::map<std::string, std::vector<Result>> known;
    if (out.comparison.conflicts) for (auto& revision : revisions()) {
        updateCancelled(stop); known[drawKey(revision.result)].push_back(std::move(revision.result));
    }
    std::vector<Result> changed;
    for (size_t n = 0; n < rows.size(); ++n) {
        updateCancelled(stop); const auto disposition = out.comparison.rows[n];
        if (disposition == UpdateDisposition::Unchanged) continue;
        if (disposition == UpdateDisposition::Conflict) {
            const auto& candidates = known[drawKey(rows[n])];
            if (std::any_of(candidates.begin(), candidates.end(), [&](const Result& old) {
                return old.source.sourceId == rows[n].source.sourceId && trustRank(old.source.trust) >= trustRank(rows[n].source.trust) && sameOutcome(old, rows[n]);
            })) continue;
        }
        changed.push_back(std::move(rows[n]));
    }
    out.ingestion = ingestInTransaction(std::move(changed), stop);
    for (size_t n = 0; n < windows.size(); ++n) {
        updateCancelled(stop); Statement receipt(db_, "INSERT INTO fetched_windows VALUES(?,?,?)");
        receipt.bind(1, attempt); receipt.bind(2, int(n)); receipt.bind(3, windows[n].responseSha256); receipt.step();
    }
    Statement success(db_, "UPDATE update_attempts SET status='succeeded',finished_utc=strftime('%Y-%m-%dT%H:%M:%fZ','now'),added=?,unchanged=?,needs_verification=?,conflicts=? WHERE id=?");
    success.bind(1, int(out.comparison.added)); success.bind(2, int(out.comparison.unchanged));
    success.bind(3, int(out.comparison.needsVerification)); success.bind(4, int(out.comparison.conflicts)); success.bind(5, attempt); success.step();
    updateCancelled(stop); tx.commit(); return out;
}
bool Archive::finishUpdate(int64_t attempt, bool cancelled, const std::string& detail) {
    inert(detail); Transaction tx(db_);
    Statement query(db_, "SELECT status FROM update_attempts WHERE id=?"); query.bind(1, attempt);
    updateCheck(query.step(), "Unknown update attempt"); if (query.text(0) != "pending") return false;
    Statement finish(db_, "UPDATE update_attempts SET status=?,detail=?,finished_utc=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?");
    finish.bind(1, std::string(cancelled ? "cancelled" : "failed")); finish.bind(2, detail); finish.bind(3, attempt); finish.step(); tx.commit(); return true;
}
std::vector<UpdateAttempt> Archive::updateAttempts() const {
    Statement query(db_, "SELECT id,source_id,policy_version,started_utc,finished_utc,status,detail,base_version,added,unchanged,needs_verification,conflicts FROM update_attempts ORDER BY id");
    std::vector<UpdateAttempt> out;
    while (query.step()) {
        updateCheck(out.size() < 100000, "Update attempt limit exceeded");
        out.push_back({query.integer64(0), query.text(1), query.text(2), query.text(3), query.text(4), query.text(5), query.text(6),
                       uint64_t(query.integer64(7)), size_t(query.integer(8)), size_t(query.integer(9)), size_t(query.integer(10)), size_t(query.integer(11))});
    }
    return out;
}
std::vector<CommittedWindow> Archive::committedWindows() const {
    Statement query(db_, "SELECT a.source_id,a.policy_version,r.date_from,r.date_through,w.response_sha256 FROM fetched_windows w JOIN update_requests r ON r.attempt_id=w.attempt_id AND r.ordinal=w.ordinal JOIN update_attempts a ON a.id=w.attempt_id WHERE a.status='succeeded' ORDER BY a.id,r.ordinal");
    std::vector<CommittedWindow> out;
    while (query.step()) {
        updateCheck(out.size() < 100000, "Committed window limit exceeded");
        out.push_back({query.text(0), query.text(1), {query.text(2), query.text(3)}, query.text(4)});
    }
    return out;
}
void Archive::resolve(const std::string& key, const std::string& digest, const std::string& reason, const std::string& evidence,
                      std::stop_token stop, std::optional<uint64_t> expectedVersion) {
    inert(reason); inert(evidence); Transaction tx(db_);
    if (stop.stop_requested()) throw std::runtime_error("Resolution cancelled");
    if (expectedVersion && version() != *expectedVersion) throw std::runtime_error("Archive changed since conflict inspection; reload and inspect again");
    Statement exists(db_, "SELECT 1 FROM revisions r JOIN draws d ON d.draw_key=r.draw_key WHERE r.digest=? AND r.draw_key=? AND d.conflict=1");
    exists.bind(1, digest); exists.bind(2, key); if (!exists.step()) throw std::invalid_argument("No unresolved conflict for selected revision");
    Statement decision(db_, "INSERT INTO decisions(draw_key,selected_digest,reason,evidence,decided_utc) VALUES(?,?,?,?,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
    decision.bind(1, key); decision.bind(2, digest); decision.bind(3, reason); decision.bind(4, evidence); decision.step();
    Statement active(db_, "UPDATE draws SET active_digest=?,conflict=0 WHERE draw_key=?");
    active.bind(1, digest); active.bind(2, key); active.step(); versionChanged();
    if (stop.stop_requested()) throw std::runtime_error("Resolution cancelled");
    tx.commit();
}
std::vector<Resolution> Archive::resolutions() const {
    Statement q(db_, "SELECT draw_key,selected_digest,reason,evidence,decided_utc FROM decisions ORDER BY sequence");
    std::vector<Resolution> out;
    while (q.step()) out.push_back({q.text(0),q.text(1),q.text(2),q.text(3),q.text(4)});
    return out;
}
}
