#pragma once
#include <string>
#include <memory>
#include <cstdint>

namespace marksix::maintenance {
void nativeStartupBlocked(const std::string& message); // No NSApplication/Qt cache initialization.
// System UI only: scrollable/selectable report, no Qt or app storage service.
bool nativeReview(const std::string& title, const std::string& report,
    const std::string& approve = {}, const std::string& cancel = "Close");
class NativeProgress {
public:
    NativeProgress();
    ~NativeProgress();
    NativeProgress(const NativeProgress&) = delete;
    void update(const std::string& message, std::uint64_t completed, std::uint64_t total);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
