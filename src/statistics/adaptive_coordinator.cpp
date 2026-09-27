#include "adaptive_coordinator.hpp"
#include <Windows.h>
#include <cstdio>

namespace marksix::statistics {
std::string adaptiveUtcNow() {
    SYSTEMTIME t{}; GetSystemTime(&t); char text[32]{};
    std::snprintf(text,sizeof(text),"%04u-%02u-%02uT%02u:%02u:%02uZ",unsigned(t.wYear),unsigned(t.wMonth),unsigned(t.wDay),unsigned(t.wHour),unsigned(t.wMinute),unsigned(t.wSecond));
    return text;
}
AdaptiveCoordinator::AdaptiveCoordinator(Observer observer) : observer_(std::move(observer)), thread_([this] { run(); }) {}
AdaptiveCoordinator::~AdaptiveCoordinator() { close(); }
void AdaptiveCoordinator::cancel() { std::lock_guard lock(mutex_); ++epoch_; active_.request_stop(); wake_.notify_one(); }
void AdaptiveCoordinator::close() {
    { std::lock_guard lock(mutex_); closed_ = true; ++epoch_; active_.request_stop(); wake_.notify_one(); }
    if (thread_.joinable()) thread_.join();
}
void AdaptiveCoordinator::run() {
    for (;;) {
        std::unique_lock lock(mutex_); wake_.wait(lock,[&] { return closed_ || !jobs_.empty(); });
        if (jobs_.empty() && closed_) return;
        auto job = std::move(jobs_.front()); jobs_.pop_front(); active_ = std::stop_source{};
        if (closed_ || job.epoch != epoch_) active_.request_stop();
        const auto token = active_.get_token(); lock.unlock(); job.run(token);
    }
}
std::future<int64_t> AdaptiveCoordinator::prepare(const std::filesystem::path& root, ArchiveState before, int64_t attempt, std::stop_token stop) {
    return submit<int64_t>([root,before=std::move(before),attempt](std::stop_token token) {
        if (token.stop_requested()) throw std::runtime_error("Adaptive boundary cancelled");
        const auto now=adaptiveUtcNow();AdaptiveLedger ledger(root);const auto boundary=ledger.prepareFetch(before,attempt,now);
        try { LabLedger lab(root); (void)lab.prepareFetch(before, attempt, now); }
        catch (const std::exception&) {
            // The independent lab boundary is optional; archive reconciliation continues.
        }
        return boundary;
    },stop);
}
std::future<LabWorkspace> AdaptiveCoordinator::lab(const std::filesystem::path& root,
                                                   std::stop_token stop) {
    return submit<LabWorkspace>([root](std::stop_token token) {
        LabWorkspace out;
        try {
            Archive source(root);
            const auto before = source.readState();
            LabLedger ledger(root);
            out.result = ledger.reconcile(before.snapshot, before.version, adaptiveUtcNow(), token);
            const auto after = source.readState();
            if (after.version != before.version ||
                after.snapshot.digest() != before.snapshot.digest()) {
                out.result.available = false;
                out.error = "History changed during Forecast Lab reconciliation; reload.";
                return out;
            }
            if (out.result.available) {
                ledger.annotateFetched(after, adaptiveUtcNow());
                out.evaluation = ledger.evaluation(out.result.lineage.id, token);
                out.result.replay.steps.clear();
                out.result.replay.steps.shrink_to_fit();
                for (size_t offset = 0;; offset += 100) {
                    const auto page = ledger.lineages(offset, 100);
                    out.lineages.insert(out.lineages.end(), page.begin(), page.end());
                    if (page.size() < 100) break;
                }
                out.historyLineage = out.result.lineage.id;
                out.history = ledger.forecasts(out.historyLineage, 0, 100);
            }
        } catch (const std::exception& e) {
            out.result.available = false;
            out.error = e.what();
        }
        return out;
    }, stop);
}
std::future<std::vector<LabStoredForecast>> AdaptiveCoordinator::labHistory(
    const std::filesystem::path& root, int64_t lineage, size_t offset,
    std::stop_token stop) {
    return submit<std::vector<LabStoredForecast>>([root, lineage, offset](std::stop_token token) {
        if (token.stop_requested()) throw std::runtime_error("Forecast Lab history cancelled");
        LabLedger ledger(root);
        return ledger.forecasts(lineage, offset, 100);
    }, stop);
}
std::future<AdaptiveWorkspace> AdaptiveCoordinator::reconcile(const std::filesystem::path& root, std::stop_token stop) {
    return submit<AdaptiveWorkspace>([root,this](std::stop_token token) {
        AdaptiveWorkspace out;
        try {
            Archive source(root); const auto before = source.readState(); AdaptiveLedger ledger(root);
            if(observer_)observer_(Stage::SnapshotRead,token);
            const auto boundary = ledger.pendingFetchBoundary(before);
            out.result = boundary ? ledger.reconcileFetched(before,boundary,adaptiveUtcNow(),token) :
                                    ledger.reconcile(before.snapshot,before.version,adaptiveUtcNow(),token);
            if(observer_)observer_(Stage::Reconciled,token);
            // A source commit survives any model failure. Never label an older snapshot current.
            const auto after = source.readState();
            if (after.version != before.version || after.snapshot.digest() != before.snapshot.digest()) {
                out.result.current.reset(); out.error = "History changed during learning; reload to reconcile the newer data.";
                return out;
            }
            for(size_t offset=0;;offset+=200){
                const auto page=ledger.lineages(offset,200);
                out.lineages.insert(out.lineages.end(),page.begin(),page.end());
                if(page.size()<200)break;
                if(token.stop_requested())throw std::runtime_error("Adaptive history cancelled");
            }
            if (out.result.current || !out.lineages.empty()) {
                out.historyLineage = out.result.current ? out.result.current->lineage.id : out.lineages.front().id;
                out.history = ledger.predictions(out.historyLineage,0,100);
                if(!token.stop_requested())out.progress = readLearningProgress(ledger,out.historyLineage,token);
            }
            const auto latest=source.readState();
            if(latest.version!=before.version||latest.snapshot.digest()!=before.snapshot.digest()){
                out.result.current.reset();out.progress={};out.error="History changed during progress analysis; reload to reconcile.";
            }
        } catch (const std::exception& e) { out.error = e.what(); out.result.current.reset(); }
        return out;
    },stop);
}
std::future<std::vector<AdaptiveStoredPrediction>> AdaptiveCoordinator::history(const std::filesystem::path& root, int64_t lineage, size_t offset, std::stop_token stop) {
    return submit<std::vector<AdaptiveStoredPrediction>>([root,lineage,offset](std::stop_token token) {
        if (token.stop_requested()) throw std::runtime_error("Adaptive history cancelled");
        AdaptiveLedger ledger(root); return ledger.predictions(lineage,offset,100);
    },stop);
}
}
