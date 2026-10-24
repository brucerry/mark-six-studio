#include "user_cleanup.hpp"
#include "native_runtime.hpp"
#include "native_dialog.hpp"
#include "machine_gate.hpp"
#include "installer_lease.hpp"

namespace marksix::maintenance {
std::string executeUserCleanup(const UserCleanupRequest& request, const std::string& account,
    std::string& observedReport, const UserCleanupEnvironment& environment) {
    observedReport = "Preserved legacy files: inspection has not completed. No empty-list or complete-cleanup claim is available.\n";
    const auto machine = readMachineGate();
    const bool recovering = request.operation == Operation::ResetRecords && resetPending(request.dataRoot);
    if (machine && (!recovering || processState(machine->executor) != ProcessState::Exited)) throw std::runtime_error("Complete uninstall is active or its executor is uncertain; finish its administrator helper before current-user cleanup");
    if (request.operation == Operation::Uninstall) throw std::runtime_error("Complete uninstall requires its administrator lifecycle");
    std::vector<InventoryIssue> issues; const auto users = environment.users(false, issues);
    if (users.size() != 1 || users.front().account != account) throw std::runtime_error("Current OS account could not be independently resolved");
    const auto& user = users.front(); validateBoundary(request.dataRoot, user.home);
    auto accountLease = FileLease::acquire(accountLeasePath(user), FileLease::Mode::Exclusive);
    if (!accountLease) throw std::runtime_error("Another desktop or maintenance helper for this OS account still has its files open; no cleanup started");
    if (!environment.writersIdle(user.account)) throw std::runtime_error("Close all Mark Six Studio instances for this account before retrying");
    auto lease = FileLease::acquire(sessionLeasePath(request.dataRoot), FileLease::Mode::Exclusive);
    if (!lease) throw std::runtime_error("Another workspace or maintenance operation still holds the storage lease");
    if (request.recovery || (request.operation == Operation::ResetRecords && resetPending(request.dataRoot))) {
        Inventory preserved; discoverPreservedLegacy(preserved, request.dataRoot, user.account);
        const auto report = "An interrupted reset is blocking this workspace. Recovery restores the complete old generation before purge, or finishes the previously approved purge after its durable commit marker.\n\n" + preservedLegacyReport(preserved);
        observedReport = report;
        if (!environment.review("Recover saved-data reset", report, "Recover reset", "Cancel")) return "Recovery cancelled. Saved files are unchanged.\n\n" + preservedLegacyReport(preserved);
        const auto recovered = recoverReset(request.dataRoot);
        return std::string(recovered == ResetRecovery::Restored ? "The complete previous saved-data generation was restored.\n\n" :
            recovered == ResetRecovery::Purged ? "The previously approved reset completed. Prior records and learning history are removed.\n\n" :
            "No reset remains pending.\n\n") + preservedLegacyReport(preserved);
    }
    if (resetPending(request.dataRoot)) throw std::runtime_error("Recover the interrupted reset before starting another operation");
    auto inventory = inspectUserCleanup(request, user);
    if (!issues.empty()) inventory.issues.insert(inventory.issues.end(), issues.begin(), issues.end());
    observedReport = cleanupReview(request.operation, inventory);
    if (!inventory.complete()) throw std::runtime_error("Inspection is incomplete; no cleanup started.\n\n" + cleanupReview(request.operation, inventory));
    const auto digest = inventoryDigest(request.operation, inventory);
    if (!environment.review(request.operation == Operation::ClearCache ? "Clear cache" : "Reset saved records and learning history",
        cleanupReview(request.operation, inventory), request.operation == Operation::ClearCache ? "Clear cache" : "Permanently reset", "Cancel"))
        return "Cleanup cancelled. No inventoried files were removed.\n\n" + preservedLegacyReport(inventory);
    if (!environment.writersIdle(user.account)) throw std::runtime_error("Another app instance started during review; no cleanup started");
    if (readMachineGate()) throw std::runtime_error("System cleanup began during review; no current-user deletion started");
    const auto fresh = inspectUserCleanup(request, user);
    observedReport = cleanupReview(request.operation, fresh);
    if (inventoryDigest(request.operation, fresh) != digest) throw std::runtime_error("Files changed during review. Retry to review the updated inventory; no cleanup started");
    if (environment.progress) environment.progress("Beginning the approved cleanup", 0, inventory.targets.size());
    if (request.operation == Operation::ClearCache) clearUserCache(inventory, user, digest, environment.progress);
    else {
        std::uint64_t completed = 0;
        try { resetRecords(request.dataRoot, inventory, digest, [&](std::string_view phase) {
            if (!environment.writersIdle(user.account)) throw std::runtime_error("Another app instance started during reset; recovery must complete before reopening");
            if (phase == "purged") ++completed;
            if (environment.progress) environment.progress(phase == "staged" ? "Staging the complete saved-data generation" :
                phase == "purged" ? "Permanently removing the approved saved records" : "Securing reset recovery state", completed, inventory.targets.size());
        }); }
        catch (const std::exception& original) {
            const auto message = std::string(original.what());
            if (resetPending(request.dataRoot)) {
                try { (void)recoverReset(request.dataRoot); }
                catch (const std::exception& recovery) { throw std::runtime_error(message + "\nRecovery also failed: " + recovery.what() + "\nWorkspace remains blocked at " + pathUtf8(resetGatePath(request.dataRoot))); }
            }
            throw std::runtime_error(message + "\nRecovery completed; retry to review the remaining operation.");
        }
    }
    if (!environment.writersIdle(user.account)) throw std::runtime_error("Another application instance started during cleanup; final quiescence could not be verified");
    return std::string(inventory.preservedLegacy.empty() ? "Current-app cleanup completed for the reviewed scope.\n\n" :
        "Current-app cleanup completed with legacy files preserved. Their containing directories remain.\n\n") + preservedLegacyReport(inventory);
}
}
