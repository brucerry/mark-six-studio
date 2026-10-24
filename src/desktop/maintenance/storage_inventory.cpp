#include "storage_inventory.hpp"

#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>

namespace marksix::maintenance {
namespace {
struct DatabaseKind { const char* filename; int application; int maximumVersion; const char* tables; };
constexpr DatabaseKind databases[] = {
    {"results.sqlite3", 0x4d365354, 3, "'sources','revisions','draws'"},
    {"adaptive.sqlite3", 0x4d364144, 1, "'lineages','predictions','head'"},
    {"forecast_lab.sqlite3", 0x4d36464c, 1, "'lineages','forecasts','head'"}
};
bool present(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return false;
    if (error) throw std::runtime_error("Cannot inspect location: " + error.message());
    return status.type() != fs::file_type::not_found;
}
std::string immutableUri(const fs::path& path) {
    std::string uri = "file:";
    for (const auto raw : pathUtf8(path)) {
        const auto byte = static_cast<unsigned char>(raw);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '/' || byte == '-' || byte == '_' || byte == '.') uri += char(byte);
        else { uri += '%'; uri += "0123456789ABCDEF"[byte >> 4]; uri += "0123456789ABCDEF"[byte & 15]; }
    }
    return uri + "?mode=ro&immutable=1";
}
int scalar(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) throw std::runtime_error("Unrecognized database schema");
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    if (sqlite3_step(raw) != SQLITE_ROW) throw std::runtime_error("Cannot validate database schema");
    return sqlite3_column_int(raw, 0);
}
void verifyDatabase(const fs::path& file, const DatabaseKind& kind) {
    sqlite3* raw = nullptr;
    const auto result = sqlite3_open_v2(immutableUri(file).c_str(), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
    if (result != SQLITE_OK) throw std::runtime_error("Cannot inspect database read-only");
    sqlite3_db_config(raw, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_db_config(raw, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
    sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, 1024 * 1024);
    int budget = 1000;
    sqlite3_progress_handler(raw, 100, [](void* value) -> int { return --*static_cast<int*>(value) <= 0; }, &budget);
    const auto version = scalar(raw, "PRAGMA user_version");
    if (scalar(raw, "PRAGMA application_id") != kind.application || version < 1 || version > kind.maximumVersion ||
        scalar(raw, std::string("SELECT count(*) FROM sqlite_schema WHERE type='table' AND name IN (") + kind.tables + ")") != 3)
        throw std::runtime_error("Unrecognized or unsupported database; retained");
}
bool protectedFile(const fs::path& file, const InventoryRequest& request) {
    return std::any_of(request.protectedDownloads.begin(), request.protectedDownloads.end(), [&](const auto& path) {
        const auto relative = file.lexically_relative(path);
        return file == path || (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..");
    });
}
void appendFile(Inventory& inventory, const fs::path& file, const fs::path& boundary, const UserStorage& user,
                Category category, const std::string& evidence) {
    validateContainedPath(file, boundary);
    if (legacyRecordingName(file)) { preserveLegacyFile(inventory, file, user.account); return; }
    if (intentionallyPreserved(inventory, file)) return;
    (void)pathFromUtf8(pathUtf8(file)); // Unsupported filename bytes remain unresolved, never deletion targets.
    if (std::any_of(inventory.targets.begin(), inventory.targets.end(), [&](const auto& target) { return target.path == file; })) return;
    inventory.targets.push_back({file, boundary, user.account, category, evidence, identifyFile(file)});
}
void inspectData(Inventory& inventory, const fs::path& root, const UserStorage& user, bool activeReset = false) {
    try {
        validateBoundary(root, user.home);
        if (!present(root)) return;
        if (!activeReset && isDevelopmentLocation(root)) {
            inventory.issues.push_back({root, user.account, "Developer checkout or build archive is outside complete uninstall; its contents were not inspected or selected for removal", false});
            return;
        }
        if (intentionallyPreserved(inventory, root) || intentionallyPreserved(inventory, root / "statistics"))
            throw std::runtime_error("A registered legacy hint covers the archive container; review is required before current records can be removed");
        discoverPreservedLegacy(inventory, root, user.account);
        if (intentionallyPreserved(inventory, root)) throw std::runtime_error("An opaque legacy candidate covers this saved-data root; current records cannot be certified as reset or removed");
        const auto directory = root / "statistics";
        validateContainedPath(directory / ".probe", root);
        if (!present(directory)) return;
        if (present(directory / ".marksix-reset")) inventory.issues.push_back({directory / ".marksix-reset", user.account,
            "An interrupted reset may contain saved history. Recover this root before full cleanup; staged records are not empty storage", true});
        for (const auto& kind : databases) {
            const auto file = directory / kind.filename;
            try {
                if (intentionallyPreserved(inventory, file)) continue;
                if (present(file)) {
                    validateContainedPath(file, root);
                    const auto before = identifyFile(file);
                    verifyDatabase(file, kind);
                    if (before != identifyFile(file)) throw std::runtime_error("Database changed during inspection; refresh after writers stop");
                    appendFile(inventory, file, root, user, Category::Records, "Supported application ID and schema");
                    for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
                        auto sidecar = file; sidecar += suffix;
                        if (present(sidecar)) appendFile(inventory, sidecar, root, user, Category::Records, "Sidecar of validated application database");
                    }
                } else {
                    for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
                        auto sidecar = file; sidecar += suffix;
                        if (present(sidecar)) inventory.issues.push_back({sidecar, user.account, "Orphan database sidecar needs review", true});
                    }
                }
            } catch (const std::exception& error) { inventory.issues.push_back({file, user.account, error.what(), true}); }
        }
        const std::regex backupPattern("results\\.sqlite3\\.before-v3-[a-f0-9]{24}\\.sqlite3");
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (!std::regex_match(pathUtf8(entry.path().filename()), backupPattern)) continue;
            if (intentionallyPreserved(inventory, entry.path())) continue;
            try {
                validateContainedPath(entry.path(), root);
                const auto before = identifyFile(entry.path());
                verifyDatabase(entry.path(), databases[0]);
                if (before != identifyFile(entry.path())) throw std::runtime_error("Recovery copy changed during inspection");
                appendFile(inventory, entry.path(), root, user, Category::RecoveryCopy, "Validated application migration recovery copy");
            } catch (const std::exception& error) { inventory.issues.push_back({entry.path(), user.account, error.what(), true}); }
        }
    } catch (const std::exception& error) { inventory.issues.push_back({root, user.account, error.what(), true}); }
}
void inspectCache(Inventory& inventory, const UserStorage& user, const InventoryRequest& request, const fs::path& cacheRoot) {
    try {
        validateBoundary(cacheRoot, user.home);
        if (!present(cacheRoot)) return;
        std::size_t count = 0;
        for (auto entry = fs::recursive_directory_iterator(cacheRoot); entry != fs::recursive_directory_iterator(); ++entry) {
            if (++count > 100000) throw std::runtime_error("Cache inventory exceeds limit");
            const auto path = entry->path();
            try {
                validateContainedPath(path, cacheRoot);
                const auto status = entry->symlink_status();
                if (intentionallyPreserved(inventory, path)) { entry.disable_recursion_pending(); continue; }
                if (legacyRecordingName(path)) { preserveLegacyFile(inventory, path, user.account); entry.disable_recursion_pending(); continue; }
                if (fs::is_directory(status)) {
                    if (protectedFile(path, request)) {
                        inventory.issues.push_back({path, user.account, "Installer state is uncertain; this in-app download container is preserved", true});
                        discoverPreservedLegacy(inventory, path, user.account); entry.disable_recursion_pending(); continue;
                    }
                    validateContainedPath(path / ".probe", cacheRoot);
                    continue;
                }
                if (legacyRecordingName(path)) { preserveLegacyFile(inventory, path, user.account); continue; }
                if (!fs::is_regular_file(status)) throw std::runtime_error("Redirected or unsupported cache entry retained");
                if (protectedFile(path, request)) {
                    inventory.issues.push_back({path, user.account, "A download or native installer still needs this file", true}); continue;
                }
                appendFile(inventory, path, cacheRoot, user, Category::Cache, "App-specific regenerable cache location");
            } catch (const std::exception& error) {
                inventory.issues.push_back({path, user.account, error.what(), true});
                entry.disable_recursion_pending();
            }
        }
    } catch (const std::exception& error) { inventory.issues.push_back({cacheRoot, user.account, error.what(), true}); }
}
}

bool Inventory::complete() const {
    return std::none_of(issues.begin(), issues.end(), [](const auto& issue) { return issue.unresolved; });
}
std::uint64_t Inventory::bytes() const {
    std::uint64_t total = 0;
    for (const auto& target : targets) total += target.identity.bytes;
    return total;
}
Inventory inspectStorage(const InventoryRequest& request) {
    Inventory inventory;
    inventory.preservedLegacy = request.retainedLegacy;
    for (const auto& user : request.users) {
        if (!user.locationsResolved) inventory.issues.push_back({user.home, user.account, "Account storage locations could not be completely resolved", true});
        OwnershipManifest manifest;
        try {
            if (intentionallyPreserved(inventory, user.manifestPath)) throw std::runtime_error("Ownership metadata is itself covered by a retained legacy hint");
            manifest = readManifest(user.manifestPath);
        }
        catch (const std::exception& error) {
            inventory.issues.push_back({user.manifestPath, user.account, std::string("Ownership hints are unreadable; this account's storage is retained: ") + error.what(), true});
            std::set<fs::path> legacyRoots{user.dataRoot, user.cacheRoot, user.downloads};
            for (const auto& cache : nativeCacheRoots(user)) legacyRoots.insert(cache);
            legacyRoots.insert(user.additionalDataRoots.begin(), user.additionalDataRoots.end());
            legacyRoots.insert(user.additionalDownloadRoots.begin(), user.additionalDownloadRoots.end());
            if (request.operation == Operation::ResetRecords) legacyRoots.insert(request.activeDataRoot);
            for (const auto& root : legacyRoots) {
                try { validateBoundary(root, user.home); discoverPreservedLegacy(inventory, root, user.account); }
                catch (const std::exception& failure) { inventory.issues.push_back({root, user.account, failure.what(), false}); }
            }
            continue;
        }
        std::set<fs::path> roots{user.dataRoot};
        roots.insert(user.additionalDataRoots.begin(), user.additionalDataRoots.end());
        roots.insert(manifest.dataRoots.begin(), manifest.dataRoots.end());
        if (request.operation == Operation::ResetRecords) roots.insert(request.activeDataRoot);
        auto hintRoots = roots; hintRoots.insert(user.cacheRoot); hintRoots.insert(user.downloads);
        for (const auto& cache : nativeCacheRoots(user)) hintRoots.insert(cache);
        hintRoots.insert(user.additionalDownloadRoots.begin(), user.additionalDownloadRoots.end());
        for (const auto& recording : manifest.recordings) {
            try {
                bool bounded = false;
                for (const auto& root : hintRoots) {
                    try { validateBoundary(root, user.home); validateContainedPath(recording.path, root); bounded = true; break; }
                    catch (const std::exception&) {}
                }
                if (!bounded) throw std::runtime_error("Registered legacy hint is outside inspected storage roots");
                if (present(recording.path)) {
                    const auto name = pathUtf8(recording.path.filename());
                    for (const auto& root : roots) if (recording.path.parent_path() == root / "statistics") {
                        bool reserved = name.starts_with("results.sqlite3.before-v3-");
                        for (const auto& kind : databases) for (const auto* suffix : {"", "-wal", "-shm", "-journal"})
                            if (name == std::string(kind.filename) + suffix) reserved = true;
                        if (reserved) inventory.issues.push_back({recording.path, user.account,
                            "Legacy hint conflicts with a reserved current-archive filename; preserved without reading its contents, review required", true});
                    }
                    preserveLegacyFile(inventory, recording.path, user.account,
                        "Registered legacy recording hint; preserved by policy without reading its contents.");
                }
            } catch (const std::exception& error) { inventory.issues.push_back({recording.path, user.account, error.what(), false}); }
        }
        if (request.operation != Operation::ResetRecords) for (const auto& cache : nativeCacheRoots(user)) inspectCache(inventory, user, request, cache);
        if (request.operation == Operation::ClearCache) continue;
        if (request.operation == Operation::ResetRecords) inspectData(inventory, request.activeDataRoot, user, true);
        else for (const auto& root : roots) inspectData(inventory, root, user);
        // Installer and payload inventory is added by the native lifecycle provider.
    }
    return inventory;
}
}
