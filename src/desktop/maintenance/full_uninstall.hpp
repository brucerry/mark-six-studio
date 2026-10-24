#pragma once
#include "uninstall_execution.hpp"
#include "elevation_request.hpp"
#include "native_report.hpp"
#include "helper_lifecycle.hpp"

namespace marksix::maintenance {
fs::path privilegedUninstallStage(const std::string& nonce);
bool protectedUninstallStage(const fs::path& stage);
std::shared_ptr<void> reserveInstallerLifecycle();
// Platform authorization launch runs on a worker while the original-user
// native agent waits on its administrator-only endpoint. Never called in tests.
int launchUninstallAuthorization(const ElevationRequest& request, const fs::path& application, bool retry);
// Dependency supplied by native code only, never by the IPC client. The
// nonpackaged fixture pins published identities; the product fetches its own.
int runUninstallBroker(const ElevationRequest& request, const std::function<ReleaseCatalog()>& releases = fetchReleaseCatalog);
// Native dependencies only; never populated from IPC, environment or CLI.
// Fixtures can exercise refusal without prompting or starting administrator code.
struct UninstallAuthorizationHooks {
    std::function<int(const ElevationRequest&, const fs::path&, bool)> launch = launchUninstallAuthorization;
    std::function<void(const std::string&)> waiting;
    std::function<void(const fs::path&, bool, const std::string&, const fs::path&)> finishCancelled;
};
int runUninstallAgent(const fs::path& executable, const UninstallScope& scope,
    const fs::path& application, const fs::path& dataRoot, const std::string& nonce, bool retry, std::string& observedReport,
    const UninstallAuthorizationHooks& authorizationHooks = {});
// Finalization contains only native system dependencies. Successful removal
// waits for both user and administrator staging; partial state remains runnable.
FullCleanupFinalization finalizeUninstallBroker(const fs::path& executable, const MachineGate& gate,
    const ProcessIdentity& userAgent, bool success);
int bootstrapUninstallWindows(const ElevationRequest& request);
}
