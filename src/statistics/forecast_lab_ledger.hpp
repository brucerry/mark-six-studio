#pragma once
#include "forecast_lab.hpp"
#include <filesystem>
struct sqlite3;

namespace marksix::statistics {
struct ArchiveState;
struct LabLineage {
    int64_t id = 0;
    std::string protocol, sourceDigest, createdUtc, reason;
    uint64_t sourceVersion = 0;
    size_t completedDraws = 0;
};
struct LabStoredForecast {
    int64_t lineage = 0;
    LabForecast forecast;
    std::string createdUtc;
    std::optional<Result> outcome;
    std::string outcomeRevision;
    std::string provenance = "historical-replay";
};
struct LabReconcile {
    bool available = false, reused = false, revised = false;
    size_t added = 0;
    std::string error;
    LabLineage lineage;
    LabReplay replay; // Latest checkpoint and newly processed rows only.
};
// An independent Forecast Lab store. Never opens predecessor Aggressive files.
class LabLedger {
public:
    explicit LabLedger(const std::filesystem::path& root);
    ~LabLedger();
    LabLedger(const LabLedger&) = delete;
    LabLedger& operator=(const LabLedger&) = delete;
    const std::filesystem::path& path() const { return path_; }
    std::optional<LabLineage> head() const;
    std::vector<LabLineage> lineages(size_t offset = 0, size_t limit = 100) const;
    std::vector<LabStoredForecast> forecasts(int64_t lineage, size_t offset = 0,
                                              size_t limit = 100) const;
    LabEvaluation evaluation(int64_t lineage, std::stop_token stop = {}) const;
    LabReconcile reconcile(const Snapshot& snapshot, uint64_t sourceVersion,
                           const std::string& createdUtc, std::stop_token stop = {});
    int64_t prepareFetch(const ArchiveState& before, int64_t attempt,
                         const std::string& createdUtc);
    void annotateFetched(const ArchiveState& after, const std::string& createdUtc);
    void saveControl(const LabControlReport& report);
    std::optional<LabControlReport> control(const std::string& sourceDigest,
                                           size_t historyLength) const;
private:
    sqlite3* db_ = nullptr;
    std::filesystem::path path_;
};
}
