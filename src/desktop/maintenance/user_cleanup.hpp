#pragma once
#include "reset_journal.hpp"
#include "native_runtime.hpp"
#include "native_dialog.hpp"
#include <functional>

namespace marksix::maintenance {
struct UserCleanupRequest {
    Operation operation = Operation::ClearCache;
    fs::path dataRoot;
    bool recovery = false;
};
Inventory inspectUserCleanup(const UserCleanupRequest& request, const UserStorage& user);
// Requires an exclusive workspace lease. Revalidates the entire plan before
// moving any cache file, then verifies each moved file before deleting it.
using CleanupProgress = std::function<void(const std::string&, std::uint64_t, std::uint64_t)>;
void clearUserCache(const Inventory& inventory, const UserStorage& user, const std::string& digest,
    const CleanupProgress& progress = {});
std::string cleanupReview(Operation operation, const Inventory& inventory);
struct UserCleanupEnvironment {
    std::function<std::vector<UserStorage>(bool, std::vector<InventoryIssue>&)> users = discoverUsers;
    std::function<bool(const std::string&)> writersIdle = [](const auto& account) { return runningApplications(account).empty(); };
    std::function<bool(const std::string&, const std::string&, const std::string&, const std::string&)> review = nativeReview;
    CleanupProgress progress;
};
// Called only after the initiating desktop exits. The environment is native in
// production; tests inject profile/consent callbacks over private fixtures.
// observedReport remains available even when an exception interrupts execution.
std::string executeUserCleanup(const UserCleanupRequest& request, const std::string& account,
    std::string& observedReport, const UserCleanupEnvironment& environment = {});
}
