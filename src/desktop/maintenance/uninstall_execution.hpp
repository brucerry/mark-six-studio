#pragma once
#include "uninstall_journal.hpp"
#include "machine_gate.hpp"

namespace marksix::maintenance {
struct UninstallExecutionEnvironment {
    std::function<bool()> idle;
    std::function<void(const UninstallInspection&)> removeRegistrations;
    std::function<void(const std::string&, std::uint64_t, std::uint64_t)> progress;
};
struct UninstallOutcome {
    bool complete = false;
    std::string report;
    std::vector<InventoryIssue> remaining;
};
// The broker supplies its protected journal and native provider. No target
// list from an unprivileged process is accepted by this execution API.
UninstallOutcome executeUninstall(UninstallJournal& journal, const std::string& approvalDigest,
    bool permitIncomplete, const UninstallExecutionEnvironment& environment);
std::string uninstallRecoveryReview(const UninstallJournal& journal);
bool nativeInstallersIdle();
#ifdef _WIN32
void secureNativeInstallationRegistration(); // Fixed product key; installed administrator helper only.
#endif
void inspectNativeRegistrations(UninstallInspection& inspection);
void removeNativeRegistrations(const UninstallInspection& inspection);
}
