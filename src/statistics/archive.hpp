#pragma once
#include "domain.hpp"
#include "update_plan.hpp"
#include <filesystem>
#include <stop_token>
struct sqlite3;

namespace marksix::statistics {
struct ImportOutcome { size_t inserted = 0, duplicate = 0, conflicts = 0; uint64_t version = 0; };
struct StoredRevision { std::string digest; Result result; bool active = false; };
struct Resolution { std::string drawKey, selectedDigest, reason, evidence, decidedUtc; };
// Internal provider boundary, never accepted from the external import format.
// A window means a complete, successfully parsed response, including valid empty.
struct FetchedWindow { DateRange range; std::string responseSha256; std::vector<Result> rows; };
struct UpdateAttempt {
    int64_t id = 0;
    std::string sourceId, policyVersion, startedUtc, finishedUtc, status, detail;
    uint64_t baseVersion = 0;
    size_t added = 0, unchanged = 0, needsVerification = 0, conflicts = 0;
};
struct UpdateOutcome { UpdateComparison comparison; ImportOutcome ingestion; };
struct ArchiveState {
    Snapshot snapshot; std::vector<StoredRevision> revisions; std::vector<UpdateAttempt> attempts;
    std::vector<Resolution> resolutions; uint64_t version = 0;
};
// One connection per worker. A caller must not share an Archive across threads.
// Only writes userRoot/statistics/results.sqlite3 (+ SQLite journal / upgrade backups).
class Archive {
public:
    explicit Archive(const std::filesystem::path& userRoot);
    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    Snapshot snapshot() const;
    ArchiveState readState() const; // One SQLite read snapshot across view inputs.
    std::vector<StoredRevision> revisions() const;
    std::vector<Resolution> resolutions() const;
    uint64_t version() const;
    // Internal trusted ingestion; external CSV parsing must not assign verified status.
    ImportOutcome ingest(std::vector<Result> rows, std::stop_token stop = {}, std::optional<uint64_t> expectedVersion = {});
    // Persists a planned attempt, not fetched coverage. Plan and version must be current.
    int64_t beginUpdate(const UpdatePlan& plan, uint64_t expectedVersion, std::stop_token stop = {});
    // Two-stage lifecycle: persist the attempt before connectivity, attach its plan
    // once the latest-result check succeeds. Neither step grants fetched coverage.
    int64_t startUpdate(const std::string& sourceId, const std::string& policyVersion, std::stop_token stop = {});
    void setUpdatePlan(int64_t attempt, const UpdatePlan& plan, std::stop_token stop = {});
    UpdateOutcome commitUpdate(int64_t attempt, const std::vector<FetchedWindow>& windows, std::stop_token stop = {});
    // Returns false if already terminal (notably: a completed commit cannot be cancelled).
    bool finishUpdate(int64_t attempt, bool cancelled, const std::string& detail);
    std::vector<UpdateAttempt> updateAttempts() const;
    std::vector<CommittedWindow> committedWindows() const;
    void resolve(const std::string& drawKey, const std::string& revisionDigest,
                 const std::string& reason, const std::string& evidence,
                 std::stop_token stop = {}, std::optional<uint64_t> expectedVersion = {});
    const std::filesystem::path& path() const { return path_; }
private:
    sqlite3* db_ = nullptr;
    std::filesystem::path path_;
    void versionChanged();
    ImportOutcome ingestInTransaction(std::vector<Result> rows, std::stop_token stop);
};
std::string drawKey(const Result& result);
}
