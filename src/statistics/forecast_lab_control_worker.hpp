#pragma once
#include "forecast_lab_ledger.hpp"
#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace marksix::statistics {
struct LabControlPoint { size_t run = 0; std::string historyHash; double statistic = 0; };
struct LabControlProgress { bool active = false; size_t completed = 0, requested = LabControlRuns; };
// One local CPU worker; pages expose completed runs without copying an entire report.
class LabControlWorker {
public:
    explicit LabControlWorker(std::filesystem::path root) : root_(std::move(root)) {}
    ~LabControlWorker() { cancel(); if (thread_.joinable()) thread_.join(); }
    LabControlWorker(const LabControlWorker&) = delete;
    LabControlWorker& operator=(const LabControlWorker&) = delete;
    std::future<LabControlReport> start(size_t historyLength, double observedStatistic,
                                        std::string sourceDigest);
    void cancel();
    LabControlProgress progress() const;
    std::vector<LabControlPoint> page(size_t offset, size_t limit = 100) const;
private:
    std::filesystem::path root_;
    mutable std::mutex mutex_;
    std::vector<LabControlPoint> points_;
    std::atomic<bool> active_{false};
    std::atomic<size_t> completed_{0};
    std::jthread thread_;
};
}
