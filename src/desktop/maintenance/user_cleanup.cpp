#include "user_cleanup.hpp"
#include "durable_files.hpp"
#include "native_runtime.hpp"
#include "installer_lease.hpp"
#include "verified_removal.hpp"
#include <map>
#include <sstream>
#include <algorithm>

namespace marksix::maintenance {
Inventory inspectUserCleanup(const UserCleanupRequest& request, const UserStorage& user) {
    if (request.operation == Operation::Uninstall) throw std::runtime_error("Complete uninstall requires its administrator lifecycle");
    validateBoundary(request.dataRoot, user.home);
    std::vector<InventoryIssue> installerIssues;
    const auto protectedFiles = request.operation == Operation::ClearCache ? protectedInstallerFiles(user, installerIssues) : std::vector<fs::path>{};
    auto inventory = inspectStorage({request.operation, {user}, request.dataRoot, protectedFiles});
    inventory.issues.insert(inventory.issues.end(), installerIssues.begin(), installerIssues.end());
    if (request.operation == Operation::ClearCache) discoverPreservedLegacy(inventory, request.dataRoot, user.account);
    return inventory;
}
void clearUserCache(const Inventory& inventory, const UserStorage& user, const std::string& digest, const CleanupProgress& progress) {
    if (!inventory.complete() || inventoryDigest(Operation::ClearCache, inventory) != digest)
        throw std::runtime_error("Cache cleanup needs an unchanged complete approval");
    const auto roots = nativeCacheRoots(user);
    for (const auto& root : roots) validateBoundary(root, user.home);
    for (const auto& target : inventory.targets) {
        if (std::find(roots.begin(), roots.end(), target.boundary) == roots.end()) throw std::runtime_error("Unapproved native cache boundary");
        validateContainedPath(target.path, target.boundary);
        if (target.category != Category::Cache || target.account != user.account || intentionallyPreserved(inventory, target.path) || !stillMatches(target))
            throw std::runtime_error("Cache inventory changed; refresh the review");
    }
    std::uint64_t completed = 0;
    for (const auto& root : roots) {
        std::vector<Target> targets;
        for (const auto& target : inventory.targets) if (target.boundary == root) targets.push_back(target);
        if (targets.empty()) continue;
        std::map<fs::path, FileIdentity> directories;
        for (const auto& target : targets) for (auto parent = target.path.parent_path(); parent != root; parent = parent.parent_path())
            directories.emplace(parent, identifyDirectory(parent));
        const auto stage = root / (".marksix-clear-" + randomToken()); makePrivateDirectory(stage);
        try {
            for (std::size_t i = 0; i < targets.size(); ++i) {
                const auto& target = targets[i]; const auto moved = stage / std::to_string(i);
                if (!stillMatches(target)) throw std::runtime_error("Cache target changed before staging");
                durableRename(target.path, moved);
                const auto actual = identifyFile(moved); const auto& expected = target.identity;
                if (actual.volume != expected.volume || actual.file != expected.file || actual.sha256 != expected.sha256 ||
                    actual.bytes != expected.bytes || actual.modified != expected.modified) {
                    durableRename(moved, target.path);
                    throw std::runtime_error("Changed cache entry was restored; review is required");
                }
                durableRemove(moved);
                if (progress) progress("Removed disposable cache file", ++completed, inventory.targets.size());
            }
            durableRemove(stage);
        } catch (...) {
            // Remaining staged bytes are disposable cache. The next bounded
            // cache inventory can retry them; no saved history is staged here.
            std::error_code ignored; if (fs::is_empty(stage, ignored)) fs::remove(stage, ignored); throw;
        }
        std::vector<std::pair<fs::path, FileIdentity>> ordered(directories.begin(), directories.end());
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first.native().size() > b.first.native().size(); });
        for (const auto& [directory, identity] : ordered) if (entryExists(directory)) (void)removeVerifiedEmptyDirectory(directory, identity);
    }
    for (const auto& target : inventory.targets) if (entryExists(target.path)) throw std::runtime_error("A cache entry was recreated; cleanup is incomplete");
}
std::string cleanupReview(Operation operation, const Inventory& inventory) {
    std::ostringstream text;
    text << (operation == Operation::ClearCache ? "Clear regenerable caches for this OS account. Saved records, learning history and browser downloads are preserved.\n" :
        "Permanently reset saved records and learning history in the active root. Draws, provenance, forecasts, models and migration recovery copies are removed together. This cannot be undone.\n");
    text << "\nValidated files: " << inventory.targets.size() << "\nEstimated bytes: " << inventory.bytes() << "\n\n";
    for (const auto& target : inventory.targets) text << "Account: " << quotedReportField(target.account) << "\nPath: " << quotedReportField(pathUtf8(target.path)) << "\n\n";
    text << preservedLegacyReport(inventory);
    return text.str();
}
}
