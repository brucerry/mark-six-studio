#include "installer_lease.hpp"
#include "durable_files.hpp"
#include "wire_codec.hpp"
#include <regex>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <signal.h>
#include <errno.h>
#endif

namespace marksix::maintenance {
ProcessState processState(const ProcessIdentity& process) {
    if (!process.pid || !process.started) return ProcessState::Unknown;
    const auto current = processIdentity(process.pid);
    if (current) return current->started == process.started ? ProcessState::Alive : ProcessState::Exited;
#ifdef _WIN32
    const auto handle = OpenProcess(SYNCHRONIZE, FALSE, process.pid);
    if (!handle) return GetLastError() == ERROR_INVALID_PARAMETER ? ProcessState::Exited : ProcessState::Unknown;
    const auto result = WaitForSingleObject(handle, 0); CloseHandle(handle);
    return result == WAIT_OBJECT_0 ? ProcessState::Exited : ProcessState::Unknown;
#else
    return kill(int(process.pid), 0) && errno == ESRCH ? ProcessState::Exited : ProcessState::Unknown;
#endif
}
fs::path installerLeasePath(const fs::path& updateCache) { return updateCache / ".installer-lease.bin"; }
void writeInstallerLease(const fs::path& path, const InstallerLease& lease) {
    validateContainedPath(lease.package.path, path.parent_path());
    if (lease.package.sha256.size() != 64 || lease.package.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("Invalid installer lease digest");
    if ((!lease.version.empty() && !std::regex_match(lease.version, std::regex("[0-9]+\\.[0-9]+\\.[0-9]+"))) ||
        (!lease.beforeNativeStamp.empty() && (lease.beforeNativeStamp.size() != 64 || lease.beforeNativeStamp.find_first_not_of("0123456789abcdef") != std::string::npos)))
        throw std::runtime_error("Invalid native installation evidence");
    Encoder out; out.text("M6INSTALLER2"); out.path(lease.package.path); out.text(lease.package.sha256); out.number(bool(lease.process));
    if (lease.process) { out.number(lease.process->pid); out.number(lease.process->started); out.text(lease.process->account); }
    out.text(lease.version); out.text(lease.beforeNativeStamp);
    durableWrite(path, out.bytes);
}
InstallerLease readInstallerLease(const fs::path& path) {
    const auto bytes = readBoundedFile(path, 128 * 1024); Decoder in(bytes);
    const auto format = in.text();
    if (format != "M6INSTALLER1" && format != "M6INSTALLER2") throw std::runtime_error("Unsupported installer lease");
    InstallerLease lease; lease.package.path = in.path(); lease.package.sha256 = in.text();
    validateContainedPath(lease.package.path, path.parent_path());
    if (lease.package.sha256.size() != 64 || lease.package.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("Invalid installer lease digest");
    const auto present = in.number(); if (present > 1) throw std::runtime_error("Invalid installer process state");
    if (present) {
        const auto pid = in.number(); const auto started = in.number(); auto account = in.text();
        if (!pid || pid > UINT32_MAX || !started) throw std::runtime_error("Invalid installer process identity");
        lease.process = ProcessIdentity{std::uint32_t(pid), started, std::move(account), {}, false};
    }
    if (format == "M6INSTALLER2") {
        lease.version = in.text(); lease.beforeNativeStamp = in.text();
        if ((!lease.version.empty() && !std::regex_match(lease.version, std::regex("[0-9]+\\.[0-9]+\\.[0-9]+"))) ||
            (!lease.beforeNativeStamp.empty() && (lease.beforeNativeStamp.size() != 64 || lease.beforeNativeStamp.find_first_not_of("0123456789abcdef") != std::string::npos))) throw std::runtime_error("Invalid native installer evidence");
    }
    in.end(); return lease;
}
std::vector<fs::path> protectedInstallerFiles(const UserStorage& user, std::vector<InventoryIssue>& issues, const InstallerEvidence& evidence) {
    std::vector<fs::path> protectedFiles;
    const auto root = user.cacheRoot / "updates";
    try {
        if (!entryExists(root)) return {};
        validateContainedPath(root / ".probe", user.cacheRoot);
        std::size_t count = 0;
        for (auto entry = fs::recursive_directory_iterator(root); entry != fs::recursive_directory_iterator(); ++entry) {
            if (++count > 100000) throw std::runtime_error("Installer-lease discovery exceeds limit");
            validateContainedPath(entry->path(), root);
            if (fs::is_symlink(entry->symlink_status())) { entry.disable_recursion_pending(); continue; }
            if (entry->path().filename() != ".installer-lease.bin") continue;
            const auto path = entry->path();
            const auto lease = readInstallerLease(path);
            const auto state = lease.process ? evidence.process(*lease.process) : ProcessState::Unknown;
            const auto installed = state == ProcessState::Exited && !lease.version.empty() && evidence.installersIdle() ? evidence.installation() : NativeInstallationState{};
            if (installed.complete && installed.version == lease.version && !installed.stamp.empty() && installed.stamp != lease.beforeNativeStamp) {
                issues.push_back({lease.package.path, user.account, "Native installer exited; changed protected installation receipt/registration confirms the requested version, and native installers are idle. The unused cache package and lease are eligible for cleanup", false});
                continue;
            }
            protectedFiles.push_back(path); protectedFiles.push_back(lease.package.path);
            // A stale/reused PID proves that this process ended, not that a
            // native installer child/transaction finished. Native completion
            // evidence is required before releasing a handed-off package.
            issues.push_back({lease.package.path, user.account, state == ProcessState::Alive ?
                "The native installer is still running; its package and lease are preserved" :
                "Native installation completion cannot be established; its package and lease are preserved", true});
        }
    } catch (const std::exception& error) {
        issues.push_back({root, user.account, std::string("Installer state cannot be established: ") + error.what(), true});
        protectedFiles.push_back(root);
    }
    return protectedFiles;
}
}
