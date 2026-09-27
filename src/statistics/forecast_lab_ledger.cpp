#include "forecast_lab_ledger.hpp"
#include "forecast_lab_storage.hpp"
#include "archive.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace marksix::statistics {
namespace {
constexpr int ApplicationId = 0x4d36464c;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void checked(int rc, sqlite3* db) {
    if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE)
        throw std::runtime_error(std::string("Forecast Lab ledger: ") + sqlite3_errmsg(db));
}
void exec(sqlite3* db, const char* sql) { checked(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), db); }
struct Query {
    sqlite3* db; sqlite3_stmt* statement = nullptr;
    Query(sqlite3* owner, const char* sql) : db(owner) {
        checked(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr), db);
    }
    ~Query() { sqlite3_finalize(statement); }
    void number(int at, int64_t value) { checked(sqlite3_bind_int64(statement, at, value), db); }
    void text(int at, const std::string& value) {
        checked(sqlite3_bind_text(statement, at, value.data(), int(value.size()), SQLITE_TRANSIENT), db);
    }
    void blob(int at, const std::string& value) {
        require(value.size() <= 4'000'000, "Oversized Forecast Lab blob");
        checked(sqlite3_bind_blob(statement, at, value.data(), int(value.size()), SQLITE_TRANSIENT), db);
    }
    bool step() { const int rc = sqlite3_step(statement); checked(rc, db); return rc == SQLITE_ROW; }
    int64_t integer(int col) const { return sqlite3_column_int64(statement, col); }
    std::string value(int col) const {
        const int n = sqlite3_column_bytes(statement, col);
        require(n >= 0 && n <= 4'000'000, "Oversized Forecast Lab stored value");
        const auto* p = static_cast<const char*>(sqlite3_column_blob(statement, col));
        return p ? std::string(p, size_t(n)) : std::string{};
    }
    bool null(int col) const { return sqlite3_column_type(statement, col) == SQLITE_NULL; }
};
struct Transaction {
    sqlite3* db; bool committed = false;
    explicit Transaction(sqlite3* owner) : db(owner) { exec(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { exec(db, "COMMIT"); committed = true; }
};
int64_t scalar(sqlite3* db, const char* sql) {
    Query q(db, sql); require(q.step(), "Forecast Lab metadata missing"); return q.integer(0);
}
void page(size_t offset, size_t limit) {
    require(offset <= 100000 && limit >= 1 && limit <= 200, "Forecast Lab page limit");
}
void digest(const std::string& value) {
    require(value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos,
            "Invalid Forecast Lab digest");
}
void timestamp(const std::string& value) {
    require(value.size() == 20 && validDate(value.substr(0, 10)) && value[10] == 'T' &&
            value[13] == ':' && value[16] == ':' && value[19] == 'Z',
            "Invalid Forecast Lab timestamp");
}
LabLineage lineage(Query& q) {
    LabLineage out;
    out.id = q.integer(0); out.protocol = q.value(1); out.sourceDigest = q.value(2);
    out.sourceVersion = uint64_t(q.integer(3)); out.createdUtc = q.value(4);
    out.reason = q.value(5); out.completedDraws = size_t(q.integer(6));
    require(out.id > 0 && out.completedDraws <= 100000, "Invalid Forecast Lab lineage");
    digest(out.protocol); digest(out.sourceDigest); timestamp(out.createdUtc);
    return out;
}
LabReplay checkpoint(sqlite3* db, int64_t lineageId, size_t ordinal) {
    Query q(db, "SELECT payload,checksum FROM checkpoints WHERE lineage=? AND ordinal=?");
    q.number(1, lineageId); q.number(2, int64_t(ordinal));
    require(q.step(), "Forecast Lab checkpoint missing");
    const auto bytes = q.value(0); require(sha256(bytes) == q.value(1), "Forecast Lab checkpoint checksum mismatch");
    auto replay = decodeLabCheckpoint(bytes);
    require(replay.adaptive.draws == ordinal, "Forecast Lab checkpoint ordinal mismatch");
    return replay;
}
void insertCheckpoint(sqlite3* db, int64_t lineageId, const LabReplay& replay) {
    const auto bytes = encodeLabCheckpoint(replay);
    Query q(db, "INSERT INTO checkpoints(lineage,ordinal,payload,checksum) VALUES(?,?,?,?)");
    q.number(1, lineageId); q.number(2, int64_t(replay.adaptive.draws));
    q.blob(3, bytes); q.text(4, sha256(bytes)); q.step();
}
void insertForecast(sqlite3* db, int64_t lineageId, const LabReplay& replay, const std::string& utc) {
    require(replay.next.has_value(), "Forecast Lab next forecast missing");
    const auto bytes = encodeLabForecast(*replay.next);
    Query q(db, "INSERT INTO forecasts(lineage,ordinal,created_utc,prefix,payload,checksum) VALUES(?,?,?,?,?,?)");
    q.number(1, lineageId); q.number(2, int64_t(replay.next->targetOrdinal));
    q.text(3, utc); q.text(4, replay.next->prefixDigest);
    q.blob(5, bytes); q.text(6, sha256(bytes)); q.step();
}
void insertOutcome(sqlite3* db, int64_t lineageId, const Result& result, size_t ordinal) {
    const auto bytes = encodeLabResult(result);
    Query q(db, "INSERT INTO outcomes(lineage,ordinal,revision,payload,checksum,provenance) VALUES(?,?,?,?,?,'historical-replay')");
    q.number(1, lineageId); q.number(2, int64_t(ordinal));
    q.text(3, revisionDigest(result)); q.blob(4, bytes); q.text(5, sha256(bytes)); q.step();
}
void promote(sqlite3* db, int64_t lineageId, size_t ordinal, const std::string& sourceDigest,
             uint64_t sourceVersion) {
    Query q(db, "INSERT INTO head(singleton,lineage,ordinal,source_digest,source_version) VALUES(1,?,?,?,?) "
                "ON CONFLICT(singleton) DO UPDATE SET lineage=excluded.lineage,ordinal=excluded.ordinal,"
                "source_digest=excluded.source_digest,source_version=excluded.source_version");
    q.number(1, lineageId); q.number(2, int64_t(ordinal));
    q.text(3, sourceDigest); q.number(4, int64_t(sourceVersion)); q.step();
}
std::string valuePrefix(const std::vector<Result>& rows, size_t count) {
    std::string prefix = adaptiveProtocolDigest();
    for (size_t i = 0; i < count; ++i) prefix = adaptiveValuePrefix(prefix, rows[i]);
    return prefix;
}
}
LabLedger::LabLedger(const std::filesystem::path& root) : path_(root/"statistics"/"forecast_lab.sqlite3") {
    const bool existed = std::filesystem::exists(path_);
    std::filesystem::create_directories(path_.parent_path());
    const auto utf8 = path_.u8string();
    try {
        checked(sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &db_,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), db_);
        sqlite3_busy_timeout(db_, 3000);
        sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, 4'000'000);
        exec(db_, "PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF;");
        if (existed) {
            require(scalar(db_, "PRAGMA application_id") == ApplicationId &&
                    scalar(db_, "PRAGMA user_version") == 1,
                    "Unknown Forecast Lab schema; file preserved");
            Query q(db_, "PRAGMA quick_check");
            require(q.step() && q.value(0) == "ok", "Corrupt Forecast Lab ledger; file preserved");
        } else {
            Transaction tx(db_);
            exec(db_, R"sql(
CREATE TABLE lineages(id INTEGER PRIMARY KEY,protocol TEXT NOT NULL,source_digest TEXT NOT NULL,
 source_version INTEGER NOT NULL,created_utc TEXT NOT NULL,reason TEXT NOT NULL);
CREATE TABLE forecasts(lineage INTEGER NOT NULL REFERENCES lineages(id),ordinal INTEGER NOT NULL,
 created_utc TEXT NOT NULL,prefix TEXT NOT NULL,payload BLOB NOT NULL,checksum TEXT NOT NULL,
 PRIMARY KEY(lineage,ordinal));
CREATE TABLE outcomes(lineage INTEGER NOT NULL,ordinal INTEGER NOT NULL,revision TEXT NOT NULL,
 payload BLOB NOT NULL,checksum TEXT NOT NULL,provenance TEXT NOT NULL,
 PRIMARY KEY(lineage,ordinal),FOREIGN KEY(lineage,ordinal) REFERENCES forecasts(lineage,ordinal));
CREATE TABLE checkpoints(lineage INTEGER NOT NULL REFERENCES lineages(id),ordinal INTEGER NOT NULL,
 payload BLOB NOT NULL,checksum TEXT NOT NULL,PRIMARY KEY(lineage,ordinal));
CREATE TABLE head(singleton INTEGER PRIMARY KEY CHECK(singleton=1),lineage INTEGER NOT NULL,
 ordinal INTEGER NOT NULL,source_digest TEXT NOT NULL,source_version INTEGER NOT NULL,
 FOREIGN KEY(lineage,ordinal) REFERENCES checkpoints(lineage,ordinal));
CREATE TABLE controls(protocol TEXT NOT NULL,source_digest TEXT NOT NULL,history_length INTEGER NOT NULL,
 payload BLOB NOT NULL,checksum TEXT NOT NULL,PRIMARY KEY(protocol,source_digest,history_length));
CREATE TABLE fetch_boundaries(id INTEGER PRIMARY KEY,lineage INTEGER NOT NULL REFERENCES lineages(id),
 ordinal INTEGER NOT NULL,attempt INTEGER NOT NULL,source_version INTEGER NOT NULL,
 source_digest TEXT NOT NULL,prefix TEXT NOT NULL,created_utc TEXT NOT NULL);
CREATE TABLE fetch_seen(boundary INTEGER NOT NULL REFERENCES fetch_boundaries(id),draw_key TEXT NOT NULL,
 PRIMARY KEY(boundary,draw_key));
CREATE TABLE prefetch_annotations(lineage INTEGER NOT NULL,ordinal INTEGER NOT NULL,
 revision TEXT NOT NULL,boundary INTEGER NOT NULL UNIQUE REFERENCES fetch_boundaries(id),
 first_retrieval TEXT NOT NULL,detail TEXT NOT NULL,
 PRIMARY KEY(lineage,ordinal,revision),
 FOREIGN KEY(lineage,ordinal) REFERENCES outcomes(lineage,ordinal));
PRAGMA application_id=0x4d36464c;
PRAGMA user_version=1;
)sql");
            for (const char* table : {"lineages", "forecasts", "outcomes", "checkpoints", "controls",
                                       "fetch_boundaries", "fetch_seen", "prefetch_annotations"})
                for (const char* operation : {"UPDATE", "DELETE"}) {
                    const std::string sql = std::string("CREATE TRIGGER immutable_lab_") + table + '_' + operation +
                        " BEFORE " + operation + " ON " + table +
                        " BEGIN SELECT RAISE(ABORT,'Immutable Forecast Lab record'); END;";
                    exec(db_, sql.c_str());
                }
            tx.commit();
        }
        Query foreign(db_, "PRAGMA foreign_key_check");
        require(!foreign.step(), "Broken Forecast Lab references");
        if (const auto current = head()) (void)checkpoint(db_, current->id, current->completedDraws);
    } catch (...) {
        if (db_) sqlite3_close(db_); db_ = nullptr; throw;
    }
}
LabLedger::~LabLedger() { if (db_) sqlite3_close(db_); }
std::optional<LabLineage> LabLedger::head() const {
    Query q(db_, "SELECT l.id,l.protocol,l.source_digest,l.source_version,l.created_utc,l.reason,h.ordinal "
                 "FROM head h JOIN lineages l ON l.id=h.lineage WHERE h.singleton=1");
    if (!q.step()) return std::nullopt;
    return lineage(q);
}
std::vector<LabLineage> LabLedger::lineages(size_t offset, size_t limit) const {
    page(offset, limit);
    Query q(db_, "SELECT l.id,l.protocol,l.source_digest,l.source_version,l.created_utc,l.reason,"
                 "COALESCE((SELECT MAX(ordinal) FROM checkpoints c WHERE c.lineage=l.id),0) "
                 "FROM lineages l ORDER BY l.id DESC LIMIT ? OFFSET ?");
    q.number(1, int64_t(limit)); q.number(2, int64_t(offset));
    std::vector<LabLineage> out;
    while (q.step()) out.push_back(lineage(q));
    return out;
}
std::vector<LabStoredForecast> LabLedger::forecasts(int64_t lineageId, size_t offset, size_t limit) const {
    page(offset, limit);
    Query q(db_, "SELECT f.created_utc,f.prefix,f.payload,f.checksum,o.revision,o.payload,o.checksum,o.provenance,a.detail "
                 "FROM forecasts f LEFT JOIN outcomes o ON o.lineage=f.lineage AND o.ordinal=f.ordinal "
                 "LEFT JOIN prefetch_annotations a ON a.lineage=o.lineage AND a.ordinal=o.ordinal AND a.revision=o.revision "
                 "WHERE f.lineage=? ORDER BY f.ordinal LIMIT ? OFFSET ?");
    q.number(1, lineageId); q.number(2, int64_t(limit)); q.number(3, int64_t(offset));
    std::vector<LabStoredForecast> out;
    while (q.step()) {
        LabStoredForecast row; row.lineage = lineageId; row.createdUtc = q.value(0);
        const auto payload = q.value(2);
        require(sha256(payload) == q.value(3), "Forecast Lab forecast checksum mismatch");
        row.forecast = decodeLabForecast(payload);
        require(row.forecast.prefixDigest == q.value(1), "Forecast Lab forecast prefix mismatch");
        if (!q.null(4)) {
            row.outcomeRevision = q.value(4);
            const auto outcomeBytes = q.value(5);
            require(sha256(outcomeBytes) == q.value(6), "Forecast Lab outcome checksum mismatch");
            row.outcome = decodeLabResult(outcomeBytes);
            require(revisionDigest(*row.outcome) == row.outcomeRevision,
                    "Forecast Lab outcome revision mismatch");
            row.provenance = q.null(8) ? q.value(7) : "locally-pre-fetch: " + q.value(8);
        } else row.provenance = "pending-unverified";
        out.push_back(std::move(row));
    }
    return out;
}
LabEvaluation LabLedger::evaluation(int64_t lineageId, std::stop_token stop) const {
    LabReplay replay;
    replay.available = true;
    for (size_t offset = 0;; offset += 100) {
        if (stop.stop_requested()) throw std::runtime_error("Forecast Lab evaluation cancelled");
        const auto rows = forecasts(lineageId, offset, 100);
        for (const auto& row : rows) if (row.outcome) {
            LabStep step;
            step.actual = *row.outcome;
            step.frozen = row.forecast;
            step.scores = labScore(step.frozen, step.actual);
            replay.steps.push_back(std::move(step));
        }
        if (rows.size() < 100) break;
    }
    return evaluateLab(replay);
}
LabReconcile LabLedger::reconcile(const Snapshot& snapshot, uint64_t sourceVersion,
                                  const std::string& createdUtc, std::stop_token stop) {
    timestamp(createdUtc);
    require(sourceVersion <= uint64_t(std::numeric_limits<int64_t>::max()), "Source version overflow");
    LabReconcile out;
    const auto rows = snapshot.select();
    if (rows.empty() || rows.front().date != "2002-07-04" || rows.front().id != "02/053") {
        out.error = "Verified current-49 seed 02/053 required";
        return out;
    }
    const auto sourceDigest = snapshot.digest();
    const auto current = head();
    bool append = false;
    if (current && current->protocol == labProtocolDigest() &&
        current->completedDraws <= rows.size()) {
        auto restored = checkpoint(db_, current->id, current->completedDraws);
        append = restored.adaptive.prefixDigest == valuePrefix(rows, current->completedDraws);
        if (append) {
            out.lineage = *current;
            out.replay = std::move(restored);
            out.replay.snapshotDigest = sourceDigest;
            if (current->completedDraws == rows.size()) {
                out.available = out.reused = true;
                return out;
            }
        }
    }
    if (!append) {
        out.revised = current.has_value();
        out.replay = labStart(rows.front());
        out.replay.snapshotDigest = sourceDigest;
        Transaction tx(db_);
        Query create(db_, "INSERT INTO lineages(protocol,source_digest,source_version,created_utc,reason) "
                          "VALUES(?,?,?,?,?)");
        create.text(1, labProtocolDigest()); create.text(2, sourceDigest);
        create.number(3, int64_t(sourceVersion)); create.text(4, createdUtc);
        create.text(5, out.revised ? "corrected values or changed protocol" : "initial verified seed");
        create.step();
        const int64_t id = sqlite3_last_insert_rowid(db_);
        insertCheckpoint(db_, id, out.replay);
        insertForecast(db_, id, out.replay, createdUtc);
        if (rows.size() == 1) promote(db_, id, 1, sourceDigest, sourceVersion);
        tx.commit();
        out.lineage = {id, labProtocolDigest(), sourceDigest, createdUtc,
                       out.revised ? "corrected values or changed protocol" : "initial verified seed",
                       sourceVersion, 1};
    }
    const size_t first = out.replay.adaptive.draws;
    for (size_t i = first; i < rows.size(); ++i) {
        if (stop.stop_requested()) throw std::runtime_error("Forecast Lab reconciliation cancelled");
        labAppend(out.replay, rows[i]);
        Transaction tx(db_);
        insertOutcome(db_, out.lineage.id, rows[i], i + 1);
        insertCheckpoint(db_, out.lineage.id, out.replay);
        insertForecast(db_, out.lineage.id, out.replay, createdUtc);
        if (append || i + 1 == rows.size())
            promote(db_, out.lineage.id, i + 1, sourceDigest, sourceVersion);
        tx.commit();
        ++out.added;
    }
    out.lineage.completedDraws = rows.size();
    out.available = true;
    return out;
}
int64_t LabLedger::prepareFetch(const ArchiveState& before, int64_t attempt,
                                const std::string& createdUtc) {
    timestamp(createdUtc);
    const auto job = std::find_if(before.attempts.begin(), before.attempts.end(),
        [&](const auto& row) { return row.id == attempt; });
    require(job != before.attempts.end() && job->status == "pending" &&
            job->baseVersion == before.version &&
            job->startedUtc.substr(0, 19) <= createdUtc.substr(0, 19),
            "Forecast Lab fetch boundary needs the pending source attempt");
    const auto current = head();
    if (!current || current->protocol != labProtocolDigest()) return 0;
    const auto rows = before.snapshot.select();
    if (rows.size() != current->completedDraws) return 0;
    const auto saved = checkpoint(db_, current->id, current->completedDraws);
    if (saved.adaptive.prefixDigest != valuePrefix(rows, rows.size())) return 0;
    Transaction tx(db_);
    Query insert(db_, "INSERT INTO fetch_boundaries(lineage,ordinal,attempt,source_version,"
                      "source_digest,prefix,created_utc) VALUES(?,?,?,?,?,?,?)");
    insert.number(1, current->id); insert.number(2, int64_t(rows.size() + 1));
    insert.number(3, attempt); insert.number(4, int64_t(before.version));
    insert.text(5, before.snapshot.digest()); insert.text(6, saved.adaptive.prefixDigest);
    insert.text(7, createdUtc); insert.step();
    const int64_t boundary = sqlite3_last_insert_rowid(db_);
    for (const auto& revision : before.revisions) {
        Query seen(db_, "INSERT OR IGNORE INTO fetch_seen(boundary,draw_key) VALUES(?,?)");
        seen.number(1, boundary); seen.text(2, drawKey(revision.result)); seen.step();
    }
    tx.commit();
    return boundary;
}
void LabLedger::annotateFetched(const ArchiveState& after, const std::string& createdUtc) {
    timestamp(createdUtc);
    const auto current = head();
    if (!current || current->protocol != labProtocolDigest()) return;
    const auto rows = after.snapshot.select();
    for (const auto& attempt : after.attempts) {
        if (attempt.status != "succeeded" || after.version != attempt.baseVersion + 1 ||
            attempt.finishedUtc.empty() || attempt.finishedUtc.substr(0, 19) > createdUtc.substr(0, 19)) continue;
        Query boundary(db_, "SELECT id,lineage,ordinal,source_version,prefix,created_utc "
                            "FROM fetch_boundaries WHERE attempt=? AND source_version=? ORDER BY id DESC LIMIT 1");
        boundary.number(1, attempt.id); boundary.number(2, int64_t(attempt.baseVersion));
        if (!boundary.step()) continue;
        const auto boundaryId = boundary.integer(0), lineageId = boundary.integer(1);
        const auto ordinal = size_t(boundary.integer(2));
        const auto version = uint64_t(boundary.integer(3));
        const auto prefix = boundary.value(4), started = boundary.value(5);
        if (lineageId != current->id || ordinal < 2 || ordinal > rows.size() ||
            ordinal > current->completedDraws || version + 1 != after.version ||
            valuePrefix(rows, ordinal - 1) != prefix) continue;
        const auto& actual = rows[ordinal - 1];
        if (actual.source.trust != Trust::OfficialRetrieval ||
            actual.source.sourceId != attempt.sourceId) continue;
        Query seen(db_, "SELECT 1 FROM fetch_seen WHERE boundary=? AND draw_key=?");
        seen.number(1, boundaryId); seen.text(2, drawKey(actual));
        if (seen.step()) continue;
        const auto revision = revisionDigest(actual);
        Query saved(db_, "SELECT f.created_utc,o.revision FROM forecasts f JOIN outcomes o "
                         "ON o.lineage=f.lineage AND o.ordinal=f.ordinal "
                         "WHERE f.lineage=? AND f.ordinal=?");
        saved.number(1, lineageId); saved.number(2, int64_t(ordinal));
        if (!saved.step() || saved.value(1) != revision ||
            saved.value(0).substr(0, 19) > started.substr(0, 19)) continue;
        std::string first;
        bool matched = false;
        for (const auto& stored : after.revisions)
            if (drawKey(stored.result) == drawKey(actual)) {
                const auto retrieval = stored.result.source.retrievedUtc;
                if (first.empty() || retrieval < first) first = retrieval;
                if (stored.digest == revision) matched = true;
            }
        if (!matched || first.empty() || first.substr(0, 19) <= started.substr(0, 19) ||
            first.substr(0, 19) > attempt.finishedUtc.substr(0, 19) ||
            first.substr(0, 19) > createdUtc.substr(0, 19)) continue;
        const std::string detail = "Saved before local fetch; first retrieval " + first +
            ". Timing before the real draw is unverified.";
        Transaction tx(db_);
        Query mark(db_, "INSERT OR IGNORE INTO prefetch_annotations"
                        "(lineage,ordinal,revision,boundary,first_retrieval,detail) VALUES(?,?,?,?,?,?)");
        mark.number(1, lineageId); mark.number(2, int64_t(ordinal));
        mark.text(3, revision); mark.number(4, boundaryId);
        mark.text(5, first); mark.text(6, detail); mark.step();
        tx.commit();
    }
}
void LabLedger::saveControl(const LabControlReport& report) {
    digest(report.protocolDigest); digest(report.sourceDigest);
    const auto bytes = encodeLabControl(report);
    Transaction tx(db_);
    Query previous(db_, "SELECT payload,checksum FROM controls WHERE protocol=? AND source_digest=? AND history_length=?");
    previous.text(1, report.protocolDigest); previous.text(2, report.sourceDigest);
    previous.number(3, int64_t(report.historyLength));
    if (previous.step()) {
        require(previous.value(1) == sha256(previous.value(0)) && previous.value(0) == bytes,
                "Forecast Lab control cache conflict or corruption");
    } else {
        Query q(db_, "INSERT INTO controls(protocol,source_digest,history_length,payload,checksum) VALUES(?,?,?,?,?)");
        q.text(1, report.protocolDigest); q.text(2, report.sourceDigest);
        q.number(3, int64_t(report.historyLength)); q.blob(4, bytes); q.text(5, sha256(bytes)); q.step();
    }
    tx.commit();
}
std::optional<LabControlReport> LabLedger::control(const std::string& sourceDigest,
                                                    size_t historyLength) const {
    digest(sourceDigest);
    Query q(db_, "SELECT payload,checksum FROM controls WHERE protocol=? AND source_digest=? AND history_length=?");
    q.text(1, labProtocolDigest()); q.text(2, sourceDigest); q.number(3, int64_t(historyLength));
    if (!q.step()) return std::nullopt;
    const auto bytes = q.value(0);
    require(sha256(bytes) == q.value(1), "Forecast Lab control checksum mismatch");
    auto report = decodeLabControl(bytes);
    require(report.sourceDigest == sourceDigest && report.historyLength == historyLength,
            "Forecast Lab control cache key mismatch");
    return report;
}
}
