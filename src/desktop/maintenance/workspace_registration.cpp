#include "workspace_registration.hpp"
#include "native_runtime.hpp"
#include <algorithm>
#include <chrono>
#include <thread>
#include <stdexcept>

namespace marksix::maintenance {
bool rememberWorkspace(const UserStorage& user, const fs::path& activeRoot) {
    validateBoundary(activeRoot, user.home);
    if (isDevelopmentLocation(activeRoot)) return false;
    validateContainedPath(user.manifestPath, user.manifestPath.parent_path());
    std::unique_ptr<FileLease> lease;
    for (unsigned attempt = 0; attempt < 50 && !lease; ++attempt) {
        lease = FileLease::acquire(user.manifestPath.parent_path() / ".ownership.lock", FileLease::Mode::Exclusive);
        if (!lease) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!lease) throw std::runtime_error("Another session is updating ownership hints; the custom root was not registered");
    auto manifest = readManifest(user.manifestPath);
    const auto before = manifest;
    rememberDataRoot(manifest, activeRoot);
    // Registration never claims that a pre-existing directory was app-created.
    if (manifest != before) writeManifest(user.manifestPath, manifest);
    return true;
}
void rememberCurrentWorkspace(const fs::path& activeRoot) {
    std::vector<InventoryIssue> issues; const auto users = discoverUsers(false, issues);
    if (users.size() != 1 || users.front().account != currentAccount() || !users.front().locationsResolved)
        throw std::runtime_error("Current native account locations are unresolved; select this custom root explicitly during future cleanup");
    (void)rememberWorkspace(users.front(), activeRoot);
}
}
