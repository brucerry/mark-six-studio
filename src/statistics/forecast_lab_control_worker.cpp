#include "forecast_lab_control_worker.hpp"
#include <algorithm>
#include <stdexcept>

namespace marksix::statistics {
std::future<LabControlReport> LabControlWorker::start(size_t historyLength,
                                                      double observedStatistic,
                                                      std::string sourceDigest) {
    if (active_.load()) throw std::runtime_error("Forecast Lab controls already running");
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard lock(mutex_);
        points_.clear();
    }
    completed_.store(0); active_.store(true);
    auto promise = std::make_shared<std::promise<LabControlReport>>();
    auto future = promise->get_future();
    thread_ = std::jthread([this, promise, historyLength, observedStatistic,
                            sourceDigest = std::move(sourceDigest)](std::stop_token stop) {
        try {
            LabLedger ledger(root_);
            if (auto cached = ledger.control(sourceDigest, historyLength)) {
                if (cached->observedStatistic != observedStatistic)
                    throw std::runtime_error("Forecast Lab cached observed statistic mismatch");
                {
                    std::lock_guard lock(mutex_);
                    for (size_t i = 0; i < cached->completed; ++i)
                        points_.push_back({i + 1, cached->historyHashes[i], cached->nullStatistics[i]});
                }
                completed_.store(cached->completed);
                active_.store(false);
                promise->set_value(std::move(*cached));
            } else {
                auto report = labControls(historyLength, observedStatistic, sourceDigest,
                    LabControlRuns, stop, [this](size_t completed, size_t,
                                                const std::string& hash, double statistic) {
                        {
                            std::lock_guard lock(mutex_);
                            points_.push_back({completed, hash, statistic});
                        }
                        completed_.store(completed);
                    });
                if (report.complete) ledger.saveControl(report);
                active_.store(false);
                promise->set_value(std::move(report));
            }
        } catch (...) { active_.store(false); promise->set_exception(std::current_exception()); }
    });
    return future;
}
void LabControlWorker::cancel() { if (thread_.joinable()) thread_.request_stop(); }
LabControlProgress LabControlWorker::progress() const {
    return {active_.load(), completed_.load(), LabControlRuns};
}
std::vector<LabControlPoint> LabControlWorker::page(size_t offset, size_t limit) const {
    if (limit < 1 || limit > 100) throw std::invalid_argument("Forecast Lab control page limit");
    std::lock_guard lock(mutex_);
    if (offset >= points_.size()) return {};
    const size_t end = std::min(points_.size(), offset + limit);
    return {points_.begin() + offset, points_.begin() + end};
}
}
