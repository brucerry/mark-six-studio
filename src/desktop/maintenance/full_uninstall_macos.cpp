#include "full_uninstall.hpp"
#include "native_command.hpp"
#include "durable_files.hpp"
#include "payload_authority.hpp"
#include <sys/stat.h>
#include <map>

namespace marksix::maintenance {
fs::path privilegedUninstallStage(const std::string& nonce) {
    if (nonce.size() != 64 || nonce.find_first_not_of("0123456789abcdef") != std::string::npos) throw std::runtime_error("Invalid protected uninstall identity");
    return fs::path("/private/var/tmp") / ("marksix-uninstall-" + nonce);
}
bool protectedUninstallStage(const fs::path& stage) {
    try {
        identifyDirectory(stage); struct stat info{};
        return !lstat(stage.c_str(), &info) && S_ISDIR(info.st_mode) && info.st_uid == 0 && !(info.st_mode & 022);
    } catch (const std::exception&) { return false; }
}
std::shared_ptr<void> reserveInstallerLifecycle() { return {}; } // PKG preinstall gate plus native process inspection.
int launchUninstallAuthorization(const ElevationRequest& request, const fs::path&, bool retry) {
    const auto result = runNativeCommand("/usr/bin/osascript", {"-e", macosAuthorizationScript(), macosBootstrapCommand(request, retry)}, std::chrono::minutes(30));
    return macosAuthorizationExitCode(result.status, result.output);
}
FullCleanupFinalization finalizeUninstallBroker(const fs::path& executable, const MachineGate& gate, const ProcessIdentity& userAgent, bool success) {
    if (!success) return {};
    const auto stage = executable.parent_path();
    if (stage != privilegedUninstallStage(gate.nonce) || gate.stage != stage || !administrator() || !protectedProductFile(executable))
        throw std::runtime_error("Protected uninstall staging failed final authentication");
    if (entryExists(userAgent.executable.parent_path())) throw std::runtime_error("Original-user helper staging remains; protected retry state is retained");
    std::map<fs::path, std::string> files;
    for (const auto& entry : fs::directory_iterator(stage)) {
        const auto name = entry.path().filename();
        if (entry.path() != executable && name != "ledger.bin" && name != ".operation.lock" && name != "remaining.txt")
            throw std::runtime_error("Unexpected protected staging file; retained for review");
        files.emplace(entry.path(), readBoundedFile(entry.path(), 64 * 1024 * 1024));
    }
    try {
        for (const auto& [path, bytes] : files) { (void)bytes; durableRemove(path); }
        durableRemove(stage);
        removeMachineGate(gate.nonce);
    } catch (...) {
        if (!entryExists(stage)) { makePrivateDirectory(stage); chmod(stage.c_str(), 0711); }
        for (const auto& [path, bytes] : files) if (!entryExists(path)) durableWrite(path, bytes);
        if (chmod(executable.c_str(), 0755)) throw std::runtime_error("Protected retry helper could not be restored");
        throw;
    }
    return {};
}
int bootstrapUninstallWindows(const ElevationRequest&) { throw std::runtime_error("Windows bootstrap is not a macOS operation"); }
}
