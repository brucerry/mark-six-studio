#include "adaptive_ledger.hpp"
#include "archive.hpp"
#include <algorithm>
#include <sqlite3.h>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace marksix::statistics {
namespace {
constexpr int ApplicationId = 0x4d364144;
constexpr size_t MaxBlob = 32768;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Writer {
    std::string bytes;
    void integer(uint64_t n) { for (unsigned i = 0; i < 8; ++i) bytes += char((n >> (i*8)) & 255); }
    void real(double n) { require(std::isfinite(n), "Nonfinite adaptive storage value"); integer(std::bit_cast<uint64_t>(n)); }
    void text(std::string_view s) { require(s.size() <= 2048, "Adaptive storage text limit"); integer(s.size()); bytes += s; }
};
struct Reader {
    std::string_view bytes; size_t at = 0;
    explicit Reader(std::string_view input) : bytes(input) { require(input.size() <= MaxBlob, "Adaptive storage payload limit"); }
    uint64_t integer() {
        require(at <= bytes.size() && bytes.size()-at >= 8, "Truncated adaptive storage");
        uint64_t n = 0; for (unsigned i = 0; i < 8; ++i) n |= uint64_t(static_cast<unsigned char>(bytes[at++])) << (i*8); return n;
    }
    double real() { const double n = std::bit_cast<double>(integer()); require(std::isfinite(n), "Nonfinite stored adaptive value"); return n; }
    std::string text() {
        const auto n = integer(); require(n <= 2048 && n <= bytes.size()-at, "Invalid adaptive string length");
        std::string out(bytes.substr(at, size_t(n))); at += size_t(n); return out;
    }
    void end() { require(at == bytes.size(), "Trailing adaptive storage bytes"); }
};
void probabilities(Writer& w, const Probabilities& p) {
    for (const auto& q : p) { w.real(q.main); w.real(q.extra); w.real(q.absent); }
}
std::string predictionBytes(const AdaptivePrediction& p) {
    Writer w; w.text("adaptive-prediction-v1"); probabilities(w, p.probabilities);
    for (const auto& expert : p.experts) probabilities(w, expert);
    for (double v : p.weights) w.real(v);
    for (int n : p.selection.main) w.integer(uint64_t(n)); w.integer(uint64_t(p.selection.extra));
    w.integer(p.trainingDraws); w.integer(p.warmup ? 1 : 0); w.text(p.cutoff); w.text(p.prefixDigest); return w.bytes;
}
void result(Writer& w, const Result& input) {
    const auto r = canonicalize(input);
    w.text(r.id); w.text(r.date); for (int n : r.main) w.integer(uint64_t(n)); w.integer(uint64_t(r.extra));
    w.integer(uint64_t(r.era));
    for (const auto* s : {&r.source.sourceId, &r.source.url, &r.source.retrievedUtc, &r.source.contentSha256, &r.source.lineage}) w.text(*s);
    w.integer(uint64_t(r.source.trust)); w.text(r.source.verificationEvidence);
    w.integer(r.extractionOrder ? 1 : 0); if (r.extractionOrder) for (int n : *r.extractionOrder) w.integer(uint64_t(n));
    w.text(r.orderEvidence); w.integer(r.unresolvedConflict ? 1 : 0);
}
Result result(Reader& r) {
    const auto bounded = [&r](uint64_t max) { const auto n = r.integer(); require(n <= max, "Invalid adaptive stored enum/number"); return int(n); };
    Result out; out.id = r.text(); out.date = r.text(); for (auto& n : out.main) n = bounded(49); out.extra = bounded(49);
    out.era = Era(bounded(1));
    out.source.sourceId = r.text(); out.source.url = r.text(); out.source.retrievedUtc = r.text();
    out.source.contentSha256 = r.text(); out.source.lineage = r.text(); out.source.trust = Trust(bounded(2));
    out.source.verificationEvidence = r.text();
    if (bounded(1)) { std::array<int, 6> order{}; for (auto& n : order) n = bounded(49); out.extractionOrder = order; }
    out.orderEvidence = r.text(); out.unresolvedConflict = bounded(1) != 0; return canonicalize(out);
}
void loss(Writer& w, const Loss& l) { w.real(l.brier); w.real(l.log); w.real(l.mainBrier); w.real(l.extraBrier); w.real(l.anyBrier); }
std::string outcomeBytes(const AdaptiveStep& step) {
    Writer w; w.text("adaptive-outcome-v1"); result(w, step.actual); loss(w, step.adaptiveLoss); loss(w, step.uniformLoss);
    for (double v : step.expertLogLoss) w.real(v); for (double v : step.weightsAfter) w.real(v);
    w.bytes += encodeAdaptiveState(step.after); return w.bytes;
}
void sqlCheck(int rc, sqlite3* db) {
    if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) throw std::runtime_error(std::string("Adaptive ledger: ")+sqlite3_errmsg(db));
}
void exec(sqlite3* db, const char* sql) { sqlCheck(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), db); }
struct Statement {
    sqlite3* db; sqlite3_stmt* q = nullptr;
    Statement(sqlite3* d, const char* sql) : db(d) { sqlCheck(sqlite3_prepare_v2(db, sql, -1, &q, nullptr), db); }
    ~Statement() { sqlite3_finalize(q); }
    void number(int at, int64_t n) { sqlCheck(sqlite3_bind_int64(q, at, n), db); }
    void text(int at, const std::string& s) { sqlCheck(sqlite3_bind_text(q, at, s.data(), int(s.size()), SQLITE_TRANSIENT), db); }
    void blob(int at, const std::string& s) { require(s.size() <= MaxBlob, "Adaptive blob limit"); sqlCheck(sqlite3_bind_blob(q, at, s.data(), int(s.size()), SQLITE_TRANSIENT), db); }
    bool step() { const int rc = sqlite3_step(q); sqlCheck(rc, db); return rc == SQLITE_ROW; }
    int64_t number(int col) const { return sqlite3_column_int64(q, col); }
    std::string value(int col) const {
        const auto n = sqlite3_column_bytes(q, col); require(n >= 0 && size_t(n) <= MaxBlob, "Oversized adaptive stored value");
        const auto* p = static_cast<const char*>(sqlite3_column_blob(q, col)); return p ? std::string(p, size_t(n)) : std::string{};
    }
};
struct Transaction {
    sqlite3* db; bool committed = false;
    explicit Transaction(sqlite3* d) : db(d) { exec(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { exec(db, "COMMIT"); committed = true; }
};
int64_t scalar(sqlite3* db, const char* sql) { Statement s(db, sql); require(s.step(), "Missing adaptive metadata"); return s.number(0); }
void digest(const std::string& s) { require(s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos, "Invalid adaptive digest"); }
void timestamp(const std::string& s) {
    // Canonical seconds-only local UTC metadata, never trusted time certification.
    require(s.size() == 20 && validDate(s.substr(0,10)) && s[10] == 'T' && s[13] == ':' && s[16] == ':' && s[19] == 'Z', "Invalid adaptive timestamp");
    for (size_t at : {size_t(11),size_t(14),size_t(17)}) {
        const auto part = s.substr(at,2); require(part.find_first_not_of("0123456789") == std::string::npos && part <= (at == 11 ? "23" : "59"), "Invalid adaptive time");
    }
}
AdaptiveLineage lineage(Statement& q) {
    AdaptiveLineage out; out.id = q.number(0); out.protocol = q.value(1); out.sourceDigest = q.value(2);
    out.sourceVersion = uint64_t(q.number(3)); out.createdUtc = q.value(4); out.reason = q.value(5);
    require(out.id > 0 && q.number(3) >= 0, "Invalid adaptive lineage identity"); digest(out.protocol); digest(out.sourceDigest); timestamp(out.createdUtc); return out;
}
void page(size_t offset, size_t limit) { require(offset <= 100000 && limit >= 1 && limit <= 200, "Adaptive page limit"); }
}
std::string encodeAdaptiveState(const AdaptiveState& state) {
    validateAdaptiveState(state); Writer w; w.text("adaptive-state-v1"); w.integer(state.draws);
    w.text(state.lastDate); w.text(state.lastId); w.text(state.prefixDigest);
    for (const auto& c : state.counts) { w.real(c.n); for (double n : c.main) w.real(n); for (double n : c.extra) w.real(n); }
    for (double n : state.logWeights) w.real(n); return w.bytes;
}
AdaptiveState decodeAdaptiveState(std::string_view bytes) {
    Reader r(bytes); require(r.text() == "adaptive-state-v1", "Unknown adaptive state version");
    AdaptiveState state; const auto n = r.integer(); require(n <= 100000, "Adaptive checkpoint count limit"); state.draws = size_t(n);
    state.lastDate = r.text(); state.lastId = r.text(); state.prefixDigest = r.text();
    for (auto& c : state.counts) { c.n = r.real(); for (auto& v : c.main) v = r.real(); for (auto& v : c.extra) v = r.real(); }
    for (auto& v : state.logWeights) v = r.real(); r.end(); validateAdaptiveState(state); return state;
}
AdaptiveLedger::AdaptiveLedger(const std::filesystem::path& root) : path_(root/"statistics"/"adaptive.sqlite3") {
    const bool existed = std::filesystem::exists(path_);
    std::filesystem::create_directories(path_.parent_path()); const auto name = path_.u8string();
    try {
        sqlCheck(sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), db_);
        sqlite3_busy_timeout(db_, 3000); sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, 1048576);
        exec(db_, "PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF;");
        if (existed) {
            require(scalar(db_, "PRAGMA application_id") == ApplicationId && scalar(db_, "PRAGMA user_version") == 1, "Unknown adaptive ledger schema; file preserved");
            Statement check(db_, "PRAGMA quick_check"); require(check.step() && check.value(0) == "ok", "Corrupt adaptive ledger; file preserved");
        } else {
            Transaction tx(db_);
            exec(db_, R"sql(
CREATE TABLE protocols(digest TEXT PRIMARY KEY, definition TEXT NOT NULL);
CREATE TABLE lineages(id INTEGER PRIMARY KEY, protocol TEXT NOT NULL REFERENCES protocols(digest), source_digest TEXT NOT NULL,
 source_version INTEGER NOT NULL, created_utc TEXT NOT NULL, reason TEXT NOT NULL);
CREATE TABLE predictions(lineage INTEGER NOT NULL REFERENCES lineages(id), n INTEGER NOT NULL CHECK(n BETWEEN 1 AND 100000),
 prefix TEXT NOT NULL, state BLOB NOT NULL, prediction BLOB NOT NULL, checksum TEXT NOT NULL, created_utc TEXT NOT NULL,
 PRIMARY KEY(lineage,n), UNIQUE(lineage,prefix));
CREATE TABLE outcomes(lineage INTEGER NOT NULL, n INTEGER NOT NULL, revision TEXT NOT NULL, payload BLOB NOT NULL, checksum TEXT NOT NULL,
 provenance TEXT NOT NULL CHECK(provenance IN ('historical-replay','locally-pre-fetch')),
 PRIMARY KEY(lineage,n,revision), FOREIGN KEY(lineage,n) REFERENCES predictions(lineage,n));
CREATE TABLE head(singleton INTEGER PRIMARY KEY CHECK(singleton=1), lineage INTEGER NOT NULL, n INTEGER NOT NULL,
 FOREIGN KEY(lineage,n) REFERENCES predictions(lineage,n));
CREATE TABLE fetch_boundaries(id INTEGER PRIMARY KEY, lineage INTEGER NOT NULL, n INTEGER NOT NULL,
 attempt INTEGER NOT NULL, source_version INTEGER NOT NULL, snapshot_digest TEXT NOT NULL, created_utc TEXT NOT NULL,
 FOREIGN KEY(lineage,n) REFERENCES predictions(lineage,n));
CREATE TABLE fetch_seen(boundary INTEGER NOT NULL REFERENCES fetch_boundaries(id), draw_key TEXT NOT NULL,
 PRIMARY KEY(boundary,draw_key));
CREATE TABLE prefetch_annotations(lineage INTEGER NOT NULL, n INTEGER NOT NULL, revision TEXT NOT NULL,
 boundary INTEGER NOT NULL UNIQUE REFERENCES fetch_boundaries(id), first_retrieval TEXT NOT NULL, detail TEXT NOT NULL,
 PRIMARY KEY(lineage,n,revision), FOREIGN KEY(lineage,n,revision) REFERENCES outcomes(lineage,n,revision));
PRAGMA application_id=1295401284;
PRAGMA user_version=1;
)sql");
            for (const auto* table : {"protocols", "lineages", "predictions", "outcomes", "fetch_boundaries", "fetch_seen", "prefetch_annotations"}) {
                for (const auto* op : {"UPDATE", "DELETE"}) {
                    const std::string sql = std::string("CREATE TRIGGER immutable_")+table+"_"+op+" BEFORE "+op+" ON "+table+" BEGIN SELECT RAISE(ABORT,'Immutable adaptive record'); END;";
                    exec(db_, sql.c_str());
                }
            }
            tx.commit();
        }
        Statement foreign(db_, "PRAGMA foreign_key_check"); require(!foreign.step(), "Broken adaptive ledger references");
        (void)head(); // Validate the active checkpoint without repairing anything.
    } catch (...) { if (db_) sqlite3_close(db_); db_ = nullptr; throw; }
}
AdaptiveLedger::~AdaptiveLedger() { if (db_) sqlite3_close(db_); }
std::vector<AdaptiveLineage> AdaptiveLedger::lineages(size_t offset, size_t limit) const {
    page(offset, limit); Statement q(db_, "SELECT id,protocol,source_digest,source_version,created_utc,reason,(SELECT COUNT(*) FROM predictions WHERE lineage=lineages.id) FROM lineages ORDER BY id DESC LIMIT ? OFFSET ?");
    q.number(1, int64_t(limit)); q.number(2, int64_t(offset)); std::vector<AdaptiveLineage> out;
    while (q.step()) { auto item=lineage(q); item.predictionCount=size_t(q.number(6)); out.push_back(std::move(item)); } return out;
}
std::vector<AdaptiveStoredPrediction> AdaptiveLedger::predictions(int64_t id, size_t offset, size_t limit) const {
    page(offset, limit); Statement q(db_, "SELECT n,prefix,state,prediction,checksum,created_utc FROM predictions WHERE lineage=? ORDER BY n LIMIT ? OFFSET ?");
    q.number(1,id); q.number(2,int64_t(limit)); q.number(3,int64_t(offset)); std::vector<AdaptiveStoredPrediction> out;
    while (q.step()) {
        const auto stateBytes = q.value(2), forecastBytes = q.value(3);
        require(sha256(stateBytes+forecastBytes) == q.value(4), "Corrupt adaptive prediction checksum");
        AdaptiveStoredPrediction saved; saved.lineage = id; saved.createdUtc = q.value(5); timestamp(saved.createdUtc);
        saved.before = decodeAdaptiveState(stateBytes); saved.prediction = predictAdaptive(saved.before);
        require(int64_t(saved.before.draws) == q.number(0) && saved.before.prefixDigest == q.value(1) &&
            predictionBytes(saved.prediction) == forecastBytes, "Inconsistent adaptive prediction");
        Statement scoreRow(db_, "SELECT revision,payload,checksum,provenance FROM outcomes WHERE lineage=? AND n=? ORDER BY rowid DESC");
        scoreRow.number(1,id); scoreRow.number(2,q.number(0));
        if (scoreRow.step()) {
            const auto bytes = scoreRow.value(1); require(sha256(bytes) == scoreRow.value(2), "Corrupt adaptive outcome checksum");
            Reader r(bytes); require(r.text() == "adaptive-outcome-v1", "Unknown adaptive outcome version");
            const auto actual = result(r); auto state = saved.before; auto step = observeAdaptive(state, actual);
            saved.outcomeRevision = scoreRow.value(0);
            require(revisionDigest(actual) == saved.outcomeRevision && outcomeBytes(step) == bytes, "Inconsistent adaptive score attachment");
            saved.outcome = std::move(step); saved.provenance = scoreRow.value(3);
            Statement annotation(db_, "SELECT detail FROM prefetch_annotations WHERE lineage=? AND n=? AND revision=?");
            annotation.number(1,id); annotation.number(2,q.number(0)); annotation.text(3,saved.outcomeRevision);
            if (annotation.step()) { saved.provenance = "locally-pre-fetch"; saved.provenanceDetail = annotation.value(0); }
            else saved.provenanceDetail = "Historical replay; no qualifying durable pre-fetch evidence.";
        } else { saved.provenance = "pending-unbound"; saved.provenanceDetail = "No unambiguous target has been bound; unscored."; }
        out.push_back(std::move(saved));
    }
    return out;
}
std::optional<AdaptiveHead> AdaptiveLedger::head() const {
    Statement q(db_, "SELECT l.id,l.protocol,l.source_digest,l.source_version,l.created_utc,l.reason,h.n FROM head h JOIN lineages l ON l.id=h.lineage WHERE h.singleton=1");
    if (!q.step()) return {};
    AdaptiveHead out; out.lineage = lineage(q); const auto n = q.number(6);
    require(n >= 1 && n <= 100000, "Invalid adaptive head count");
    const auto records = predictions(out.lineage.id, size_t(n-1), 1);
    require(records.size() == 1 && records.front().before.draws == size_t(n) && !records.front().outcome, "Missing or scored adaptive head checkpoint");
    out.state = records.front().before; return out;
}
int64_t AdaptiveLedger::publishReplay(const AdaptiveReplay& replay, uint64_t sourceVersion,
    const std::string& createdUtc, const std::string& reason, int64_t expectedHead, std::stop_token stop, const std::string& expectedPrefix) {
    require(replay.available && replay.next && replay.protocolDigest == adaptiveProtocolDigest(), "Unavailable or incompatible adaptive replay");
    validateAdaptiveState(replay.state); require(replay.steps.size() == replay.state.draws-1, "Incomplete adaptive replay");
    digest(replay.snapshotDigest); timestamp(createdUtc);
    require(!reason.empty() && reason.size() <= 2048 && reason.find_first_of("\r\n") == std::string::npos, "Invalid adaptive lineage reason");
    require(sourceVersion <= uint64_t(std::numeric_limits<int64_t>::max()), "Adaptive source version limit");
    const auto cancel = [&] { if (stop.stop_requested()) throw std::runtime_error("Adaptive ledger publication cancelled"); };
    cancel(); Transaction tx(db_); const auto current = head();
    require((current ? current->lineage.id : 0) == expectedHead, "Adaptive head changed; reconcile again");
    require(expectedPrefix.empty() || (current && current->state.prefixDigest == expectedPrefix), "Adaptive prefix changed; reconcile again");
    Statement protocol(db_, "INSERT OR IGNORE INTO protocols VALUES(?,?)"); protocol.text(1,adaptiveProtocolDigest()); protocol.text(2,std::string(AdaptiveProtocol)); protocol.step();
    Statement definition(db_, "SELECT definition FROM protocols WHERE digest=?"); definition.text(1,adaptiveProtocolDigest());
    require(definition.step() && definition.value(0) == AdaptiveProtocol, "Corrupt adaptive protocol definition");
    Statement insert(db_, "INSERT INTO lineages(protocol,source_digest,source_version,created_utc,reason) VALUES(?,?,?,?,?)");
    insert.text(1,replay.protocolDigest); insert.text(2,replay.snapshotDigest); insert.number(3,int64_t(sourceVersion)); insert.text(4,createdUtc); insert.text(5,reason); insert.step();
    const int64_t id = sqlite3_last_insert_rowid(db_);
    auto state = replay.steps.empty() ? replay.state : replay.steps.front().before;
    require(state.draws == 1 && state.lastDate == "2002-07-04" && state.lastId == "02/053", "Invalid adaptive replay origin");
    for (size_t i = 0; i <= replay.steps.size(); ++i) {
        cancel(); const auto p = predictAdaptive(state); const auto stateBytes = encodeAdaptiveState(state), pBytes = predictionBytes(p);
        Statement save(db_, "INSERT INTO predictions VALUES(?,?,?,?,?,?,?)"); save.number(1,id); save.number(2,int64_t(state.draws));
        save.text(3,state.prefixDigest); save.blob(4,stateBytes); save.blob(5,pBytes); save.text(6,sha256(stateBytes+pBytes)); save.text(7,createdUtc); save.step();
        if (i == replay.steps.size()) break;
        const auto& expected = replay.steps[i];
        require(stateBytes == encodeAdaptiveState(expected.before) && pBytes == predictionBytes(expected.prediction), "Broken adaptive replay chain");
        const auto step = observeAdaptive(state, expected.actual); const auto bytes = outcomeBytes(step);
        require(bytes == outcomeBytes(expected), "Inconsistent adaptive replay outcome");
        Statement observation(db_, "INSERT INTO outcomes VALUES(?,?,?,?,?,'historical-replay')"); observation.number(1,id); observation.number(2,int64_t(step.before.draws));
        observation.text(3,revisionDigest(step.actual)); observation.blob(4,bytes); observation.text(5,sha256(bytes)); observation.step();
    }
    require(encodeAdaptiveState(state) == encodeAdaptiveState(replay.state) && predictionBytes(predictAdaptive(state)) == predictionBytes(*replay.next), "Adaptive final checkpoint mismatch");
    Statement activate(db_, "INSERT INTO head VALUES(1,?,?) ON CONFLICT(singleton) DO UPDATE SET lineage=excluded.lineage,n=excluded.n");
    activate.number(1,id); activate.number(2,int64_t(state.draws)); activate.step(); cancel(); tx.commit(); return id;
}
AdaptiveReconcileResult AdaptiveLedger::reconcileAppend(const Snapshot& snapshot, uint64_t sourceVersion,
    const std::string& createdUtc, std::stop_token stop, const std::function<void(size_t)>& progress) {
    AdaptiveReconcileResult out; out.snapshotDigest = snapshot.digest(); out.sourceVersion = sourceVersion;
    timestamp(createdUtc);
    const auto cancel = [&] { if (stop.stop_requested()) throw std::runtime_error("Adaptive reconciliation cancelled"); };
    cancel(); const auto rows = snapshot.select();
    if (rows.empty() || rows.front().date != "2002-07-04" || rows.front().id != "02/053") {
        out.detail = "Verified seed 02/053 (2002-07-04) unavailable."; return out;
    }
    std::vector<std::string> prefixes; prefixes.reserve(rows.size());
    std::string prefix = adaptiveProtocolDigest();
    for (size_t i = 0; i < rows.size(); ++i) {
        cancel();
        require(i == 0 || rows[i].date > rows[i-1].date, "Ambiguous adaptive input ordering");
        prefix = adaptiveValuePrefix(prefix,rows[i]); prefixes.push_back(prefix);
    }
    // Serialize all head checks and writes. The source snapshot is immutable;
    // coordinator still must check source currentness before displaying this result.
    Transaction tx(db_); const auto saved = head();
    if (!saved) {
        out.status = AdaptiveReconcileStatus::NeedsReplay;
        out.detail = "Initial replay required."; return out;
    }
    const size_t n = saved->state.draws;
    if (saved->lineage.protocol != adaptiveProtocolDigest() || n > rows.size() || prefixes[n-1] != saved->state.prefixDigest) {
        out.status = AdaptiveReconcileStatus::NeedsReplay;
        out.detail = "Training prefix or protocol changed; retained forecast is not current."; return out;
    }
    if (n == rows.size()) {
        cancel(); out.status = AdaptiveReconcileStatus::Current; out.current = saved;
        out.detail = "No new eligible draws; learned zero draws."; return out;
    }
    // A next-draw claim cannot bridge a missing published identity or year boundary
    // without further source-supported ordering evidence. Replay may still learn it.
    const auto& target = rows[n];
    if (target.date.substr(0,4) != saved->state.lastDate.substr(0,4) ||
        std::stoi(target.id.substr(3)) != std::stoi(saved->state.lastId.substr(3))+1) {
        out.status = AdaptiveReconcileStatus::NeedsReplay;
        out.detail = "Uncertain pending target; retained pending forecast stays unscored."; return out;
    }
    auto state = saved->state;
    for (size_t i = n; i < rows.size(); ++i) {
        cancel(); const auto step = observeAdaptive(state,rows[i]); ++out.processed;
        require(state.prefixDigest == prefixes[i], "Adaptive appended prefix mismatch");
        if (progress) progress(out.processed); cancel();
        const auto bytes = outcomeBytes(step);
        Statement observation(db_, "INSERT INTO outcomes VALUES(?,?,?,?,?,'historical-replay')");
        observation.number(1,saved->lineage.id); observation.number(2,int64_t(step.before.draws));
        observation.text(3,revisionDigest(step.actual)); observation.blob(4,bytes); observation.text(5,sha256(bytes)); observation.step();
        const auto stateBytes = encodeAdaptiveState(state), pBytes = predictionBytes(predictAdaptive(state));
        Statement prediction(db_, "INSERT INTO predictions VALUES(?,?,?,?,?,?,?)");
        prediction.number(1,saved->lineage.id); prediction.number(2,int64_t(state.draws)); prediction.text(3,state.prefixDigest);
        prediction.blob(4,stateBytes); prediction.blob(5,pBytes); prediction.text(6,sha256(stateBytes+pBytes)); prediction.text(7,createdUtc); prediction.step();
    }
    Statement activate(db_, "UPDATE head SET n=? WHERE singleton=1 AND lineage=? AND n=?");
    activate.number(1,int64_t(state.draws)); activate.number(2,saved->lineage.id); activate.number(3,int64_t(n)); activate.step();
    require(sqlite3_changes(db_) == 1, "Adaptive checkpoint changed during append");
    cancel(); tx.commit(); // Commit wins over a subsequent cancellation.
    out.status = AdaptiveReconcileStatus::Current; out.current = AdaptiveHead{saved->lineage,std::move(state)};
    out.detail = "Appended eligible draws; historical replay provenance until first-retrieval binding is verified.";
    return out;
}
AdaptiveReconcileResult AdaptiveLedger::reconcile(const Snapshot& snapshot, uint64_t sourceVersion,
    const std::string& createdUtc, std::stop_token stop, const std::function<void(size_t)>& progress) {
    auto out = reconcileAppend(snapshot,sourceVersion,createdUtc,stop,progress);
    if (out.status != AdaptiveReconcileStatus::NeedsReplay) return out;
    const auto previous = head();
    const auto replay = replayAdaptive(snapshot,stop);
    if (!replay.available) { out.status = AdaptiveReconcileStatus::Unavailable; out.detail = replay.unavailableReason; return out; }
    const std::string reason = !previous ? "Initial verified-seed replay" :
        previous->lineage.protocol != adaptiveProtocolDigest() ? "Protocol changed; separate replay lineage" :
        "Eligible history changed (correction/insertion/deletion/eligibility); separate replay lineage";
    if (progress) progress(replay.steps.size());
    const auto id = publishReplay(replay,sourceVersion,createdUtc,reason,previous ? previous->lineage.id : 0,stop,
                                  previous ? previous->state.prefixDigest : std::string{});
    AdaptiveLineage created{id,replay.protocolDigest,snapshot.digest(),createdUtc,reason,sourceVersion};
    out.status = AdaptiveReconcileStatus::Current; out.current = AdaptiveHead{std::move(created),replay.state};
    out.processed = replay.steps.size(); out.detail = reason; return out;
}
int64_t AdaptiveLedger::prepareFetch(const ArchiveState& before, int64_t attempt, const std::string& createdUtc) {
    timestamp(createdUtc);
    const auto job = std::find_if(before.attempts.begin(),before.attempts.end(),[&](const auto& a) { return a.id == attempt; });
    require(job != before.attempts.end() && job->status == "pending" && job->baseVersion == before.version,
            "Fetch boundary requires the current pending archive attempt");
    require(job->startedUtc.substr(0,19) <= createdUtc.substr(0,19), "Fetch boundary clock precedes attempt");
    require(before.version <= uint64_t(std::numeric_limits<int64_t>::max()) && before.revisions.size() <= 1000000,
            "Fetch boundary input limit");
    Transaction tx(db_); const auto saved = head(); if (!saved) return 0;
    const auto rows = before.snapshot.select();
    if (rows.size() != saved->state.draws || saved->lineage.protocol != adaptiveProtocolDigest()) return 0;
    std::string prefix = adaptiveProtocolDigest();
    for (const auto& r : rows) prefix = adaptiveValuePrefix(prefix,r);
    if (prefix != saved->state.prefixDigest) return 0;
    Statement insert(db_, "INSERT INTO fetch_boundaries(lineage,n,attempt,source_version,snapshot_digest,created_utc) VALUES(?,?,?,?,?,?)");
    insert.number(1,saved->lineage.id); insert.number(2,int64_t(saved->state.draws)); insert.number(3,attempt);
    insert.number(4,int64_t(before.version)); insert.text(5,before.snapshot.digest()); insert.text(6,createdUtc); insert.step();
    const auto id = sqlite3_last_insert_rowid(db_);
    // Any previously seen identity counts, including excluded/imported revisions
    // and revisions whose date may later be corrected across the cutoff.
    for (const auto& revision : before.revisions) {
        Statement seen(db_, "INSERT OR IGNORE INTO fetch_seen VALUES(?,?)"); seen.number(1,id); seen.text(2,drawKey(revision.result)); seen.step();
    }
    tx.commit(); return id;
}
AdaptiveReconcileResult AdaptiveLedger::reconcileFetched(const ArchiveState& after, int64_t boundary,
    const std::string& createdUtc, std::stop_token stop) {
    auto out = reconcile(after.snapshot,after.version,createdUtc,stop);
    if (!boundary || !out.current) return out;
    if (stop.stop_requested()) throw std::runtime_error("Adaptive provenance cancelled; model checkpoint retained");
    Transaction tx(db_);
    Statement q(db_, "SELECT lineage,n,attempt,source_version,created_utc FROM fetch_boundaries WHERE id=?"); q.number(1,boundary);
    require(q.step(), "Unknown adaptive fetch boundary");
    const auto id = q.number(0), n = q.number(1), attempt = q.number(2), version = q.number(3); const auto started = q.value(4);
    timestamp(started);
    if (id != out.current->lineage.id || n < 1 || size_t(n) >= out.current->state.draws || version < 0 || after.version != uint64_t(version)+1) return out;
    const auto job = std::find_if(after.attempts.begin(),after.attempts.end(),[&](const auto& a) { return a.id == attempt; });
    if (job == after.attempts.end() || job->status != "succeeded" || job->baseVersion != uint64_t(version) ||
        job->finishedUtc.empty() || job->finishedUtc.substr(0,19) > createdUtc.substr(0,19)) return out;
    const auto saved = predictions(id,size_t(n-1),1);
    if (saved.size() != 1 || !saved.front().outcome) return out;
    const auto& record = saved.front(); const auto& actual = record.outcome->actual;
    if (actual.source.trust != Trust::OfficialRetrieval || actual.source.sourceId != job->sourceId) return out;
    Statement seen(db_, "SELECT 1 FROM fetch_seen WHERE boundary=? AND draw_key=?"); seen.number(1,boundary); seen.text(2,drawKey(actual));
    if (seen.step()) return out;
    // Truncate valid canonical UTC values to whole seconds: equal-second evidence
    // does not qualify. A later re-fetch must not erase an earlier local revision.
    std::string first; bool foundRevision = false;
    for (const auto& revision : after.revisions) if (drawKey(revision.result) == drawKey(actual)) {
        const auto r = canonicalize(revision.result);
        if (first.empty() || r.source.retrievedUtc.substr(0,19) < first.substr(0,19)) first = r.source.retrievedUtc;
        if (revision.digest == record.outcomeRevision) foundRevision = true;
    }
    if (!foundRevision || first.empty() || first.substr(0,19) <= started.substr(0,19) || record.createdUtc > started ||
        first.substr(0,19) > createdUtc.substr(0,19) || first.substr(0,19) > job->finishedUtc.substr(0,19)) return out;
    const std::string detail = "Saved before local fetch boundary; first archive retrieval " + first +
        ". Not certified before the real-world draw; local timestamps are not trusted certification.";
    Statement add(db_, "INSERT OR IGNORE INTO prefetch_annotations VALUES(?,?,?,?,?,?)");
    add.number(1,id); add.number(2,n); add.text(3,record.outcomeRevision); add.number(4,boundary); add.text(5,first); add.text(6,detail); add.step();
    if (stop.stop_requested()) throw std::runtime_error("Adaptive provenance cancelled; model checkpoint retained");
    tx.commit(); return out;
}
int64_t AdaptiveLedger::pendingFetchBoundary(const ArchiveState& after) const {
    // Resume a committed fetch even if the UI was hidden/closed before learning.
    for (const auto& attempt : after.attempts) if (attempt.status == "succeeded" && after.version == attempt.baseVersion+1) {
        Statement q(db_, "SELECT id FROM fetch_boundaries WHERE attempt=? AND source_version=? ORDER BY id DESC LIMIT 1");
        q.number(1,attempt.id); q.number(2,int64_t(attempt.baseVersion)); if (q.step()) return q.number(0);
    }
    return 0;
}
}
