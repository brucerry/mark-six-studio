#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace marksix::maintenance {
namespace fs = std::filesystem;

enum class Operation { ClearCache, ResetRecords, Uninstall };
enum class Category { Cache, Records, RecoveryCopy, Installer, Application, Registration, Maintenance };

// Portable, versioned manifest. These are discovery hints, never deletion authority.
struct VerifiedFile {
    fs::path path;
    std::string sha256;
    bool operator==(const VerifiedFile&) const = default;
};
struct OwnershipManifest {
    std::vector<fs::path> dataRoots;
    std::vector<fs::path> createdDirectories;
    std::vector<VerifiedFile> downloads;
    std::vector<VerifiedFile> recordings;
    bool operator==(const OwnershipManifest&) const = default;
};

// File and ancestor identities must still match at execution. Includes content digest.
struct FileIdentity {
    std::uint64_t volume = 0;
    std::uint64_t file = 0;
    std::uint64_t bytes = 0;
    std::int64_t modified = 0;
    std::string ancestors;
    std::string sha256;
    std::string linkTarget; // Nonempty only for an explicitly inventoried payload symlink.
    bool operator==(const FileIdentity&) const = default;
};
struct Target {
    fs::path path;
    fs::path boundary;
    std::string account;
    Category category = Category::Records;
    std::string evidence;
    FileIdentity identity;
};
struct InventoryIssue {
    fs::path path;
    std::string account;
    std::string reason;
    bool unresolved = true;
};
struct PreservedFile {
    fs::path path;
    std::string account;
    std::string reason;
    bool operator==(const PreservedFile&) const = default;
};
struct Inventory {
    std::vector<Target> targets;
    std::vector<InventoryIssue> issues;
    std::vector<PreservedFile> preservedLegacy;
    bool complete() const;
    std::uint64_t bytes() const;
};
struct UserStorage {
    std::string account; // Native SID or UID, not an elevation-dependent display name.
    fs::path home;
    fs::path dataRoot;
    fs::path cacheRoot;
    fs::path downloads;
    fs::path manifestPath;
    std::vector<fs::path> additionalDataRoots;
    std::vector<fs::path> additionalDownloadRoots;
    bool locationsResolved = true;
};
enum class HostPlatform { Windows, MacOS };
struct NativeProfile {
    std::string account;
    fs::path home;
    fs::path localData;
    fs::path downloads;
    bool locationsResolved = true;
};
UserStorage storageForProfile(const NativeProfile& profile, HostPlatform platform);
std::vector<fs::path> nativeCacheRoots(const UserStorage& user);
struct InstalledApplication {
    fs::path root;
    fs::path executable;
    std::string account; // Empty for an installation shared by every user.
    std::string registration;
};
bool isDevelopmentLocation(const fs::path& path);
bool hasProductIdentity(const fs::path& applicationRoot);
#ifdef __APPLE__
bool applicationRegistered(const fs::path& applicationRoot); // Current native account; read-only LaunchServices query.
std::string nativeApplicationRegistrations(); // Bounded native account/URL snapshot; no app launch.
#endif
std::vector<InstalledApplication> discoverApplications(const std::vector<UserStorage>& users,
    std::vector<InventoryIssue>& issues);
struct InventoryRequest {
    Operation operation = Operation::ClearCache;
    std::vector<UserStorage> users;
    fs::path activeDataRoot;
    // Protected packages survive all operations until native installation is known to be idle.
    std::vector<fs::path> protectedDownloads;
    std::vector<PreservedFile> retainedLegacy = {}; // Native protected recovery snapshot, never deletion authority.
};

std::string pathUtf8(const fs::path& path);
fs::path pathFromUtf8(const std::string& text);
std::string sha256File(const fs::path& path);
std::string sha256Bytes(std::string_view bytes);
FileIdentity identifyFile(const fs::path& path);
FileIdentity identifyFileMetadata(const fs::path& path); // Regular-file identity, no content read/hash.
FileIdentity identifyDirectory(const fs::path& path); // Identity and ancestor chain; never traverses contents.
FileIdentity identifySymbolicLink(const fs::path& path);
// Reject links/reparse points in every existing ancestor, root/home/system roots and '..'.
void validateBoundary(const fs::path& boundary, const fs::path& home);
void validateContainedPath(const fs::path& path, const fs::path& boundary);
bool stillMatches(const Target& target);
OwnershipManifest readManifest(const fs::path& path); // Missing file is an empty manifest.
void writeManifest(const fs::path& path, const OwnershipManifest& manifest);
void rememberDataRoot(OwnershipManifest& manifest, const fs::path& root);
void rememberDownload(OwnershipManifest& manifest, const fs::path& file);
Inventory inspectStorage(const InventoryRequest& request);
bool legacyRecordingName(const fs::path& path);
void preserveLegacyFile(Inventory& inventory, const fs::path& path, const std::string& account,
    const std::string& reason = "Legacy simulator recording candidate; preserved by policy without reading its contents.");
void discoverPreservedLegacy(Inventory& inventory, const fs::path& root, const std::string& account);
bool intentionallyPreserved(const Inventory& inventory, const fs::path& path);
std::string preservedLegacyReport(const Inventory& inventory);
std::string quotedReportField(const std::string& value);

// Read-only discovery: no directory creation or system-wide registry hive mounting.
std::vector<UserStorage> discoverUsers(bool allUsers, std::vector<InventoryIssue>& issues);
struct PayloadInspection {
    Inventory inventory;
    std::vector<fs::path> directories;
};
void preserveApplicationRecordingHints(Inventory& inventory, const std::vector<UserStorage>& users,
    const std::vector<InstalledApplication>& applications);
PayloadInspection inspectApplicationPayload(const InstalledApplication& application,
    const std::vector<fs::path>& protectedHomes, const std::vector<PreservedFile>& retainedLegacy = {});
}
