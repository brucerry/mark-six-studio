#pragma once
#include "archive.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace marksix::statistics {
enum class UpdateStage { Idle, Checking, Comparing, Downloading, Validating, Saving, Succeeded, Failed, Cancelled, Disabled };
enum class UpdateFailure { None, Connection, Tls, Denied, RateLimited, Schema, Storage, Cancelled, Disabled };
class ProviderFailure : public std::runtime_error {
public:
    explicit ProviderFailure(UpdateFailure failure) : std::runtime_error("History source request failed"), category(failure) {}
    UpdateFailure category;
};
// Internal, audited implementations only. No runtime plugin, URL or trust override.
// The desktop adapter supplies the reviewed local provider explicitly.
class UpdateProvider {
public:
    virtual ~UpdateProvider() = default;
    virtual bool enabled() const = 0;
    virtual UpdatePolicy policy() const = 0;
    // Successful bounded source HTTPS check, reusing its latest-result response.
    // Null means a schema-validated empty source, never a null/error HTTP response.
    virtual std::optional<Result> latest(std::stop_token stop) = 0;
    virtual FetchedWindow fetch(const DateRange& range, std::stop_token stop) = 0;
};
struct UpdateProgress {
    uint64_t generation = 0;
    UpdateStage stage = UpdateStage::Idle;
    size_t completed = 0, total = 0;
};
struct UpdateResult {
    uint64_t generation = 0;
    int64_t attempt = 0;
    UpdateStage stage = UpdateStage::Failed;
    UpdateFailure failure = UpdateFailure::None;
    std::optional<UpdateOutcome> committed;
    bool attemptStatusSaved = true;
    std::string adaptiveNotice;
};
using UpdateProgressSink = std::function<void(const UpdateProgress&)>;
// Internal diagnostics/test observer; invoked on the worker, never under its lock.
using UpdateObserver = std::function<void(const UpdateProgress&, std::stop_token)>;
using BeforeSourceFetch = std::function<void(const ArchiveState&, int64_t, std::stop_token)>;
UpdateResult executeUpdate(const std::filesystem::path& root, UpdateProvider* provider,
                           const UpdateProgressSink& progress = {}, std::stop_token stop = {}, const BeforeSourceFetch& beforeFetch = {});
std::string updateProgressText(const UpdateProgress& progress);
std::string updateResultText(const UpdateResult& result);
std::string updateHistoryText(const std::vector<UpdateAttempt>& attempts);
inline constexpr const char* UpdateDisabledNotice =
    "Update history unavailable: no reviewed provider is enabled. Cached analysis works offline.";

// A mutating job is not a LatestWorker job: cancellation must not discard a
// commit-winning outcome. One active update; no queued/automatic startup fetch.
class UpdateWorker {
public:
    explicit UpdateWorker(std::shared_ptr<UpdateProvider> provider = {}, UpdateObserver observer = {}, BeforeSourceFetch beforeFetch = {});
    ~UpdateWorker();
    UpdateWorker(const UpdateWorker&) = delete;
    bool available() const;
    bool start(const std::filesystem::path& root);
    void cancel();
    void close();
    bool busy() const;
    UpdateProgress progress() const;
    std::optional<UpdateResult> takeResult();
private:
    std::shared_ptr<UpdateProvider> provider_;
    UpdateObserver observer_;
    BeforeSourceFetch beforeFetch_;
    mutable std::mutex mutex_;
    UpdateProgress progress_;
    std::optional<UpdateResult> result_;
    std::stop_source stop_;
    bool busy_ = false, closed_ = false;
    std::jthread thread_;
};
}
