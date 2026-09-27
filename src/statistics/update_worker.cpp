#include "update_worker.hpp"
#include <algorithm>
#include <map>

namespace marksix::statistics {
namespace {
void cancelled(std::stop_token stop) { if (stop.stop_requested()) throw ProviderFailure(UpdateFailure::Cancelled); }
const char* failureText(UpdateFailure failure) {
    switch (failure) {
    case UpdateFailure::Connection: return "Source connection failed (network, DNS or timeout).";
    case UpdateFailure::Tls: return "Source TLS/certificate validation failed; not an offline diagnosis.";
    case UpdateFailure::Denied: return "Source access denied; no bypass attempted.";
    case UpdateFailure::RateLimited: return "Source rate limit reached; try later.";
    case UpdateFailure::Schema: return "Source response or staged update failed validation.";
    case UpdateFailure::Storage: return "Archive operation failed or the dataset changed; reload and retry.";
    case UpdateFailure::Cancelled: return "Update cancelled before commit. Accepted results and fetched ranges are unchanged.";
    case UpdateFailure::Disabled: return UpdateDisabledNotice;
    default: return "History update failed.";
    }
}
}
UpdateResult executeUpdate(const std::filesystem::path& root, UpdateProvider* provider,
                           const UpdateProgressSink& progress, std::stop_token stop, const BeforeSourceFetch& beforeFetch) {
    UpdateResult result; UpdateProgress state; UpdateFailure failureContext = UpdateFailure::Storage;
    auto emit = [&](UpdateStage stage, size_t completed = 0, size_t total = 0) {
        state.stage = stage; state.completed = completed; state.total = total;
        // A presentation callback cannot turn a completed DB commit into a failure.
        if (progress) try { progress(state); } catch (...) {}
    };
    std::unique_ptr<Archive> archive;
    try {
        if (!provider || !provider->enabled()) {
            result.stage = UpdateStage::Disabled; result.failure = UpdateFailure::Disabled; emit(result.stage); return result;
        }
        cancelled(stop); const auto policy = provider->policy();
        archive = std::make_unique<Archive>(root);
        result.attempt = archive->startUpdate(policy.sourceId, policy.version, stop);
        if (beforeFetch) try { beforeFetch(archive->readState(),result.attempt,stop); }
        catch (...) { result.adaptiveNotice = " Adaptive pre-fetch evidence unavailable; history update remains independent."; }
        emit(UpdateStage::Checking); cancelled(stop);
        failureContext = UpdateFailure::Schema;
        const auto latest = provider->latest(stop); cancelled(stop);
        emit(UpdateStage::Comparing); cancelled(stop);
        failureContext = UpdateFailure::Storage;
        const auto snapshot = archive->snapshot();
        const auto receipts = archive->committedWindows(); failureContext = UpdateFailure::Schema;
        const auto plan = planUpdate(policy, snapshot, receipts, latest, stop);
        failureContext = UpdateFailure::Storage;
        archive->setUpdatePlan(result.attempt, plan, stop);
        std::vector<FetchedWindow> windows; size_t rows = 0;
        failureContext = UpdateFailure::Schema;
        emit(UpdateStage::Downloading, 0, plan.requests.size());
        for (const auto& range : plan.requests) {
            cancelled(stop); auto window = provider->fetch(range, stop); cancelled(stop);
            if (window.range != range || window.rows.size() > 100000 - rows) throw ProviderFailure(UpdateFailure::Schema);
            rows += window.rows.size(); windows.push_back(std::move(window));
            emit(UpdateStage::Downloading, windows.size(), plan.requests.size());
        }
        emit(UpdateStage::Validating, windows.size(), windows.size()); cancelled(stop);
        std::vector<Result> staged; staged.reserve(rows);
        for (const auto& window : windows) {
            cancelled(stop);
            if (window.responseSha256.size() != 64 || window.responseSha256.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw ProviderFailure(UpdateFailure::Schema);
            for (const auto& row : window.rows) {
                cancelled(stop);
                if (row.source.sourceId != policy.sourceId || row.source.contentSha256 != window.responseSha256 ||
                    row.date < window.range.from || row.date > window.range.through) throw ProviderFailure(UpdateFailure::Schema);
                staged.push_back(row);
            }
        }
        compareUpdate(snapshot, staged, stop);
        // A latest draw in a requested range must not disappear in its window response.
        // No guessed expected inventory is inferred for other dates.
        if (latest && std::any_of(plan.requests.begin(), plan.requests.end(), [&](const DateRange& r) { return latest->date >= r.from && latest->date <= r.through; })) {
            const auto comparison = compareUpdate(Snapshot(staged), {*latest}, stop);
            if (comparison.unchanged != 1) throw ProviderFailure(UpdateFailure::Schema);
        }
        emit(UpdateStage::Saving, windows.size(), windows.size()); cancelled(stop);
        failureContext = UpdateFailure::Storage;
        result.committed = archive->commitUpdate(result.attempt, windows, stop);
        result.stage = UpdateStage::Succeeded; emit(result.stage); return result;
    } catch (const ProviderFailure& failure) {
        result.failure = failure.category;
    } catch (...) {
        result.failure = failureContext;
    }
    if (stop.stop_requested()) result.failure = UpdateFailure::Cancelled;
    result.stage = result.failure == UpdateFailure::Cancelled ? UpdateStage::Cancelled : UpdateStage::Failed;
    if (archive && result.attempt) {
        try { archive->finishUpdate(result.attempt, result.stage == UpdateStage::Cancelled, failureText(result.failure)); }
        catch (...) { result.attemptStatusSaved = false; }
    }
    emit(result.stage); return result;
}
std::string updateProgressText(const UpdateProgress& p) {
    switch (p.stage) {
    case UpdateStage::Checking: return "Checking source connection... Cached analysis remains available.";
    case UpdateStage::Comparing: return "Checking for new records and unfetched ranges...";
    case UpdateStage::Downloading: return "Downloading history windows: " + std::to_string(p.completed) + " / " + std::to_string(p.total);
    case UpdateStage::Validating: return "Validating staged results and source provenance...";
    case UpdateStage::Saving: return "Saving results and fetched ranges atomically...";
    case UpdateStage::Disabled: return UpdateDisabledNotice;
    case UpdateStage::Cancelled: return "Update cancelled before commit.";
    case UpdateStage::Succeeded: return "Update committed. Reloading accepted data...";
    case UpdateStage::Failed: return "Update failed. Cached analysis remains available.";
    default: return "No update running. Updates start only when requested.";
    }
}
std::string updateResultText(const UpdateResult& r) {
    if (r.committed) {
        const auto& c = r.committed->comparison;
        return std::string(c.added ? "Update committed." : "No new records in checked ranges.") +
            " New " + std::to_string(c.added) + "; unchanged " + std::to_string(c.unchanged) +
            "; newly verified " + std::to_string(c.needsVerification) + "; changed/conflicting " + std::to_string(c.conflicts) +
            ". Older corrections outside checked ranges and archive completeness remain unknown." + r.adaptiveNotice;
    }
    return std::string(failureText(r.failure)) + (r.attemptStatusSaved ? "" : " Attempt status could not be saved; reload to inspect durable state.");
}
std::string updateHistoryText(const std::vector<UpdateAttempt>& attempts) {
    if (attempts.empty()) return "Update checks: never attempted. Last successful check: none. Imported/retrieved draw dates do not establish checked ranges.";
    // Keep different source/policy histories separate. IDs order attempts; UTC is
    // reported evidence, not used as a substitute for a monotonic sequence.
    std::map<std::pair<std::string,std::string>, std::pair<const UpdateAttempt*,const UpdateAttempt*>> groups;
    for (const auto& a : attempts) {
        auto& group = groups[{a.sourceId,a.policyVersion}];
        if (!group.first || a.id > group.first->id) group.first = &a;
        if (a.status == "succeeded" && (!group.second || a.id > group.second->id)) group.second = &a;
    }
    std::string out;
    for (const auto& [source, pair] : groups) {
        const auto& a = *pair.first;
        if (!out.empty()) out += "\r\n";
        out += source.first + " / " + source.second + ": last attempted " + a.startedUtc + " (" +
            (a.status == "pending" ? "unfinished; success not established" : a.status) + "). Last successful check: " +
            (pair.second ? pair.second->finishedUtc : "none") + ".";
        if (!a.detail.empty()) out += " " + a.detail;
        if (pair.second) {
            const auto& s = *pair.second;
            out += " Last success counts: new " + std::to_string(s.added) + ", unchanged " + std::to_string(s.unchanged) +
                ", newly verified " + std::to_string(s.needsVerification) + ", changed/conflicting " + std::to_string(s.conflicts) + ".";
        }
    }
    return out;
}
UpdateWorker::UpdateWorker(std::shared_ptr<UpdateProvider> provider, UpdateObserver observer, BeforeSourceFetch beforeFetch)
    : provider_(std::move(provider)), observer_(std::move(observer)), beforeFetch_(std::move(beforeFetch)) {}
UpdateWorker::~UpdateWorker() { close(); }
bool UpdateWorker::available() const { return provider_ && provider_->enabled(); }
bool UpdateWorker::start(const std::filesystem::path& root) {
    std::lock_guard lock(mutex_);
    if (closed_ || busy_ || !available()) return false;
    if (thread_.joinable()) thread_.join();
    result_.reset(); stop_ = std::stop_source{}; busy_ = true;
    const auto generation = progress_.generation + 1; progress_ = {generation,UpdateStage::Checking,0,0};
    const auto token = stop_.get_token();
    try {
        thread_ = std::jthread([this, root, token, generation] {
            auto result = executeUpdate(root, provider_.get(), [this,generation,token](const UpdateProgress& p) {
                {
                    std::lock_guard lock(mutex_);
                    if (!closed_ && progress_.generation == generation) { progress_ = p; progress_.generation = generation; }
                }
                if (observer_) observer_(p, token);
            }, token, beforeFetch_);
            std::lock_guard lock(mutex_); busy_ = false;
            if (!closed_ && progress_.generation == generation) { result.generation = generation; result_ = std::move(result); }
        });
    } catch (...) { busy_ = false; throw; }
    return true;
}
void UpdateWorker::cancel() { std::stop_source stop; { std::lock_guard lock(mutex_); stop = stop_; } stop.request_stop(); }
void UpdateWorker::close() {
    { std::lock_guard lock(mutex_); closed_ = true; }
    cancel(); if (thread_.joinable()) thread_.join();
}
bool UpdateWorker::busy() const { std::lock_guard lock(mutex_); return busy_; }
UpdateProgress UpdateWorker::progress() const { std::lock_guard lock(mutex_); return progress_; }
std::optional<UpdateResult> UpdateWorker::takeResult() {
    std::lock_guard lock(mutex_); auto result = std::move(result_); result_.reset(); return result;
}
}
