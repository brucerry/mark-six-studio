#pragma once
#include "storage_inventory.hpp"
#include <memory>
#include <optional>

namespace marksix::maintenance {
struct ProcessIdentity {
    std::uint32_t pid = 0;
    std::uint64_t started = 0;
    std::string account;
    fs::path executable;
    bool uncertain = false;
    bool operator==(const ProcessIdentity&) const = default;
};
std::uint32_t currentProcessId();
std::string currentAccount();
bool administrator();
std::optional<ProcessIdentity> processIdentity(std::uint32_t pid);
bool sameProcessAlive(const ProcessIdentity& process);
std::vector<ProcessIdentity> runningApplications(const std::string& account = {});
std::string randomToken();
fs::path normalizePlatformPath(const fs::path& path);

class FileLease {
public:
    enum class Mode { Shared, Exclusive };
    static std::unique_ptr<FileLease> acquire(const fs::path& path, Mode mode, bool create = true);
    ~FileLease();
    bool matchesEmptyFile(const FileIdentity& expected) const;
    FileLease(const FileLease&) = delete;
    FileLease& operator=(const FileLease&) = delete;
private:
    explicit FileLease(std::intptr_t handle) : handle_(handle) {}
    std::intptr_t handle_ = -1;
};
fs::path sessionLeasePath(const fs::path& dataRoot);
fs::path accountLeasePath(const UserStorage& user);
std::unique_ptr<FileLease> acquireCurrentDesktopLease(); // Before Qt; shared for the entire desktop lifetime.
}
