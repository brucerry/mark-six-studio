#include "uninstall_inventory.hpp"
#include "installer_lease.hpp"
#include "reset_journal.hpp"
#include "wire_codec.hpp"
#include "uninstall_metadata.hpp"
#include "durable_files.hpp"
#include <algorithm>
#include <set>
#include <map>
#include <sstream>

namespace marksix::maintenance {
namespace {
bool inside(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    if (path == root || (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")) return true;
    for (auto cursor = path; !cursor.empty(); cursor = cursor.parent_path()) {
        std::error_code error;
        if (fs::equivalent(cursor, root, error) && !error) return true;
        if (cursor == cursor.root_path()) break;
    }
    return false;
}
void merge(Inventory& destination, const Inventory& source) {
    for (const auto& file : source.preservedLegacy) preserveLegacyFile(destination, file.path, file.account, file.reason);
    for (const auto& target : source.targets) {
        if (intentionallyPreserved(destination, target.path)) continue;
        const auto found = std::find_if(destination.targets.begin(), destination.targets.end(), [&](const auto& existing) { return existing.path == target.path; });
        if (found == destination.targets.end()) destination.targets.push_back(target);
        else if (found->account != target.account || found->identity != target.identity || found->category != target.category)
            destination.issues.push_back({target.path, target.account, "Conflicting account or ownership attribution; independent review is required", true});
    }
    destination.issues.insert(destination.issues.end(), source.issues.begin(), source.issues.end());
}
void paths(Encoder& out, std::vector<fs::path> value) {
    std::sort(value.begin(), value.end()); value.erase(std::unique(value.begin(), value.end()), value.end());
    out.number(value.size()); for (const auto& path : value) out.path(path);
}
}
std::string encodeUninstallScope(const UninstallScope& scope) {
    if (scope.additional.size() > 64) throw std::runtime_error("Too many additional cleanup locations");
    Encoder out; out.text("M6UNINSTALLSCOPE1"); out.number(scope.additional.size());
    for (const auto& location : scope.additional) {
        if (location.account.empty() || location.account.size() > 256 || !location.path.is_absolute() || location.path != location.path.lexically_normal() ||
            (location.kind != LocationKind::SavedData && location.kind != LocationKind::Downloads && location.kind != LocationKind::Application)) throw std::runtime_error("Invalid additional cleanup location");
        out.text(location.account); out.path(location.path); out.number(std::uint64_t(location.kind));
    }
    return out.bytes;
}
UninstallScope decodeUninstallScope(std::string_view bytes) {
    Decoder in(bytes); if (in.text() != "M6UNINSTALLSCOPE1") throw std::runtime_error("Unsupported uninstall scope");
    const auto count = in.number(); if (count > 64) throw std::runtime_error("Too many additional cleanup locations");
    UninstallScope scope;
    for (std::uint64_t i = 0; i < count; ++i) {
        auto account = in.text(); auto path = in.path(); const auto kind = in.number();
        if (kind > 2) throw std::runtime_error("Invalid cleanup location kind");
        scope.additional.push_back({std::move(account), std::move(path), LocationKind(kind)});
    }
    in.end(); (void)encodeUninstallScope(scope); return scope;
}
UninstallInspection inspectUninstall(const UninstallScope& scope, const std::string& authenticatedAccount, const UninstallDiscovery& discovery) {
    (void)encodeUninstallScope(scope);
    if (authenticatedAccount.empty()) throw std::runtime_error("The initiating native account is required");
    UninstallInspection result; result.originalAccount = authenticatedAccount;
    result.inventory.preservedLegacy = discovery.retainedLegacy;
    std::vector<InventoryIssue> issues;
    result.users = discovery.users(true, issues);
    if (result.users.empty() || result.users.size() > 4096) throw std::runtime_error("All-user account discovery is unavailable or exceeds its limit");
    std::set<std::string> accounts;
    for (const auto& user : result.users) {
        if (user.account.empty() || !accounts.insert(user.account).second || !user.home.is_absolute()) throw std::runtime_error("Ambiguous native account mapping");
    }
    if (!accounts.contains(authenticatedAccount)) throw std::runtime_error("The initiating account is missing from independent OS discovery");
    for (const auto& location : scope.additional) {
        const auto user = std::find_if(result.users.begin(), result.users.end(), [&](const auto& candidate) { return candidate.account == location.account; });
        if (user == result.users.end()) throw std::runtime_error("Additional location refers to an unknown native account");
        for (const auto& profile : result.users) {
            validateBoundary(location.path, profile.home);
            if (profile.account != user->account && inside(location.path, profile.home)) throw std::runtime_error("Additional location belongs to a different OS profile");
        }
        if (location.kind == LocationKind::Application) continue;
        auto& roots = location.kind == LocationKind::SavedData ? user->additionalDataRoots : user->additionalDownloadRoots;
        if (std::find(roots.begin(), roots.end(), location.path) == roots.end()) roots.push_back(location.path);
    }
    // Persist all independently discovered custom roots in the native snapshot,
    // so deleting the original hints cannot strand standalone retry coverage.
    for (auto& user : result.users) {
        try {
            if (intentionallyPreserved(result.inventory, user.manifestPath)) throw std::runtime_error("Ownership metadata is covered by a protected legacy hint; retained without reading");
            const auto manifest = readManifest(user.manifestPath);
            for (const auto& root : manifest.dataRoots) if (std::find(user.additionalDataRoots.begin(), user.additionalDataRoots.end(), root) == user.additionalDataRoots.end())
                user.additionalDataRoots.push_back(root);
        } catch (const std::exception& error) { issues.push_back({user.manifestPath, user.account, error.what(), true}); }
    }
    std::vector<fs::path> protectedPackages;
    for (const auto& user : result.users) {
        const auto protectedFiles = protectedInstallerFiles(user, issues);
        protectedPackages.insert(protectedPackages.end(), protectedFiles.begin(), protectedFiles.end());
    }
    result.inventory = inspectStorage({Operation::Uninstall, result.users, {}, protectedPackages, discovery.retainedLegacy});
    inspectUninstallMetadata(result.inventory, result.users, discovery.heldLeases);
    const auto catalog = discovery.catalog();
    inspectDownloads(result.inventory, result.users, catalog, protectedPackages);
    result.applications = discovery.applications(result.users, issues);
    for (const auto& app : discovery.retainedApplications) if (std::none_of(result.applications.begin(), result.applications.end(), [&](const auto& native) { return native.root == app.root; }))
        result.applications.push_back(app);
    for (const auto& location : scope.additional) if (location.kind == LocationKind::Application &&
        std::none_of(result.applications.begin(), result.applications.end(), [&](const auto& app) { return app.root == location.path; })) {
        std::string nativeAccount;
        for (const auto& user : result.users) if (inside(location.path, user.home)) { nativeAccount = user.account; break; }
#ifdef _WIN32
        const auto executable = location.path / "mark-six-studio.exe";
#else
        const auto executable = location.path / "Contents/MacOS/mark-six-studio";
#endif
        if (!entryExists(executable)) {
            // The protected recovery journal can retain authority for a
            // partially removed payload after its executable is gone.
            discoverPreservedLegacy(result.inventory, location.path, nativeAccount); continue;
        }
        if (!hasProductIdentity(location.path)) { issues.push_back({location.path, location.account, "Selected application has unverified native product identity", true}); continue; }
        result.applications.push_back({location.path, executable, nativeAccount, {}});
    }
    std::vector<fs::path> homes; for (const auto& user : result.users) homes.push_back(user.home);
    preserveApplicationRecordingHints(result.inventory, result.users, result.applications);
    const auto references = trustedPayloads(result.applications, homes, discovery.authority, result.inventory.preservedLegacy);
    for (const auto& application : result.applications) {
        if (!entryExists(application.executable)) { discoverPreservedLegacy(result.inventory, application.root, application.account); continue; }
        const auto payload = inspectApplicationPayload(application, homes, result.inventory.preservedLegacy);
        const auto authorized = authorizePayload(application, payload, references, discovery.authority);
        merge(result.inventory, authorized);
        if (authorized.targets.size() == payload.inventory.targets.size() && !authorized.targets.empty())
            result.payloadDirectories.insert(result.payloadDirectories.end(), payload.directories.begin(), payload.directories.end());
    }
    result.inventory.issues.insert(result.inventory.issues.end(), issues.begin(), issues.end());
    inspectUninstallDirectories(result);
    return result;
}
std::string uninstallDigest(const UninstallInspection& inspection) {
    Encoder out; out.text("M6UNINSTALLAPPROVAL1"); out.text(inspection.originalAccount);
    auto users = inspection.users; std::sort(users.begin(), users.end(), [](const auto& a, const auto& b) { return a.account < b.account; });
    out.number(users.size());
    for (const auto& user : users) {
        out.text(user.account); out.path(user.home); out.path(user.dataRoot); out.path(user.cacheRoot); out.path(user.downloads);
        out.path(user.manifestPath); out.number(user.locationsResolved); paths(out, user.additionalDataRoots); paths(out, user.additionalDownloadRoots);
    }
    auto applications = inspection.applications;
    std::sort(applications.begin(), applications.end(), [](const auto& a, const auto& b) { return a.root < b.root; });
    out.number(applications.size());
    for (const auto& application : applications) {
        out.path(application.root); out.path(application.executable); out.text(application.account); out.text(application.registration);
    }
    paths(out, inspection.payloadDirectories);
    auto directories = inspection.ownedDirectories;
    std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    out.number(directories.size());
    for (const auto& directory : directories) { out.path(directory.path); out.text(directory.account); out.identity(directory.identity); }
    out.text(inspection.nativeState); out.text(inspection.nativeReview);
    out.text(inventoryDigest(Operation::Uninstall, inspection.inventory));
    return sha256Bytes(out.bytes);
}
std::string uninstallReview(const UninstallInspection& inspection) {
    std::ostringstream out;
    out << "Permanently remove Mark Six Studio and its validated files for ALL reviewed OS accounts. This cannot be undone. Legacy recordings and unrelated files are preserved. This review is not permission to delete unknown files.\n\n"
        << "Initiating account: " << quotedReportField(inspection.originalAccount) << "\nNative accounts (" << inspection.users.size() << ")\n";
    for (const auto& user : inspection.users) {
        out << "Account: " << quotedReportField(user.account) << "\nHome: " << quotedReportField(pathUtf8(user.home)) <<
            "\nLocations: " << (user.locationsResolved ? "resolved" : "INCOMPLETE") << "\nSaved data: " << quotedReportField(pathUtf8(user.dataRoot)) << '\n';
        for (const auto& root : nativeCacheRoots(user)) out << "Cache: " << quotedReportField(pathUtf8(root)) << '\n';
        out << "Ownership metadata: " << quotedReportField(pathUtf8(user.manifestPath)) << "\nDownloads: " << quotedReportField(pathUtf8(user.downloads)) << '\n';
        for (const auto& root : user.additionalDataRoots) out << "Additional saved data: " << quotedReportField(pathUtf8(root)) << '\n';
        for (const auto& root : user.additionalDownloadRoots) out << "Additional downloads: " << quotedReportField(pathUtf8(root)) << '\n';
        out << '\n';
    }
    out << "Reviewed application locations (" << inspection.applications.size() << ")\n";
    for (const auto& app : inspection.applications) out << quotedReportField(pathUtf8(app.root)) << " | account " << quotedReportField(app.account.empty() ? "Shared installation" : app.account) << '\n';
    out << "Validated files (" << inspection.inventory.targets.size() << ", " << inspection.inventory.bytes() << " bytes)\n";
    for (const auto& target : inspection.inventory.targets) out << "Path: " << quotedReportField(pathUtf8(target.path)) << "\nAccount: " << quotedReportField(target.account) <<
        "\nOwnership: " << quotedReportField(target.evidence) << "\n\n";
    out << "Owned directories: removed only if empty, with unchanged native identity (" << inspection.ownedDirectories.size() << ")\n";
    for (const auto& directory : inspection.ownedDirectories) out << quotedReportField(pathUtf8(directory.path)) << " | account " << quotedReportField(directory.account) << '\n';
    out << preservedLegacyReport(inspection.inventory);
    out << "\nNative registrations\n" << inspection.nativeReview;
    out << "\nUnregistered custom roots and nonstandard or nested download folders require explicit selection. Shared OS components, unrelated files and developer checkouts are outside cleanup scope.\n";
    if (!inspection.inventory.complete()) out << "\nInspection is INCOMPLETE. Full cleanup cannot be reported as complete.\n";
    return out.str();
}
void inspectUninstallDirectories(UninstallInspection& inspection) {
    std::map<fs::path, std::string> candidates;
    for (const auto& path : inspection.payloadDirectories) candidates.emplace(path, std::string());
    for (const auto& user : inspection.users) {
        for (const auto& path : {user.dataRoot, user.dataRoot / "statistics", user.cacheRoot, user.manifestPath.parent_path()})
            candidates.emplace(path, user.account);
        // A user-editable createdDirectories hint alone is not authority to
        // remove a custom parent. Only fixed product namespaces, protected
        // payload members and parents inside validated boundaries qualify.
#ifdef __APPLE__
        candidates.emplace(user.home / "Library/Saved Application State/com.brucerry.marksixstudio.savedState", user.account);
        candidates.emplace(user.home / "Library/Caches/com.brucerry.marksixstudio", user.account);
#endif
    }
    // Only ancestors inside an already validated dedicated product boundary.
    for (const auto& target : inspection.inventory.targets) {
        for (auto path = target.path.parent_path(); path != target.boundary && !path.empty(); path = path.parent_path()) candidates.emplace(path, target.account);
    }
    inspection.ownedDirectories.clear();
    for (const auto& [path, account] : candidates) {
        try {
            if (!entryExists(path) || isDevelopmentLocation(path)) continue;
            for (const auto& user : inspection.users) validateBoundary(path, user.home);
            inspection.ownedDirectories.push_back({path, account, identifyDirectory(path)});
        } catch (const std::exception& error) { inspection.inventory.issues.push_back({path, account, error.what(), true}); }
    }
}
}
