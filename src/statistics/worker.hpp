#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>

namespace marksix::statistics {
// One owned thread, one replaceable queued job, no HWNDs or callbacks into a view.
// UI polling consumes only the currently requested generation. Destruction joins.
template<class T> class LatestWorker {
public:
    using Job = std::function<T(std::stop_token)>;
    struct Result { uint64_t generation; std::shared_ptr<const T> value; std::string error; };
    LatestWorker() : thread_([this](std::stop_token stop) { run(stop); }) {}
    ~LatestWorker() { close(); }
    LatestWorker(const LatestWorker&) = delete;
    LatestWorker& operator=(const LatestWorker&) = delete;
    uint64_t submit(Job job) {
        std::lock_guard lock(mutex_);
        if (closed_) throw std::logic_error("Statistics worker is closed");
        active_.request_stop(); result_.reset();
        pending_ = Pending{++generation_, std::move(job)}; busy_ = true; wake_.notify_all(); return generation_;
    }
    void cancel() {
        std::lock_guard lock(mutex_); ++generation_; active_.request_stop(); pending_.reset(); result_.reset(); busy_ = false;
    }
    bool busy() const { std::lock_guard lock(mutex_); return busy_; }
    std::optional<Result> poll() {
        std::lock_guard lock(mutex_); auto out = std::move(result_); result_.reset(); return out;
    }
    void close() {
        {
            std::lock_guard lock(mutex_);
            if (closed_) return;
            closed_ = true; ++generation_; active_.request_stop(); pending_.reset(); result_.reset(); busy_ = false;
        }
        thread_.request_stop(); wake_.notify_all(); if (thread_.joinable()) thread_.join();
    }
private:
    struct Pending { uint64_t generation; Job job; };
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<Pending> pending_;
    std::optional<Result> result_;
    std::stop_source active_;
    uint64_t generation_ = 0;
    bool busy_ = false, closed_ = false;
    std::jthread thread_; // Last: every member above is initialized before the thread starts.
    void run(std::stop_token stop) {
        for (;;) {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [&] { return closed_ || pending_.has_value(); });
            if (closed_ || stop.stop_requested()) return;
            auto job = std::move(*pending_); pending_.reset(); active_ = std::stop_source{};
            const auto token = active_.get_token(); lock.unlock();
            Result result{job.generation, {}, {}};
            try { result.value = std::make_shared<const T>(job.job(token)); }
            catch (const std::exception& e) { result.error = e.what(); }
            catch (...) { result.error = "Unexpected statistics worker failure"; }
            lock.lock();
            if (!closed_ && !token.stop_requested() && job.generation == generation_) { result_ = std::move(result); busy_ = false; }
        }
    }
};
}
