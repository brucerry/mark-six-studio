#pragma once
#include "adaptive_ledger.hpp"
#include "learning_progress.hpp"
#include "forecast_lab_ledger.hpp"
#include "archive.hpp"
#include <condition_variable>
#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace marksix::statistics {
struct AdaptiveWorkspace {
    AdaptiveReconcileResult result;
    std::vector<AdaptiveLineage> lineages;
    std::vector<AdaptiveStoredPrediction> history;
    int64_t historyLineage = 0;
    size_t historyOffset = 0;
    std::string error;
    LearningProgress progress;
};
struct LabWorkspace {
    LabReconcile result;
    LabEvaluation evaluation;
    std::vector<LabLineage> lineages;
    std::vector<LabStoredForecast> history;
    int64_t historyLineage = 0;
    size_t historyOffset = 0;
    std::string error;
};
std::string adaptiveUtcNow();
// All model writes are serialized here, never in replaceable analysis jobs.
// No window handles, detached threads, or network access. Destruction cancels/joins.
class AdaptiveCoordinator {
public:
    enum class Stage { SnapshotRead, Reconciled };
    using Observer = std::function<void(Stage,std::stop_token)>;
    explicit AdaptiveCoordinator(Observer observer = {});
    ~AdaptiveCoordinator();
    AdaptiveCoordinator(const AdaptiveCoordinator&) = delete;
    std::future<AdaptiveWorkspace> reconcile(const std::filesystem::path& root, std::stop_token stop = {});
    std::future<LabWorkspace> lab(const std::filesystem::path& root, std::stop_token stop = {});
    std::future<std::vector<LabStoredForecast>> labHistory(const std::filesystem::path& root,
        int64_t lineage, size_t offset, std::stop_token stop = {});
    std::future<int64_t> prepare(const std::filesystem::path& root, ArchiveState before, int64_t attempt, std::stop_token stop = {});
    std::future<std::vector<AdaptiveStoredPrediction>> history(const std::filesystem::path& root, int64_t lineage, size_t offset, std::stop_token stop = {});
    void cancel();
    void close();
private:
    struct Job { uint64_t epoch; std::function<void(std::stop_token)> run; };
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::stop_source active_;
    uint64_t epoch_ = 0;
    bool closed_ = false;
    Observer observer_;
    std::jthread thread_;
    void run();
    template<class T, class F> std::future<T> submit(F f, std::stop_token outside) {
        auto promise = std::make_shared<std::promise<T>>(); auto future = promise->get_future();
        std::lock_guard lock(mutex_);
        if (closed_ || jobs_.size() >= 16) throw std::runtime_error("Adaptive coordinator closed or queue full");
        jobs_.push_back({epoch_,[promise,f=std::move(f),outside](std::stop_token inside) mutable {
            try {
                std::stop_source combined;
                std::stop_callback a(inside,[&] { combined.request_stop(); });
                std::stop_callback b(outside,[&] { combined.request_stop(); });
                if (combined.stop_requested()) throw std::runtime_error("Adaptive job cancelled");
                // Do not discard a commit-winning result because cancellation arrived later.
                promise->set_value(f(combined.get_token()));
            } catch (...) { promise->set_exception(std::current_exception()); }
        }});
        wake_.notify_one(); return future;
    }
};
}
