#pragma once
#include "native_runtime.hpp"
#include <functional>

namespace marksix::maintenance {
enum class ProcessState { Exited, Alive, Unknown };
ProcessState processState(const ProcessIdentity& process);
struct InstallerLease {
    VerifiedFile package;
    std::optional<ProcessIdentity> process; // Absent means handoff cannot be proven idle.
    std::string version = {}, beforeNativeStamp = {};
};
struct NativeInstallationState { std::string version, stamp; bool complete = false; };
NativeInstallationState nativeInstallationState(); // Protected receipt/registration and installed code; read-only.
bool nativeInstallersIdle();
struct InstallerEvidence {
    std::function<ProcessState(const ProcessIdentity&)> process = processState;
    std::function<bool()> installersIdle = nativeInstallersIdle;
    std::function<NativeInstallationState()> installation = nativeInstallationState;
};
fs::path installerLeasePath(const fs::path& updateCache);
void writeInstallerLease(const fs::path& path, const InstallerLease& lease);
InstallerLease readInstallerLease(const fs::path& path);
std::vector<fs::path> protectedInstallerFiles(const UserStorage& user, std::vector<InventoryIssue>& issues, const InstallerEvidence& evidence = {});
struct InstallerLaunch { bool started = false; std::optional<ProcessIdentity> process; std::string error; };
using InstallerCompletion = std::function<void(InstallerLaunch)>;
void launchNativeInstaller(const fs::path& package, InstallerCompletion completed);
}
