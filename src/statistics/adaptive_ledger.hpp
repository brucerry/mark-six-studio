#pragma once
#include "adaptive.hpp"
#include <filesystem>
#include <functional>
struct sqlite3;

namespace marksix::statistics {
struct ArchiveState;
// Explicit little-endian, bounded, lossless checkpoint format; not native struct bytes.
std::string encodeAdaptiveState(const AdaptiveState& state);
AdaptiveState decodeAdaptiveState(std::string_view bytes);
struct AdaptiveLineage {
    int64_t id = 0;
    std::string protocol, sourceDigest, createdUtc, reason;
    uint64_t sourceVersion = 0;
    size_t predictionCount = 0;
};
struct AdaptiveStoredPrediction {
    int64_t lineage = 0;
    std::string createdUtc;
    AdaptiveState before;
    AdaptivePrediction prediction;
    std::optional<AdaptiveStep> outcome;
    std::string outcomeRevision;
    // Historical publication does not manufacture local pre-fetch provenance.
    std::string provenance = "historical-replay";
    std::string provenanceDetail;
};
struct AdaptiveHead {
    AdaptiveLineage lineage;
    AdaptiveState state;
};
enum class AdaptiveReconcileStatus { Current, Unavailable, NeedsReplay };
struct AdaptiveReconcileResult {
    AdaptiveReconcileStatus status = AdaptiveReconcileStatus::Unavailable;
    size_t processed = 0;
    std::string snapshotDigest, detail;
    uint64_t sourceVersion = 0;
    std::optional<AdaptiveHead> current;
};
// Single-owner connection. Only writes userRoot/statistics/adaptive.sqlite3.
// No network, source-database mutation or automatic corruption recovery.
class AdaptiveLedger {
public:
    explicit AdaptiveLedger(const std::filesystem::path& userRoot);
    ~AdaptiveLedger();
    AdaptiveLedger(const AdaptiveLedger&) = delete;
    AdaptiveLedger& operator=(const AdaptiveLedger&) = delete;
    const std::filesystem::path& path() const { return path_; }
    std::optional<AdaptiveHead> head() const;
    std::vector<AdaptiveLineage> lineages(size_t offset = 0, size_t limit = 100) const;
    std::vector<AdaptiveStoredPrediction> predictions(int64_t lineage, size_t offset = 0, size_t limit = 100) const;
    // Atomic initial/revised replay, including the unscored next prediction.
    // expectedHead=0 for empty ledger. A stale caller cannot activate its replay.
    int64_t publishReplay(const AdaptiveReplay& replay, uint64_t sourceVersion,
                          const std::string& createdUtc, const std::string& reason,
                          int64_t expectedHead, std::stop_token stop = {}, const std::string& expectedPrefix = {});
    // Reports progress before publication; exceptions/cancellation roll back the batch.
    // Changed prefixes return NeedsReplay, never a stale current forecast.
    AdaptiveReconcileResult reconcileAppend(const Snapshot& snapshot, uint64_t sourceVersion,
        const std::string& createdUtc, std::stop_token stop = {},
        const std::function<void(size_t)>& progress = {});
    // Initial/revised histories replay into a new lineage; old evidence is retained.
    AdaptiveReconcileResult reconcile(const Snapshot& snapshot, uint64_t sourceVersion,
        const std::string& createdUtc, std::stop_token stop = {},
        const std::function<void(size_t)>& progress = {});
    // Call after archive.startUpdate, but BEFORE any source network request.
    // Returns zero if no compatible durable pending forecast exists.
    int64_t prepareFetch(const ArchiveState& before, int64_t attempt, const std::string& createdUtc);
    int64_t pendingFetchBoundary(const ArchiveState& after) const;
    // Consumes a coherent accepted archive state, not user-supplied timestamps.
    // A qualifying annotation is append-only and never changes saved probabilities.
    AdaptiveReconcileResult reconcileFetched(const ArchiveState& after, int64_t boundary,
        const std::string& createdUtc, std::stop_token stop = {});
private:
    sqlite3* db_ = nullptr;
    std::filesystem::path path_;
};
}
