#pragma once
#include "download_inventory.hpp"
#include "payload_authority.hpp"
#include <functional>

namespace marksix::maintenance {
enum class LocationKind { SavedData, Downloads, Application };
struct AdditionalLocation {
    std::string account;
    fs::path path;
    LocationKind kind = LocationKind::SavedData;
    bool operator==(const AdditionalLocation&) const = default;
};
// These are discovery requests only. No file list or asserted ownership crosses
// the privilege boundary. The original account comes from authenticated IPC.
struct UninstallScope {
    std::vector<AdditionalLocation> additional;
};
struct UninstallInspection {
    struct Directory { fs::path path; std::string account; FileIdentity identity; };
    std::vector<UserStorage> users;
    std::vector<InstalledApplication> applications;
    std::vector<fs::path> payloadDirectories;
    std::vector<Directory> ownedDirectories;
    std::string nativeState, nativeReview; // Native provider's independently derived registration proof.
    Inventory inventory;
    std::string originalAccount;
};
struct UninstallDiscovery {
    std::function<std::vector<UserStorage>(bool, std::vector<InventoryIssue>&)> users = discoverUsers;
    std::function<std::vector<InstalledApplication>(const std::vector<UserStorage>&, std::vector<InventoryIssue>&)> applications = discoverApplications;
    std::function<ReleaseCatalog()> catalog = fetchReleaseCatalog;
    PayloadAuthority authority;
    std::vector<fs::path> heldLeases; // Native provider's actual held handles; never IPC input.
    std::vector<PreservedFile> retainedLegacy; // Native journal hints applied before any file hashing.
    std::vector<InstalledApplication> retainedApplications; // Previously verified native addresses for retry; not payload ownership.
};
UninstallInspection inspectUninstall(const UninstallScope& scope, const std::string& authenticatedAccount,
    const UninstallDiscovery& discovery = {});
std::string uninstallDigest(const UninstallInspection& inspection);
std::string uninstallReview(const UninstallInspection& inspection);
void inspectUninstallDirectories(UninstallInspection& inspection);
std::string encodeUninstallScope(const UninstallScope& scope);
UninstallScope decodeUninstallScope(std::string_view bytes);
}
