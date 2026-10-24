#include "uninstall_metadata.hpp"
#include "durable_files.hpp"
#include "native_runtime.hpp"
#include <algorithm>
#include <set>

namespace marksix::maintenance {
namespace {
void append(Inventory& inventory, const fs::path& path, const fs::path& root, const UserStorage& user, const std::string& evidence, Category category = Category::Maintenance) {
    validateContainedPath(path, root);
    if (intentionallyPreserved(inventory, path) || legacyRecordingName(path)) {
        preserveLegacyFile(inventory, path, user.account);
        inventory.issues.push_back({path, user.account, "Legacy preservation conflicts with a reserved maintenance filename; retained for explicit review", true}); return;
    }
    if (!std::any_of(inventory.targets.begin(), inventory.targets.end(), [&](const auto& existing) { return existing.path == path; }))
        inventory.targets.push_back({path, root, user.account, category, evidence, identifyFile(path)});
}
void lease(Inventory& inventory, const fs::path& path, const fs::path& boundary, const UserStorage& user, const std::vector<fs::path>& held) {
    if (!entryExists(path)) return;
    validateContainedPath(path, boundary);
    if (intentionallyPreserved(inventory, path)) {
        inventory.issues.push_back({path, user.account, "A registered legacy hint covers this reserved coordination file; retained for review", true}); return;
    }
    auto identity = identifyFileMetadata(path);
    if (identity.bytes) throw std::runtime_error("Unsupported coordination marker content; retained");
    identity.sha256 = sha256Bytes("");
    std::unique_ptr<FileLease> acquired;
    if (std::find(held.begin(), held.end(), path) == held.end()) {
        acquired = FileLease::acquire(path, FileLease::Mode::Exclusive, false);
        if (!acquired) throw std::runtime_error("An application session or metadata writer still holds this marker; retained until orderly shutdown");
    }
    auto metadata = identity; metadata.sha256.clear();
    if ((acquired && !acquired->matchesEmptyFile(identity)) || identifyFileMetadata(path) != metadata) throw std::runtime_error("Coordination marker changed during inspection");
    if (!std::any_of(inventory.targets.begin(), inventory.targets.end(), [&](const auto& target) { return target.path == path; }))
        inventory.targets.push_back({path, boundary, user.account, Category::Maintenance,
            "Empty product coordination marker in a validated product namespace; exclusive native lease verified", identity});
}
}
void inspectUninstallMetadata(Inventory& inventory, const std::vector<UserStorage>& users, const std::vector<fs::path>& held) {
    for (const auto& user : users) {
        std::set<fs::path> roots{user.dataRoot}; roots.insert(user.additionalDataRoots.begin(), user.additionalDataRoots.end());
        OwnershipManifest manifest;
        try {
            if (intentionallyPreserved(inventory, user.manifestPath)) throw std::runtime_error("A legacy hint conflicts with reserved ownership metadata; preserved without further reading");
            const auto before = entryExists(user.manifestPath) ? std::optional<FileIdentity>(identifyFile(user.manifestPath)) : std::nullopt;
            manifest = readManifest(user.manifestPath); roots.insert(manifest.dataRoots.begin(), manifest.dataRoots.end());
            if (before) {
                if (identifyFile(user.manifestPath) != *before) throw std::runtime_error("Ownership metadata changed during inspection");
                append(inventory, user.manifestPath, user.manifestPath.parent_path(), user, "Versioned product ownership metadata independently parsed without opening saved-data services");
            }
            lease(inventory, user.manifestPath.parent_path() / ".ownership.lock", user.manifestPath.parent_path(), user, held);
            lease(inventory, accountLeasePath(user), user.manifestPath.parent_path(), user, held);
        } catch (const std::exception& error) {
            inventory.issues.push_back({user.manifestPath, user.account, error.what(), true});
            continue; // Unreadable hints cannot silently unprotect renamed recordings.
        }
        for (const auto& root : roots) {
            try {
                validateBoundary(root, user.home);
                if (isDevelopmentLocation(root)) continue;
                const auto statistics = root / "statistics";
                lease(inventory, sessionLeasePath(root), statistics, user, held);
            } catch (const std::exception& error) { inventory.issues.push_back({root, user.account, error.what(), true}); }
        }
#ifdef __APPLE__
        // These are fixed per-account namespaces for this bundle, not a scan of
        // Library/Preferences or other applications' saved-state containers.
        const auto preferences = user.home / "Library/Preferences/com.brucerry.marksixstudio.plist";
        for (const auto& recording : manifest.recordings) if (recording.path == preferences && entryExists(preferences))
            preserveLegacyFile(inventory, preferences, user.account, "Registered legacy hint conflicts with a reserved native preferences path; preserved for review.");
        try { if (entryExists(preferences)) append(inventory, preferences, preferences.parent_path(), user, "Fixed native preferences namespace for com.brucerry.marksixstudio"); }
        catch (const std::exception& error) { inventory.issues.push_back({preferences, user.account, error.what(), true}); }
        for (const auto& [relative, category] : {std::pair{"Library/Saved Application State/com.brucerry.marksixstudio.savedState", Category::Maintenance},
            std::pair{"Library/Caches/com.brucerry.marksixstudio", Category::Cache}}) {
            const auto container = user.home / relative;
            try {
                if (!entryExists(container)) continue;
                validateBoundary(container, user.home);
                for (const auto& recording : manifest.recordings) {
                    try {
                        if (recording.path != container) validateContainedPath(recording.path, container);
                        if (entryExists(recording.path)) preserveLegacyFile(inventory, recording.path, user.account,
                            "Registered legacy recording in a native product container; preserved without reading its contents.");
                    } catch (const std::exception&) {} // An unrelated hint is not a target here.
                }
                discoverPreservedLegacy(inventory, container, user.account);
                if (intentionallyPreserved(inventory, container)) {
                    inventory.issues.push_back({container, user.account, "A registered legacy hint covers this native container; contents are retained without inspection", true}); continue;
                }
                std::size_t count = 0;
                for (auto entry = fs::recursive_directory_iterator(container); entry != fs::recursive_directory_iterator(); ++entry) {
                    if (++count > 100000) throw std::runtime_error("Native product-container inventory exceeds its limit");
                    const auto path = entry->path(); validateContainedPath(path, container);
                    if (legacyRecordingName(path) || intentionallyPreserved(inventory, path)) { preserveLegacyFile(inventory, path, user.account); entry.disable_recursion_pending(); continue; }
                    if (fs::is_directory(entry->symlink_status())) continue;
                    append(inventory, path, container, user, "Dedicated native product container for com.brucerry.marksixstudio", category);
                }
            } catch (const std::exception& error) { inventory.issues.push_back({container, user.account, error.what(), true}); }
        }
#endif
    }
}
}
