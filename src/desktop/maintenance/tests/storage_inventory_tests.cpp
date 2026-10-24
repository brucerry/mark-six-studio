#include "storage_inventory.hpp"
#include "native_runtime.hpp"
#include "reset_journal.hpp"
#include "durable_files.hpp"
#include "user_cleanup.hpp"
#include "installer_lease.hpp"
#include "json_reader.hpp"
#include "download_inventory.hpp"
#include "private_channel.hpp"
#include "uninstall_inventory.hpp"
#include "elevation_request.hpp"
#include "verified_removal.hpp"
#include "workspace_registration.hpp"
#include "machine_gate.hpp"
#include "payload_authority.hpp"
#include "workspace_session.hpp"
#include "uninstall_journal.hpp"
#include "uninstall_metadata.hpp"
#include "native_command.hpp"
#include "native_report.hpp"
#include "uninstall_execution.hpp"
#include "full_uninstall.hpp"
#include <future>
#include <thread>
#include <sqlite3.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <map>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#endif

using namespace marksix::maintenance;
namespace {
int assertions = 0;
void check(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
template<class Action> void rejects(Action action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
struct Fixture {
    fs::path root;
    Fixture() {
        const auto base = fs::canonical(fs::temp_directory_path());
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto name = "marksix-inventory-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(attempt);
            std::error_code error;
            if (fs::create_directory(base / name, error)) { root = base / name; break; }
        }
        if (root.empty()) throw std::runtime_error("Cannot reserve isolated fixture");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
    UserStorage user() const {
        return {"fixture-user", root / "home", root / "home/data", root / "home/cache",
            root / "home/Downloads", root / "home/maintenance/ownership.bin", {}, {}, true};
    }
};
void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary); out << text;
    if (!out) throw std::runtime_error("Fixture write failed");
}
void database(const fs::path& file, int app = 0x4d365354, int version = 3, const std::string& tables = "sources,revisions,draws") {
    fs::create_directories(file.parent_path());
    sqlite3* db = nullptr;
    if (sqlite3_open(pathUtf8(file).c_str(), &db) != SQLITE_OK) throw std::runtime_error("Fixture database failed");
    auto sql = "PRAGMA application_id=" + std::to_string(app) + ";PRAGMA user_version=" + std::to_string(version) + ";";
    std::size_t position = 0;
    do {
        const auto end = tables.find(',', position);
        sql += "CREATE TABLE " + tables.substr(position, end - position) + "(id INTEGER);";
        if (end == std::string::npos) break;
        position = end + 1;
    } while (true);
    const auto result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (result != SQLITE_OK) throw std::runtime_error("Fixture schema failed");
}
bool has(const Inventory& inventory, const fs::path& file) {
    return std::any_of(inventory.targets.begin(), inventory.targets.end(), [&](const auto& item) { return item.path == file; });
}
std::map<fs::path, std::string> contents(const fs::path& root) {
    std::map<fs::path, std::string> hashes;
    if (!fs::exists(root)) return hashes;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (fs::is_regular_file(entry.symlink_status())) hashes[entry.path()] = sha256File(entry.path());
    return hashes;
}
void manifestTests() {
    Fixture fixture; const auto user = fixture.user();
    check(readManifest(user.manifestPath) == OwnershipManifest{}, "Missing manifest is empty");
    OwnershipManifest manifest;
    rememberDataRoot(manifest, fixture.root / "custom \"root\" with spaces");
    rememberDataRoot(manifest, fixture.root / "custom \"root\" with spaces");
    check(manifest.dataRoots.size() == 1, "Root registration deduplicates");
    rememberDataRoot(manifest, fixture.root / fs::path(u8"中文\n資料"));
    manifest.createdDirectories.push_back(user.dataRoot / "statistics");
    const auto download = user.downloads / "verified.pkg";
    write(download, "abc"); rememberDownload(manifest, download);
    check(manifest.downloads[0].sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "Native streamed SHA-256 known answer");
    writeManifest(user.manifestPath, manifest);
    check(readManifest(user.manifestPath) == manifest, "Manifest preserves Unicode, newlines and file identities");
    rememberDataRoot(manifest, fixture.root / "second");
    writeManifest(user.manifestPath, manifest);
    check(readManifest(user.manifestPath) == manifest, "Atomic manifest replacement");
    const auto before = contents(fixture.root);
    readManifest(user.manifestPath);
    check(contents(fixture.root) == before, "Manifest discovery never mutates files");
    write(user.manifestPath, "M6OWNERSHIP\n999\n");
    rejects([&] { readManifest(user.manifestPath); }, "Unknown manifest version is not silently empty");
    write(user.manifestPath, "M6OWNERSHIP\n1\n\xff\xff\xff\xff");
    rejects([&] { readManifest(user.manifestPath); }, "Unbounded entry count rejected");
    rejects([&] { rememberDataRoot(manifest, "relative"); }, "Relative manifest paths rejected");
    rejects([&] { pathFromUtf8(std::string("a\0b", 3)); }, "NUL in decoded path rejected");
    rejects([&] { pathFromUtf8(std::string("\xc0\xaf", 2)); }, "Overlong UTF-8 cannot disguise a separator");
    rejects([&] { pathFromUtf8(std::string("\xed\xa0\x80", 3)); }, "UTF-8 surrogate code points rejected");
}
void inventoryTests() {
    Fixture fixture; const auto user = fixture.user();
    const auto archive = user.dataRoot / "statistics/results.sqlite3";
    const auto adaptive = user.dataRoot / "statistics/adaptive.sqlite3";
    const auto lab = user.dataRoot / "statistics/forecast_lab.sqlite3";
    database(archive); database(adaptive, 0x4d364144, 1, "lineages,predictions,head");
    database(lab, 0x4d36464c, 1, "lineages,forecasts,head");
    write(user.dataRoot / "personal.txt", "not app data");
    write(user.dataRoot / "preserved.msx", "unrecognized recording sentinel");
    write(user.downloads / "MarkSixStudio-fake.pkg", "unverified download");
    write(user.cacheRoot / "graphics/cache.bin", "regenerable");
    const auto backup = user.dataRoot / "statistics/results.sqlite3.before-v3-0123456789abcdef01234567.sqlite3";
    fs::copy_file(archive, backup);
    write(archive.string() + "-wal", "sidecar fixture");
    const auto before = contents(fixture.root);
    InventoryRequest request{Operation::ResetRecords, {user}, user.dataRoot, {}};
    auto inventory = inspectStorage(request);
    check(inventory.complete(), "Known owned storage resolves completely");
    check(inventory.targets.size() == 5, "Archive, models, sidecar and migration backup inventoried");
    check(has(inventory, archive) && has(inventory, adaptive) && has(inventory, lab) && has(inventory, backup), "All data categories included");
    check(!has(inventory, user.dataRoot / "personal.txt") && !has(inventory, user.dataRoot / "preserved.msx"), "Foreign documents and recordings preserved");
    check(contents(fixture.root) == before, "Read-only inventory leaves database bytes, sidecars and files unchanged");
    check(stillMatches(inventory.targets[0]), "Original file identity matches");
    write(inventory.targets[0].path, "changed");
    check(!stillMatches(inventory.targets[0]), "Content mutation invalidates approval");
    request.operation = Operation::ClearCache;
    inventory = inspectStorage(request);
    check(inventory.complete() && inventory.targets.size() == 1, "Cache clearing excludes all saved data and browser downloads");
    const auto cache = user.cacheRoot / "graphics/cache.bin";
    request.protectedDownloads = {cache}; inventory = inspectStorage(request);
    check(!inventory.complete() && !has(inventory, cache), "Active installer lease prevents package removal");
}
void unknownAndBoundaryTests() {
    Fixture fixture; const auto user = fixture.user();
    const auto archive = user.dataRoot / "statistics/results.sqlite3";
    database(archive, 0x1234);
    auto inventory = inspectStorage({Operation::ResetRecords, {user}, user.dataRoot, {}});
    check(!inventory.complete() && !has(inventory, archive), "Unknown app ID preserved and reported");
    fs::remove(archive); database(archive, 0x4d365354, 99);
    inventory = inspectStorage({Operation::ResetRecords, {user}, user.dataRoot, {}});
    check(!inventory.complete() && !has(inventory, archive), "Future schema preserved and reported");
    fs::remove(archive); database(archive, 0x4d365354, 3, "unrelated");
    inventory = inspectStorage({Operation::ResetRecords, {user}, user.dataRoot, {}});
    check(!inventory.complete() && !has(inventory, archive), "Matching pragma without product tables is insufficient");
    rejects([&] { validateBoundary(user.home, user.home); }, "Whole home rejected");
    rejects([&] { validateBoundary(user.home.root_path(), user.home); }, "Filesystem root rejected");
    rejects([&] { validateContainedPath(user.dataRoot / "../foreign", user.dataRoot); }, "Parent traversal rejected");
    rejects([&] { validateContainedPath(user.home / "data-other/file", user.dataRoot); }, "Prefix collision rejected");
    rejects([&] { validateContainedPath(user.dataRoot, user.dataRoot); }, "Entire data root cannot be a file deletion target");
    const auto outside = fixture.root / "outside"; fs::create_directory(outside);
    write(outside / "valuable", "preserve");
    std::error_code error; fs::create_directory_symlink(outside, user.cacheRoot, error);
    if (!error) {
        inventory = inspectStorage({Operation::ClearCache, {user}, user.dataRoot, {}});
        check(!inventory.complete() && inventory.targets.empty(), "Symlinked cache root is not traversed");
        check(sha256File(outside / "valuable") == identifyFile(outside / "valuable").sha256, "Outside sentinel remains intact");
    } else {
#ifdef _WIN32
        std::cout << "Symlink fixture unavailable: native reparse acceptance remains pending\n";
#else
        throw std::runtime_error("Cannot create required symbolic-link fixture");
#endif
    }
}
void profileTests() {
    Fixture fixture;
    const NativeProfile first{"SID-one", fixture.root / "first", fixture.root / "redirected/local", fixture.root / "redirected/downloads", true};
    const NativeProfile administrator{"SID-admin", fixture.root / "admin", fixture.root / "admin/local", fixture.root / "admin/downloads", true};
    const auto original = storageForProfile(first, HostPlatform::Windows);
    const auto elevated = storageForProfile(administrator, HostPlatform::Windows);
    check(original.account == "SID-one" && elevated.account == "SID-admin", "Profile identity survives alternative-administrator elevation");
    check(original.dataRoot == first.localData / "MarkSixEmulator" && original.downloads == first.downloads, "Redirected profile locations used without elevated environment substitution");
    check(original.dataRoot != elevated.dataRoot, "Original user is never replaced by authorizing administrator");
    const auto offline = storageForProfile({"SID-offline", fixture.root / "offline", {}, {}, false}, HostPlatform::Windows);
    check(!offline.locationsResolved, "Unavailable known folders are not treated as resolved defaults");
    const auto inspection = inspectStorage({Operation::Uninstall, {offline}, {}, {}});
    check(!inspection.complete() && !inspection.issues.empty(), "Offline account remains an explicit coverage failure");
    const auto mac = storageForProfile({"501", fixture.root / "mac-user", {}, {}, true}, HostPlatform::MacOS);
    check(mac.dataRoot == mac.home / "Library/Application Support/MarkSixStudio", "macOS saved-data path matches application resolver");
    check(mac.cacheRoot == mac.home / "Library/Caches/Mark Six Studio", "macOS cache path is separate from saved data");
    check(mac.manifestPath == mac.dataRoot / "maintenance/ownership.bin", "Ownership manifest lives outside reset target");
    const auto before = contents(fixture.root);
    std::vector<InventoryIssue> issues;
    const auto current = discoverUsers(false, issues);
    check(!current.empty(), "Native initiating account can be resolved read-only");
    check(contents(fixture.root) == before, "Profile discovery creates no fixture storage");
}
void applicationIdentityTests() {
    Fixture fixture;
    const auto user = fixture.user();
    const auto source = fixture.root / "source";
    fs::create_directories(source / "build"); write(source / ".git", "gitdir: private-worktree");
    check(isDevelopmentLocation(source / "build/copied.app"), "A worktree registration cannot make developer sources uninstall targets");
    const auto build = fixture.root / "separate-build";
    write(build / "CMakeCache.txt", "CMAKE_BUILD_TYPE:STRING=Release");
    check(isDevelopmentLocation(build / "mark-six-studio.app"), "Out-of-tree CMake builds are excluded");
#ifdef __APPLE__
    const auto bundle = user.home / "Applications/Mark Six Studio.app";
    const auto plist = bundle / "Contents/Info.plist";
    const auto metadata = [](const std::string& identifier) {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict>"
            "<key>CFBundleIdentifier</key><string>" + identifier + "</string>"
            "<key>CFBundleExecutable</key><string>mark-six-studio</string>"
            "<key>CFBundlePackageType</key><string>APPL</string></dict></plist>";
    };
    write(plist, metadata("com.brucerry.marksixstudio"));
    write(bundle / "Contents/MacOS/mark-six-studio", std::string("\xca\xfe\xba\xbe", 4));
    check(hasProductIdentity(bundle), "Native bundle metadata recognizes the product without loading its executable");
    const auto before = contents(fixture.root);
    std::vector<InventoryIssue> issues;
    const auto copies = discoverApplications({user}, issues);
    check(std::any_of(copies.begin(), copies.end(), [&](const auto& copy) { return copy.root == bundle && copy.account == user.account; }),
          "Per-user Applications copy retains the correct account identity");
    check(contents(fixture.root) == before, "Native installed-copy discovery is read-only");
    write(plist, metadata("org.example.unrelated"));
    check(!hasProductIdentity(bundle), "Matching filename with another bundle ID is not the product");
    write(plist, metadata("com.brucerry.marksixstudio"));
    write(bundle / "Contents/MacOS/mark-six-studio", "personal document");
    check(!hasProductIdentity(bundle), "Bundle metadata alone does not make a document an application payload");
#else
    check(!hasProductIdentity(fixture.root / "absent-installation"), "Missing Windows product is not a valid installed copy");
#endif
}
void payloadTests() {
#ifdef __APPLE__
    Fixture fixture;
    const auto bundle = fixture.user().home / "Applications/Mark Six Studio.app";
    const auto plist = bundle / "Contents/Info.plist";
    const auto executable = bundle / "Contents/MacOS/mark-six-studio";
    const auto manifest = bundle / "Contents/Resources/maintenance-payload.txt";
    const auto binary = std::string("\xca\xfe\xba\xbe", 4);
    write(plist, "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>CFBundleIdentifier</key><string>com.brucerry.marksixstudio</string>"
        "<key>CFBundleExecutable</key><string>mark-six-studio</string>"
        "<key>CFBundlePackageType</key><string>APPL</string></dict></plist>");
    write(executable, binary);
    const auto baseline = "M6PAYLOAD\t1\tcom.brucerry.marksixstudio\n"
        "D\t-\tContents\nD\t-\tContents/MacOS\nD\t-\tContents/Resources\n"
        "F\t" + sha256File(plist) + "\tContents/Info.plist\nF\t" + sha256File(executable) + "\tContents/MacOS/mark-six-studio\n";
    write(manifest, baseline);
    const InstalledApplication application{bundle, executable, fixture.user().account, "com.brucerry.marksixstudio"};
    const auto before = contents(fixture.root);
    auto payload = inspectApplicationPayload(application, {fixture.user().home});
    const auto unsafe = inspectApplicationPayload(application, {application.root});
    check(!unsafe.inventory.complete() && unsafe.inventory.targets.empty(), "An application manifest cannot turn an OS home directory into a payload boundary");
    check(payload.inventory.complete() && payload.inventory.targets.size() == 3, "Payload manifest includes exact owned files and its own manifest");
    check(contents(fixture.root) == before, "Payload discovery never rewrites bundle files");
    const auto legacy = bundle / "Contents/Resources/retained.MSX";
    write(legacy, "old opaque recording");
    write(manifest, baseline + "F\t" + std::string(64, '0') + "\tContents/Resources/retained.MSX\n");
    payload = inspectApplicationPayload(application, {fixture.user().home});
    check(payload.inventory.complete() && intentionallyPreserved(payload.inventory, legacy) && !has(payload.inventory, legacy),
          "Legacy preservation overrides a payload manifest without reading the recording hash");
    check(std::find(payload.directories.begin(), payload.directories.end(), bundle) == payload.directories.end(),
          "Bundle container with preserved recordings cannot be removed");
    check(preservedLegacyReport(payload.inventory).find(pathUtf8(legacy)) != std::string::npos, "Retained bundle file has a clearly listed full path");
    fs::remove(legacy); write(manifest, baseline);
    const auto foreign = bundle / "Contents/Resources/personal.txt";
    write(foreign, "unrelated personal document");
    payload = inspectApplicationPayload(application, {fixture.user().home});
    check(!payload.inventory.complete() && !has(payload.inventory, foreign), "Unknown bundle content is retained and prevents an unqualified complete result");
    fs::remove(foreign);
    write(executable, binary + " changed");
    payload = inspectApplicationPayload(application, {fixture.user().home});
    check(!payload.inventory.complete() && !has(payload.inventory, executable), "Changed native payload is not approved under an old content digest");
    write(executable, binary);
    const auto link = bundle / "Contents/Resources/runtime-link";
    fs::create_symlink("../MacOS/mark-six-studio", link);
    write(manifest, baseline + "L\t../MacOS/mark-six-studio\tContents/Resources/runtime-link\n");
    payload = inspectApplicationPayload(application, {fixture.user().home});
    check(payload.inventory.complete() && has(payload.inventory, link), "Declared internal link is inventoried without following it");
    const auto found = std::find_if(payload.inventory.targets.begin(), payload.inventory.targets.end(), [&](const auto& target) { return target.path == link; });
    check(found != payload.inventory.targets.end() && stillMatches(*found), "Declared link identity is revalidatable");
    const auto oldLink = *found;
    fs::remove(link); fs::create_symlink("../Info.plist", link);
    check(!stillMatches(oldLink), "Replacing a payload link invalidates approval");
    fs::remove(link);
    const auto outside = fixture.root / "outside.txt"; write(outside, "outside sentinel");
    const auto sentinelHash = sha256File(outside);
    fs::create_symlink(outside, link);
    write(manifest, baseline + "L\t" + pathUtf8(outside) + "\tContents/Resources/runtime-link\n");
    payload = inspectApplicationPayload(application, {fixture.user().home});
    check(!payload.inventory.complete() && !has(payload.inventory, link), "External payload link is rejected");
    check(sha256File(outside) == sentinelHash, "External link target remains untouched");
    fs::remove(link);
    write(manifest, baseline + "F\t" + sentinelHash + "\t../outside.txt\n");
    payload = inspectApplicationPayload(application, {fixture.user().home});
    check(!payload.inventory.complete() && !has(payload.inventory, outside), "Manifest traversal cannot authorize a file outside the product");
    write(manifest, baseline + "F\t" + sentinelHash + "\tContents/Resources/bad%00path\n");
    check(!inspectApplicationPayload(application, {fixture.user().home}).inventory.complete(), "Encoded NUL in payload path rejected");
#endif
}
void windowsPayloadTests() {
#ifdef _WIN32
    Fixture fixture;
    const auto root = fixture.root / "installed-fixture";
    fs::create_directories(root);
    std::vector<wchar_t> module(32768);
    const auto count = GetModuleFileNameW(nullptr, module.data(), DWORD(module.size()));
    check(count && count < module.size(), "Resolve copied native Windows fixture binary");
    const auto executable = root / "mark-six-studio.exe";
    fs::copy_file(fs::path(std::wstring(module.data(), count)), executable);
    const auto owned = root / "owned.txt"; write(owned, "owned payload fixture");
    const auto manifest = root / "maintenance-payload.txt";
    const auto baseline = "M6PAYLOAD\t1\tcom.brucerry.marksixstudio\nF\t" + sha256File(executable) +
        "\tmark-six-studio.exe\nF\t" + sha256File(owned) + "\towned.txt\n";
    write(manifest, baseline);
    const InstalledApplication application{root, executable, "fixture-account", "fixture-registration"};
    check(hasProductIdentity(root), "Native Windows version resources identify the copied fixture");
    auto payload = inspectApplicationPayload(application, {fixture.user().home});
    check(payload.inventory.complete() && payload.inventory.targets.size() == 3, "Windows payload manifest identifies exact owned files");
    std::ostringstream metadata;
    metadata << "{\"format\":\"marksix-statistics-package-v1\",\"qtVersion\":\"6.10.2\",\"files\":[";
    bool comma = false;
    for (const auto& target : payload.inventory.targets) {
        if (comma) metadata << ','; comma = true;
        metadata << "{\"path\":" << quotedReportField(pathUtf8(target.path.lexically_relative(root))) << ",\"sha256\":" << quotedReportField(target.identity.sha256) << '}';
    }
    metadata << "]}"; write(root / "manifest.json", metadata.str());
    payload = inspectApplicationPayload(application, {fixture.user().home});
    check(payload.inventory.complete() && has(payload.inventory, root / "manifest.json"), "Typed Windows package metadata is attributable without a cyclic manifest hash");
    write(root / "manifest.json", "{\"format\":\"personal-manifest\",\"files\":[]}");
    payload = inspectApplicationPayload(application, {fixture.user().home});
    check(!payload.inventory.complete() && !has(payload.inventory, root / "manifest.json"), "Generic same-named manifests remain protected");
    fs::remove(root / "manifest.json");
    const auto legacy = root / "legacy.MSX"; write(legacy, "old opaque recording");
    write(manifest, baseline + "F\t" + std::string(64, '0') + "\tlegacy.MSX\n");
    payload = inspectApplicationPayload(application, {fixture.user().home});
    check(payload.inventory.complete() && intentionallyPreserved(payload.inventory, legacy) && !has(payload.inventory, legacy),
          "Windows payload manifest cannot override legacy preservation");
    check(payload.directories.empty(), "Windows install container holding legacy files is retained");
    check(preservedLegacyReport(payload.inventory).find(pathUtf8(legacy)) != std::string::npos, "Windows final report lists the preserved path");
    const auto foreign = root / "personal.txt"; write(foreign, "foreign document");
    payload = inspectApplicationPayload(application, {fixture.user().home});
    check(!payload.inventory.complete() && !has(payload.inventory, foreign), "Windows foreign payload contents remain protected");
    fs::remove(foreign);
    write(owned, "changed");
    payload = inspectApplicationPayload(application, {fixture.user().home});
    check(!payload.inventory.complete() && !has(payload.inventory, owned), "Changed Windows payload identity requires fresh review");
#endif
}
void preservedLegacyTests() {
    Fixture fixture;
    auto first = fixture.user(); first.account = "first-account";
    auto second = first; second.account = "second-account";
    second.home = fixture.root / "second-home"; second.dataRoot = second.home / "data";
    second.cacheRoot = second.home / "cache"; second.downloads = second.home / "Downloads";
    second.manifestPath = second.home / "maintenance/ownership.bin";
    const auto old = first.dataRoot / "old.msx";
    const auto upper = first.dataRoot / "nested/OLDER.MsX";
    const auto cached = first.cacheRoot / "cached.MSX";
    const auto other = second.dataRoot / "old.msx";
    const auto registered = first.dataRoot / "renamed-recording.dat";
    for (const auto& file : {old, upper, cached, other, registered}) write(file, "opaque legacy bytes; deliberately not a known recording format");
    write(first.cacheRoot / "regenerable", "cache");
    OwnershipManifest manifest; manifest.recordings.push_back({registered, sha256File(registered)});
    writeManifest(first.manifestPath, manifest);
    database(first.dataRoot / "statistics/results.sqlite3");
    const auto before = contents(fixture.root);
    const auto inventory = inspectStorage({Operation::Uninstall, {first, second}, {}, {}});
    check(inventory.complete(), "Intentional preservation is distinct from a cleanup failure");
    check(inventory.preservedLegacy.size() == 5, "All inspected accounts, cache files, mixed-case names and registered legacy hints listed");
    check(contents(fixture.root) == before, "Discovery leaves every legacy byte unchanged");
    for (const auto& file : {old, upper, cached, other, registered}) {
        check(intentionallyPreserved(inventory, file) && !has(inventory, file), "Preserved files are never deletion targets");
        const auto entry = std::find_if(inventory.preservedLegacy.begin(), inventory.preservedLegacy.end(), [&](const auto& item) { return item.path == file; });
        check(entry != inventory.preservedLegacy.end() && !entry->account.empty() && !entry->reason.empty(), "Each preserved item carries its account and reason");
    }
    const auto report = preservedLegacyReport(inventory);
    check(report.find("Preserved legacy files (5)") != std::string::npos && report.find("second-account") != std::string::npos,
          "Report clearly labels legacy preservation and all accounts");
    check(report.find(pathUtf8(upper)) != std::string::npos && report.find("Reason:") != std::string::npos, "Report includes full paths and reasons");
    for (const auto& target : inventory.targets) {
        validateContainedPath(target.path, fixture.root);
        fs::remove(target.path); // Simulate successful current-file cleanup in the private fixture only.
    }
    check(preservedLegacyReport(inventory) == report, "Final preserved report remains available after current-app files disappear");
    for (const auto& file : {old, upper, cached, other, registered}) check(sha256File(file) == before.at(file), "Simulated cleanup preserves legacy bytes");
    Inventory limits;
    discoverPreservedLegacy(limits, old, first.account); // A file cannot be inspected as a directory.
    const auto limited = preservedLegacyReport(limits);
    check(limited.find("Inspection notes") != std::string::npos && limited.find(pathUtf8(old)) != std::string::npos,
          "Incomplete inspection is disclosed alongside an empty preserved list");
    check(limited.find("inspected locations") != std::string::npos, "Empty result never claims no legacy recordings exist anywhere");
    Inventory escaped;
    preserveLegacyFile(escaped, first.dataRoot / fs::path("line\nAccount: forged.msx"), "account\nforged");
    const auto escapedReport = preservedLegacyReport(escaped);
    check(escapedReport.find("line\\nAccount: forged.msx") != std::string::npos && escapedReport.find("account\\nforged") != std::string::npos,
          "Control characters cannot spoof report fields");
    check(quotedReportField("line\xe2\x80\xa8" "forged\xe2\x80\xae").find("\\u2028forged\\u202e") != std::string::npos,
          "Unicode line and direction controls cannot disguise report fields");
#ifndef _WIN32
    Inventory nontext;
    preserveLegacyFile(nontext, first.dataRoot / fs::path(std::string("nontext-") + char(0xff) + ".msx"), first.account);
    check(preservedLegacyReport(nontext).find("nontext-\\u00ff.msx") != std::string::npos, "Non-text filename bytes remain clearly represented in preserved reports");
    const auto fifo = first.dataRoot / "unreadable-content.msx";
    check(mkfifo(fifo.c_str(), 0600) == 0, "Create no-content-read fixture");
    Inventory noRead;
    discoverPreservedLegacy(noRead, first.dataRoot, first.account);
    check(intentionallyPreserved(noRead, fifo), "Discovery does not open a recording candidate's contents");
    const auto external = fixture.root / "external"; write(external / "outside.msx", "outside root");
    fs::create_directory_symlink(external, first.dataRoot / "outside-link");
    fs::create_symlink(external / "outside.msx", first.dataRoot / "linked.msx");
    Inventory linked;
    discoverPreservedLegacy(linked, first.dataRoot, first.account);
    check(intentionallyPreserved(linked, first.dataRoot / "linked.msx"), "Recording links themselves are clearly listed");
    check(!intentionallyPreserved(linked, external / "outside.msx"), "Linked locations are never traversed");
    check(!linked.issues.empty(), "Skipped external link coverage is disclosed");
#endif
}
void runtimeTests() {
    Fixture fixture;
    const auto path = sessionLeasePath(fixture.user().dataRoot);
    auto first = FileLease::acquire(path, FileLease::Mode::Shared);
    auto second = FileLease::acquire(path, FileLease::Mode::Shared);
    check(first && second, "Application sessions can share a storage lease");
    check(!FileLease::acquire(path, FileLease::Mode::Exclusive), "Maintenance cannot bypass active sessions");
    first.reset();
    check(!FileLease::acquire(path, FileLease::Mode::Exclusive), "Every session must release its lease");
    second.reset();
    auto exclusive = FileLease::acquire(path, FileLease::Mode::Exclusive);
    check(bool(exclusive), "Quiescent storage admits exclusive maintenance");
    check(!FileLease::acquire(path, FileLease::Mode::Shared), "New sessions cannot bypass maintenance");
    exclusive.reset();
    const auto identity = processIdentity(currentProcessId());
    check(identity && !identity->uncertain && identity->account == currentAccount(), "Current process has native account and creation identity");
    check(sameProcessAlive(*identity), "Live process is recognized");
    auto reused = *identity; ++reused.started;
    check(!sameProcessAlive(reused), "PID alone is not sufficient authority");
    MachineGate gate{randomToken(), fixture.root / "root-protected-stage", *identity};
    const auto encodedGate = encodeMachineGate(gate);
    check(encodeMachineGate(decodeMachineGate(encodedGate)) == encodedGate, "System gate binds operation nonce, protected retry location and actual process creation identity");
    rejects([&] { decodeMachineGate(encodedGate + "changed"); }, "Changed system state blocks startup instead of silently opening a workspace");
#ifndef _WIN32
    check(protectedProductFile("/usr/bin/true"), "Native protected-file check accepts an actual root-owned immutable system executable without running it");
    const auto rootProcess = processIdentity(1);
    check(rootProcess && rootProcess->account == "0" && rootProcess->started != 0, "An unprivileged process can bind a root peer to a native creation time without same-user BSD-info access");
#endif
    const auto firstToken = randomToken(), secondToken = randomToken();
    check(firstToken.size() == 64 && firstToken != secondToken, "Operation nonces are distinct 256-bit values");
    const auto alias = path.parent_path() / "hardlink";
    fs::create_hard_link(path, alias);
    check(!FileLease::acquire(alias, FileLease::Mode::Exclusive), "Hard-linked lease files are rejected");
#ifndef _WIN32
    check(normalizePlatformPath("/var/folders/private-fixture") == "/private/var/folders/private-fixture", "Darwin's fixed temporary-directory alias is normalized");
    check(normalizePlatformPath("/tmp/private-fixture") == "/private/tmp/private-fixture", "Darwin's fixed tmp alias is normalized");
    rejects([&] { validateBoundary(normalizePlatformPath("/var"), fixture.user().home); }, "Darwin system aliases cannot bypass broad-root protection");
    check(normalizePlatformPath(fixture.root / "user-link") == fixture.root / "user-link", "User-selected links are never silently canonicalized");
#endif
}
void channelTests() {
    const auto self = processIdentity(currentProcessId());
    check(bool(self), "Native channel test process has an identity");
    std::string endpoint;
    {
        PrivateServer server; endpoint = server.endpoint();
        auto client = std::async(std::launch::async, [&] {
            PrivateChannel channel(server.endpoint(), *self);
            channel.send("M6HELLO1:" + randomToken());
            return channel.receive();
        });
        auto peer = server.accept();
        check(peer->peer() == *self, "Server authenticates kernel PID, creation identity and native account");
        const auto hello = peer->receive();
        check(hello.starts_with("M6HELLO1:"), "Authenticated first frame is retained unchanged");
        peer->send("authenticated result");
        check(client.get() == "authenticated result", "Private channel exchanges bounded native frames");
    }
#ifndef _WIN32
    check(!fs::exists(endpoint) && !fs::exists(fs::path(endpoint).parent_path()), "Native IPC socket and private staging disappear after use");
#endif
    {
        PrivateServer server; auto reused = *self; ++reused.started;
        rejects([&] { PrivateChannel channel(server.endpoint(), reused); }, "A reused server PID cannot authenticate the channel");
    }
    {
        PrivateServer server;
        rejects([&] { PrivateChannel channel(server.endpoint(), *self, {"another-account", false}); }, "A different server account is rejected");
    }
    {
        PrivateServer server({"another-account", false});
        auto client = std::async(std::launch::async, [&] {
            try { PrivateChannel channel(server.endpoint(), *self); channel.send("unapproved peer"); (void)channel.receive(); }
            catch (const std::exception&) {} // A rejected peer may observe the connection close before its first write.
        });
        rejects([&] { (void)server.accept(); }, "Server refuses a client outside its explicit native account policy");
        client.get();
    }
    {
        PrivateServer server;
        rejects([&] { (void)server.accept(std::chrono::milliseconds(10)); }, "An idle native endpoint has a bounded accept timeout");
    }
    {
        PrivateServer server({{}, true});
        auto client = std::async(std::launch::async, [&] {
            try { PrivateChannel channel(server.endpoint(), *self); channel.send("native privilege probe"); return channel.receive() == "authenticated privilege"; }
            catch (const std::exception&) { return false; }
        });
        if (administrator()) {
            auto peer = server.accept();
            check(peer->receive() == "native privilege probe", "Administrator-only server authenticates a real elevated native token");
            peer->send("authenticated privilege");
        } else rejects([&] { (void)server.accept(); }, "A normal OS token cannot claim an administrator-only connection");
        check(client.get() == administrator(), "Native privilege probe remains connected until its authenticated outcome");
    }
}
void resetTests() {
    // Exercise every persistent boundary, including interruption between
    // individual renames/deletes. The next invocation must restore one complete
    // old generation or finish the approved purge, never mix generations.
    for (int stop = 0; stop < 11; ++stop) {
        Fixture fixture; const auto user = fixture.user(); const auto root = user.dataRoot;
        database(root / "statistics/results.sqlite3");
        database(root / "statistics/adaptive.sqlite3", 0x4d364144, 1, "lineages,predictions,head");
        database(root / "statistics/forecast_lab.sqlite3", 0x4d36464c, 1, "lineages,forecasts,head");
        const auto backup = root / "statistics/results.sqlite3.before-v3-0123456789abcdef01234567.sqlite3";
        database(backup);
        const auto legacy = root / "retained.MSX", foreign = root / "statistics/notes.txt";
        write(legacy, "old recording"); write(foreign, "user document");
        const auto before = contents(root);
        const auto inventory = inspectStorage({Operation::ResetRecords, {user}, root, {}});
        check(inventory.complete() && inventory.targets.size() == 4, "Reset inventories archive, both learning ledgers and migration backup");
        const auto digest = inventoryDigest(Operation::ResetRecords, inventory);
        rejects([&] { resetRecords(root, inventory, std::string(64, '0')); }, "An altered reset approval is rejected before staging");
        check(!resetPending(root), "Rejected approval does not create a reset gate");
        int step = 0; bool purging = false; bool interrupted = false;
        try {
            resetRecords(root, inventory, digest, [&](std::string_view point) {
                if (point == "purging") purging = true;
                if (step++ == stop) { interrupted = true; throw std::runtime_error("simulated power loss"); }
            });
        } catch (const std::exception&) { if (!interrupted) throw; }
        if (interrupted) {
            check(resetPending(root), "Interrupted reset remains visibly gated");
            const auto result = recoverReset(root);
            check(result == (purging ? ResetRecovery::Purged : ResetRecovery::Restored), "Recovery follows its durable commit boundary");
        }
        check(!resetPending(root), "Successful recovery removes journal and quarantine");
        for (const auto& target : inventory.targets) {
            if (interrupted && !purging) check(sha256File(target.path) == before.at(target.path), "Pre-purge recovery restores every original database byte");
            else check(!entryExists(target.path), "Committed reset removes all old databases and recovery copies");
        }
        check(sha256File(legacy) == before.at(legacy) && sha256File(foreign) == before.at(foreign), "Reset always preserves legacy recordings and documents");
        check(recoverReset(root) == ResetRecovery::NothingPending, "Completed recovery is idempotent");
    }
    Fixture conflict; const auto user = conflict.user(); const auto root = user.dataRoot;
    database(root / "statistics/results.sqlite3");
    const auto inventory = inspectStorage({Operation::ResetRecords, {user}, root, {}});
    try { resetRecords(root, inventory, inventoryDigest(Operation::ResetRecords, inventory), [](std::string_view point) {
        if (point == "staged") throw std::runtime_error("simulated power loss");
    }); } catch (const std::exception&) {}
    write(root / "statistics/results.sqlite3", "new foreign content");
    rejects([&] { recoverReset(root); }, "Recovery refuses to overwrite a conflicting replacement");
    check(resetPending(root) && readBoundedFile(root / "statistics/results.sqlite3") == "new foreign content", "Failed restoration remains gated and preserves replacements");
    fs::remove(root / "statistics/results.sqlite3");
    check(recoverReset(root) == ResetRecovery::Restored, "Recovery can retry after a conflict is resolved");
    const auto changed = inspectStorage({Operation::ResetRecords, {user}, root, {}});
    write(root / "statistics/results.sqlite3", "substituted");
    rejects([&] { resetRecords(root, changed, inventoryDigest(Operation::ResetRecords, changed)); }, "Changed file identity invalidates approval");
    check(!resetPending(root), "Changed targets are rejected before journal creation");
    check(sha256Bytes("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "Approval hashing uses standard SHA-256");
}
void cacheCleanupTests() {
    Fixture fixture; const auto user = fixture.user();
    database(user.dataRoot / "statistics/results.sqlite3");
    database(user.dataRoot / "statistics/adaptive.sqlite3", 0x4d364144, 1, "lineages,predictions,head");
    database(user.dataRoot / "statistics/forecast_lab.sqlite3", 0x4d36464c, 1, "lineages,forecasts,head");
    write(user.dataRoot / "saved.msx", "saved legacy recording");
    write(user.cacheRoot / "legacy.MSX", "cached legacy recording");
    write(user.cacheRoot / "graphics/shader", "regenerable shader bytes");
    write(user.cacheRoot / "updates/unused-setup.exe", "unused in-app package");
    write(user.downloads / "browser-setup.exe", "browser download");
    const auto before = contents(fixture.root);
    const auto inventory = inspectUserCleanup({Operation::ClearCache, user.dataRoot, false}, user);
    check(inventory.complete() && inventory.targets.size() == 2, "Clear cache selects graphics and unused in-app downloads only");
    check(inventory.preservedLegacy.size() == 2, "Cache review lists legacy recordings in both data and cache roots");
    const auto digest = inventoryDigest(Operation::ClearCache, inventory);
    clearUserCache(inventory, user, digest);
    for (const auto& target : inventory.targets) check(!entryExists(target.path), "Approved cache file was removed");
    for (const auto& [path, hash] : before) if (!has(inventory, path)) check(sha256File(path) == hash, "Cache cleanup byte-preserves databases, learning, legacy and browser downloads");
    check(entryExists(user.cacheRoot) && entryExists(user.cacheRoot / "legacy.MSX"), "Legacy cache container remains intact");
    write(user.cacheRoot / "changed", "before");
    const auto changed = inspectUserCleanup({Operation::ClearCache, user.dataRoot, false}, user);
    write(user.cacheRoot / "changed", "substitution");
    rejects([&] { clearUserCache(changed, user, inventoryDigest(Operation::ClearCache, changed)); }, "Changed cache content is rejected before cleanup");
    check(readBoundedFile(user.cacheRoot / "changed") == "substitution", "Rejected replacement remains byte-preserved");
    rejects([&] { (void)inspectUserCleanup({Operation::Uninstall, user.dataRoot, false}, user); }, "Unprivileged cache/reset executor cannot perform complete uninstall");
}
void installerLeaseTests() {
    Fixture fixture; const auto user = fixture.user();
    const auto package = user.cacheRoot / "updates/update-private/fixture.pkg";
    write(package, "fixture installer bytes; never executed"); write(user.cacheRoot / "shader", "cache");
    const auto path = installerLeasePath(package.parent_path());
    InstallerLease lease{{package, sha256File(package)}, processIdentity(currentProcessId())};
    writeInstallerLease(path, lease);
    const auto read = readInstallerLease(path);
    check(read.package == lease.package && read.process->started == lease.process->started, "Installer lease preserves package identity and native process birth");
    std::vector<InventoryIssue> issues;
    const auto protectedFiles = protectedInstallerFiles(user, issues);
    check(protectedFiles.size() == 2 && !issues.empty(), "Active native installer protects both package and lease");
    check(processState(*lease.process) == ProcessState::Alive, "Native installer process liveness uses creation identity");
    auto inventory = inspectUserCleanup({Operation::ClearCache, user.dataRoot, false}, user);
    check(!inventory.complete() && !has(inventory, package) && !has(inventory, path), "Uncertain handed-off package is excluded and coverage remains unresolved");
    rejects([&] { clearUserCache(inventory, user, inventoryDigest(Operation::ClearCache, inventory)); }, "Active installer blocks destructive cache approval");
    check(readBoundedFile(user.cacheRoot / "shader") == "cache", "Installer blocking occurs before any unrelated cache deletion");
    ++lease.process->started; writeInstallerLease(path, lease);
    check(processState(*lease.process) == ProcessState::Exited, "Reused PID does not identify the original installer");
    issues.clear();
    check(protectedInstallerFiles(user, issues).size() == 2 && !issues.empty(), "A stale PID alone does not prove native installation completion");
    lease.version = "0.2.0"; lease.beforeNativeStamp = std::string(64, 'a'); writeInstallerLease(path, lease);
    InstallerEvidence evidence;
    evidence.process = [](const auto&) { return ProcessState::Exited; };
    evidence.installersIdle = [] { return true; };
    evidence.installation = [] { return NativeInstallationState{"0.2.0", std::string(64, 'b'), true}; };
    issues.clear(); check(protectedInstallerFiles(user, issues, evidence).empty() && std::none_of(issues.begin(), issues.end(), [](const auto& issue) { return issue.unresolved; }),
        "A changed protected native completion proof for the requested version and idle installers releases an exited handoff");
    evidence.installersIdle = [] { return false; }; issues.clear();
    check(protectedInstallerFiles(user, issues, evidence).size() == 2, "Matching receipt cannot release a package while a native installer is active");
    evidence.installersIdle = [] { return true; }; evidence.installation = [] { return NativeInstallationState{"0.2.0", std::string(64, 'a'), true}; }; issues.clear();
    check(protectedInstallerFiles(user, issues, evidence).size() == 2, "Unchanged native metadata is not installation completion evidence");
    evidence.installation = [] { return NativeInstallationState{"0.1.0", std::string(64, 'b'), true}; }; issues.clear();
    check(protectedInstallerFiles(user, issues, evidence).size() == 2, "A different installed version cannot release this handoff");
    lease.process.reset(); writeInstallerLease(path, lease);
    check(!readInstallerLease(path).process, "Unknown process handoff remains representable and protected");
    const auto outside = fixture.root / "foreign.pkg"; write(outside, "foreign");
    rejects([&] { writeInstallerLease(path, {{outside, sha256File(outside)}, {}}); }, "Installer lease cannot name a package outside its private download location");
    write(path, "malformed"); issues.clear(); (void)protectedInstallerFiles(user, issues);
    check(!issues.empty(), "Malformed installer state is disclosed rather than treated idle");
    const auto malformed = inspectUserCleanup({Operation::ClearCache, user.dataRoot, false}, user);
    check(!has(malformed, package) && !has(malformed, path), "Malformed lease preserves the whole uncertain update container even in a partial inventory");
}
void jsonTests() {
    JsonDocument valid("{\"files\":[{\"path\":\"unicode-\\u6e2c\\u8a66\",\"bytes\":12}]}");
    auto query = valid.query("SELECT json_extract(value, '$.path'), json_extract(value, '$.bytes') FROM json_each(?1, '$.files')");
    check(query.next() && query.text(0) == "unicode-測試" && query.integer(1) == 12 && !query.next(), "Pinned read-only JSON parser handles typed fields and Unicode");
    rejects([] { JsonDocument invalid("{\"files\":[],\"files\":[]}"); }, "Duplicate JSON fields cannot conceal alternate metadata");
    rejects([] { JsonDocument invalid("{\"nested\":{\"a\":1,\"a\":2}}"); }, "Nested duplicate JSON fields are rejected");
    rejects([] { JsonDocument invalid("{broken"); }, "Malformed product metadata is rejected");
    rejects([] { JsonDocument invalid("{files:[]} "); }, "Nonstandard JSON5 metadata is rejected");
}
void downloadTests() {
    Fixture fixture; const auto user = fixture.user();
    const auto name = "MarkSixStudio-0.1.0-macos-universal-unsigned.pkg";
    const auto package = user.downloads / name;
    const std::string body = "isolated published-installer fixture; never executed";
    write(package, body); write(user.downloads / "MarkSixStudio-0.1.0-macos-universal-unsigned (1).pkg", body);
    const auto checksumName = "MarkSixStudio-0.1.0-macos-universal-unsigned.sha256";
    const std::string checksum = sha256Bytes(body) + "  " + name + "\n";
    write(user.downloads / checksumName, checksum);
    write(user.downloads / "manifest.json", "generic personal manifest");
    const auto fake = user.downloads / "MarkSixStudio-0.1.1-windows-x64-unsigned-setup.exe";
    write(fake, std::string(body.size(), 'x'));
    write(user.downloads / "old-recording.MSX", "legacy recording");
    const auto metadata = std::string("[{\"draft\":false,\"prerelease\":false,\"tag_name\":\"v0.1.0\",\"html_url\":\"https://github.com/brucerry/mark-six-studio/releases/tag/v0.1.0\",\"assets\":[") +
        "{\"name\":\"" + name + "\",\"state\":\"uploaded\",\"browser_download_url\":\"https://github.com/brucerry/mark-six-studio/releases/download/v0.1.0/" + name +
        "\",\"size\":" + std::to_string(body.size()) + ",\"digest\":\"sha256:" + sha256Bytes(body) + "\"}," +
        "{\"name\":\"" + checksumName + "\",\"state\":\"uploaded\",\"browser_download_url\":\"https://github.com/brucerry/mark-six-studio/releases/download/v0.1.0/" + checksumName +
        "\",\"size\":" + std::to_string(checksum.size()) + ",\"digest\":\"sha256:" + sha256Bytes(checksum) + "\"}]}]";
    ReleaseCatalog catalog; check(appendPublishedAssets(catalog, metadata) == 1 && catalog.complete && catalog.assets.size() == 2,
        "Official repository/tag/platform metadata and digests form a typed installer catalogue");
    const auto before = contents(fixture.root); Inventory inventory;
    inspectDownloads(inventory, {user}, catalog);
    check(has(inventory, package) && has(inventory, user.downloads / checksumName) && inventory.targets.size() == 3,
        "Older installer, identical browser copy and attributable companion are identified individually");
    check(!has(inventory, fake) && !has(inventory, user.downloads / "manifest.json") && !inventory.complete(),
        "A matching filename and generic manifest cannot authorize download deletion");
    check(intentionallyPreserved(inventory, user.downloads / "old-recording.MSX"), "Download review clearly preserves legacy recordings");
    check(contents(fixture.root) == before, "Installer discovery changes no downloaded or personal bytes");
    Inventory leased; inspectDownloads(leased, {user}, catalog, {package});
    check(!has(leased, package), "A handed-off browser installer remains protected");
    ReleaseCatalog offline; offline.complete = false; offline.limits.push_back("offline fixture");
    Inventory unknown; inspectDownloads(unknown, {user}, offline);
    check(unknown.targets.empty() && !unknown.complete(), "Offline verification preserves uncertain candidates and reports incomplete coverage");
    auto hostile = metadata; const auto pos = hostile.find("github.com/brucerry/mark-six-studio/releases/download");
    hostile.replace(pos, std::string("github.com").size(), "example.invalid");
    ReleaseCatalog rejected; appendPublishedAssets(rejected, hostile);
    check(!rejected.complete && rejected.assets.size() == 1, "Untrusted asset origin cannot become product identity");
    auto external = user; external.downloads = fixture.root / "external";
    write(external.downloads / name, body);
    auto withAdditional = user; withAdditional.additionalDownloadRoots.push_back(external.downloads);
    Inventory additional; inspectDownloads(additional, {withAdditional}, catalog);
    check(has(additional, external.downloads / name), "Explicit additional download folder is inspected with the same identity rules");
    const std::string windowsName = "MarkSixStudio-0.1.0-windows-x64-unsigned-setup.exe";
    const std::string windowsBody = "isolated Windows installer; never executed";
    const auto windowsPackage = user.downloads / windowsName;
    write(windowsPackage, windowsBody);
    auto bothPlatforms = catalog;
    bothPlatforms.assets.push_back({windowsName, sha256Bytes(windowsBody), "v0.1.0", windowsBody.size(), AssetRole::Installer});
    Inventory windows; inspectDownloads(windows, {user}, bothPlatforms);
    check(has(windows, windowsPackage), "Older Windows installers are recognized by exact official identity on either native platform");
    write(windowsPackage, std::string(windowsBody.size(), 'x'));
    Inventory changed; inspectDownloads(changed, {user}, bothPlatforms);
    check(!has(changed, windowsPackage), "Same-sized Windows installer substitution is preserved");
    fs::create_directory(external.downloads / ".git");
    Inventory development; inspectDownloads(development, {external}, catalog);
    check(development.targets.empty(), "Developer checkout archives remain outside downloaded-installer cleanup");
}
void payloadAuthorityTests() {
    Fixture fixture;
    const auto trustedRoot = fixture.root / "protected-package-reference", copiedRoot = fixture.root / "unprivileged-copy";
    const auto trustedFile = trustedRoot / "runtime.bin", copiedFile = copiedRoot / "runtime.bin";
    write(trustedFile, "published product runtime"); write(copiedFile, "published product runtime");
    TrustedPayload reference{trustedRoot, {{trustedFile, trustedRoot, {}, Category::Application, "Protected test reference", identifyFile(trustedFile)}}};
    InstalledApplication application{copiedRoot, copiedRoot / "mark-six-studio", "actual-profile", "com.brucerry.marksixstudio"};
    PayloadInspection candidate;
    candidate.inventory.targets.push_back({copiedFile, copiedRoot, application.account, Category::Application, "Editable discovery manifest", identifyFile(copiedFile)});
    const auto legacy = copiedRoot / "opaque.MSX"; write(legacy, "retained opaque recording");
    preserveLegacyFile(candidate.inventory, legacy, application.account);
    PayloadAuthority authority; authority.codeValid = [](const auto&) { return true; };
    const auto accepted = authorizePayload(application, candidate, {reference}, authority);
    check(accepted.complete() && has(accepted, copiedFile) && intentionallyPreserved(accepted, legacy),
        "A copied runtime is attributable by an independently protected reference while the full legacy list remains preserved");
    const auto untrusted = authorizePayload(application, candidate, {}, authority);
    check(!untrusted.complete() && untrusted.targets.empty() && intentionallyPreserved(untrusted, legacy),
        "An editable payload manifest alone cannot authorize privileged removal");
    write(copiedFile, "user-controlled substitute"); candidate.inventory.targets.front().identity = identifyFile(copiedFile);
    const auto replaced = authorizePayload(application, candidate, {reference}, authority);
    check(replaced.targets.empty() && !replaced.complete(), "Refreshed client hashes cannot turn substituted content into trusted product ownership");
    authority.codeValid = [](const auto&) { return false; };
    check(authorizePayload(application, candidate, {reference}, authority).targets.empty(), "Native product code failure blocks payload removal independently of metadata");
    if (!administrator()) check(!protectedProductFile(copiedFile), "An ordinary user-owned payload does not become protected installation metadata");
}
void workspaceRegistrationTests() {
    Fixture fixture; const auto user = fixture.user();
    const auto selected = fixture.root / "custom user archive";
    check(rememberWorkspace(user, selected), "Future custom-root discovery is registered under its own native profile");
    check(readManifest(user.manifestPath).dataRoots == std::vector<fs::path>{selected}, "Registration stores exact selected roots as bounded discovery hints");
    const auto before = identifyFile(user.manifestPath);
    rememberWorkspace(user, selected);
    check(identifyFile(user.manifestPath) == before, "Repeated startup does not rewrite unchanged ownership metadata");
    const auto development = fixture.root / "developer"; fs::create_directories(development / ".git");
    check(!rememberWorkspace(user, development / "build-data") && readManifest(user.manifestPath).dataRoots.size() == 1,
        "Developer checkout data is not registered as an installed-product uninstall target");
    database(development / "build-data/statistics/results.sqlite3");
    auto explicitUser = user; explicitUser.additionalDataRoots.push_back(development / "build-data");
    const auto full = inspectStorage({Operation::Uninstall, {explicitUser}, {}, {}});
    check(!has(full, development / "build-data/statistics/results.sqlite3"), "Complete uninstall cannot remove source/build archive records even through an explicit root hint");
    const auto active = inspectStorage({Operation::ResetRecords, {user}, development / "build-data", {}});
    check(has(active, development / "build-data/statistics/results.sqlite3"), "A developer can deliberately reset the active generated workspace through separate final consent");
    rejects([&] { rememberWorkspace(user, user.home); }, "Workspace registration cannot claim an entire home");
    write(user.manifestPath, "unreadable ownership version");
    rejects([&] { rememberWorkspace(user, selected); }, "Startup never overwrites an unsupported ownership manifest");
    check(readBoundedFile(user.manifestPath) == "unreadable ownership version", "Unsupported metadata is preserved for review");
}
void nativeReportTests() {
    PrivateServer server; const auto self = *processIdentity(currentProcessId()); const auto nonce = randomToken();
    std::string text = "Preserved legacy files\n";
    for (int i = 0; i < 2000; ++i) text += "Account: fixture user\nPath: /private/fixture/legacy-recording-" + std::to_string(i) + ".msx\nReason: preserved without reading.\n";
    auto client = std::async(std::launch::async, [&] {
        PrivateChannel channel(server.endpoint(), self); channel.setTimeout(std::chrono::minutes(5));
        channel.send("native report peer preface");
        return receiveNativeReport(channel, nonce);
    });
    auto channel = server.accept(); (void)channel->receive();
    NativeReport report{"review", std::string(64, 'a'), text, true, false};
    sendNativeReport(*channel, nonce, report); const auto received = client.get();
    check(received.text == text && received.approvalDigest == report.approvalDigest && received.incomplete,
        "Full preserved lists larger than a path field cross native IPC without truncation or digest substitution");
    rejects([&] { channel->setTimeout(std::chrono::seconds(0)); }, "Unbounded or invalid native I/O deadlines are rejected");
}
void nativeCommandTests() {
    const auto self = processIdentity(currentProcessId()); check(bool(self), "Native command fixture has an image identity");
    const std::string literal = "literal $(no-shell-expansion); 'quote' \"double\" \\ trailing\\ 測試";
    const auto result = runNativeCommand(self->executable, {"--echo-native-argument", literal});
    check(result.status == 0 && result.output == literal, "Explicit native argv preserves Unicode, quotes, backslashes and shell metacharacters without source expansion");
    rejects([&] { runNativeCommand(self->executable, {std::string("unsafe\0argument", 15)}); }, "Embedded argument terminators are rejected before process launch");
    rejects([&] { runNativeCommand(self->executable, {"--delay-native-command"}, std::chrono::seconds(1)); }, "Native command timeout affects only its owned child and reports unverified completion");
}
void uninstallMetadataTests() {
    Fixture fixture; const auto user = fixture.user();
    check(rememberWorkspace(user, user.dataRoot), "Create isolated native ownership metadata");
    { auto lease = FileLease::acquire(sessionLeasePath(user.dataRoot), FileLease::Mode::Shared); check(bool(lease), "Create a real workspace coordination marker"); }
    Inventory inventory; inspectUninstallMetadata(inventory, {user});
    check(inventory.complete() && has(inventory, user.manifestPath) && has(inventory, user.manifestPath.parent_path() / ".ownership.lock") && has(inventory, sessionLeasePath(user.dataRoot)),
        "Full cleanup includes its typed ownership file and native coordination markers");
    auto writer = FileLease::acquire(sessionLeasePath(user.dataRoot), FileLease::Mode::Shared);
    Inventory busy; inspectUninstallMetadata(busy, {user});
    check(!busy.complete() && !has(busy, sessionLeasePath(user.dataRoot)), "A live workspace marker cannot become a deletion target");
    writer.reset();
#ifdef __APPLE__
    const auto saved = user.home / "Library/Saved Application State/com.brucerry.marksixstudio.savedState";
    const auto state = saved / "windows.plist", legacy = saved / "old.MSX";
    const auto preferences = user.home / "Library/Preferences/com.brucerry.marksixstudio.plist";
    write(state, "native state fixture"); write(legacy, "opaque old recording"); write(preferences, "product preference fixture");
    Inventory native; inspectUninstallMetadata(native, {user});
    check(has(native, state) && has(native, preferences) && !has(native, legacy) && intentionallyPreserved(native, legacy),
        "Native preferences/saved state are bounded by bundle identity and preserve the full legacy list");
#endif
}
void uninstallJournalTests() {
    Fixture fixture; const auto user = fixture.user();
    const auto file = user.dataRoot / "statistics/results.sqlite3"; write(file, "isolated owned payload fixture");
    const auto legacy = user.dataRoot / "retained.MSX"; write(legacy, "opaque preserved recording");
    const auto legacyHash = sha256File(legacy);
    UninstallInspection inspection; inspection.originalAccount = user.account; inspection.users = {user};
    inspection.inventory.targets.push_back({file, user.dataRoot, user.account, Category::Records, "Native fixture authority", identifyFile(file)});
    preserveLegacyFile(inspection.inventory, legacy, user.account);
    const auto stage = fixture.root / "private-operation-stage"; makePrivateDirectory(stage);
    auto journal = UninstallJournal::create(stage, randomToken(), {}, inspection);
    check(!journal.executionStarted() && !UninstallJournal::open(stage).executionStarted(), "A preflight-only journal durably certifies that deletion never began");
    rejects([&] { journal.execute(); }, "A durable uninstall journal is never deletion consent by itself");
    const auto digest = journal.reviewDigest();
    journal.approve(digest);
    auto restarted = UninstallJournal::open(stage);
    rejects([&] { restarted.execute(); }, "Uninstall recovery never reuses another process's approval");
    check(uninstallReview(restarted.inspection()).find(quotedReportField(pathUtf8(legacy))) != std::string::npos,
        "The full preserved-file report remains available from standalone durable recovery");
    write(file, "changed after native final review");
    rejects([&] { journal.execute(); }, "A changed target invalidates whole-plan approval before uninstall starts");
    check(entryExists(file) && journal.currentStates().front() == RemovalState::Changed, "Changed files remain intact for fresh ownership review");
    fs::remove(stage / "ledger.bin"); inspection.inventory.targets.front().identity = identifyFile(file);
    journal = UninstallJournal::create(stage, randomToken(), {}, inspection); journal.approve(journal.reviewDigest());
#ifndef _WIN32
    journal.execute({}, [](std::string_view point) { if (point == "staged") throw std::runtime_error("isolated process interruption"); });
    check(!journal.filesRemoved() && journal.currentStates().front() == RemovalState::Staged, "An interrupted uninstall retains its exact staged target and readable failure");
    restarted = UninstallJournal::open(stage); restarted.approve(restarted.reviewDigest()); restarted.execute();
#else
    journal.execute(); restarted = UninstallJournal::open(stage);
#endif
    check(restarted.filesRemoved() && !entryExists(file) && sha256File(legacy) == legacyHash, "Uninstall file recovery finishes exact approved removal while preserving legacy bytes");
    check(restarted.executionStarted() && UninstallJournal::open(stage).executionStarted(), "Execution remains durable after interruption and completed file removal");
    const auto original = readBoundedFile(stage / "ledger.bin"); auto corrupt = original; corrupt[40] ^= 1; durableWrite(stage / "ledger.bin", corrupt);
    rejects([&] { (void)UninstallJournal::open(stage); }, "Corrupted durable uninstall state cannot authorize retry");
}
void resetPermissionTests() {
#ifndef _WIN32
    if (administrator()) return; // Root intentionally bypasses POSIX mode denial.
#endif
    Fixture fixture; const auto user = fixture.user(); const auto root = user.dataRoot;
    database(root / "statistics/results.sqlite3");
    database(root / "statistics/adaptive.sqlite3", 0x4d364144, 1, "lineages,predictions,head");
    const auto inventory = inspectStorage({Operation::ResetRecords, {user}, root, {}});
    const auto before = contents(root);
    const auto directory = root / "statistics";
#ifdef _WIN32
    struct Permissions {
        fs::path path; std::wstring nativePath; PSECURITY_DESCRIPTOR descriptor = nullptr; PACL original = nullptr; bool changed = false;
        explicit Permissions(fs::path value) : path(std::move(value)), nativePath(path.native()) {
            if (GetNamedSecurityInfoW(nativePath.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &original, nullptr, &descriptor) != ERROR_SUCCESS)
                throw std::runtime_error("Cannot snapshot isolated fixture permissions");
        }
        void deny() {
            PSID sid = nullptr; PACL denied = nullptr;
            if (!ConvertStringSidToSidW(pathFromUtf8(currentAccount()).c_str(), &sid)) throw std::runtime_error("Cannot resolve fixture SID");
            EXPLICIT_ACCESSW access{}; access.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
            access.grfAccessMode = DENY_ACCESS; access.grfInheritance = NO_INHERITANCE;
            access.Trustee.TrusteeForm = TRUSTEE_IS_SID; access.Trustee.TrusteeType = TRUSTEE_IS_USER;
            access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
            const auto merged = SetEntriesInAclW(1, &access, original, &denied); LocalFree(sid);
            if (merged != ERROR_SUCCESS) throw std::runtime_error("Cannot reserve isolated denial ACL");
            changed = true;
            const auto status = SetNamedSecurityInfoW(nativePath.data(), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, denied, nullptr);
            LocalFree(denied);
            if (status != ERROR_SUCCESS) throw std::runtime_error("Cannot set isolated fixture denial");
        }
        void restore() {
            if (!changed) return;
            SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
            if (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
                SetNamedSecurityInfoW(nativePath.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
                    (control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION), nullptr, nullptr, original, nullptr) != ERROR_SUCCESS)
                throw std::runtime_error("Cannot restore isolated fixture permissions");
            changed = false;
        }
        ~Permissions() { try { restore(); } catch (const std::exception&) {} if (descriptor) LocalFree(descriptor); }
    } permissions(directory);
#else
    struct Permissions {
        fs::path path; fs::perms original;
        explicit Permissions(fs::path value) : path(std::move(value)), original(fs::status(path).permissions()) {}
        void deny() { fs::permissions(path, fs::perms::owner_read | fs::perms::owner_exec); }
        void restore() { fs::permissions(path, original); }
        ~Permissions() { std::error_code error; fs::permissions(path, original, error); }
    } permissions(directory);
#endif
    rejects([&] { resetRecords(root, inventory, inventoryDigest(Operation::ResetRecords, inventory), [&](std::string_view point) {
        if (point == "staged") { permissions.deny(); throw std::runtime_error("Isolated permission failure during reset"); }
    }); }, "A real permission failure interrupts reset without claiming successful purge");
    rejects([&] { (void)recoverReset(root); }, "Failed restoration leaves recoverable staging instead of exposing mixed generations");
    check(resetPending(root), "Permission-blocked recovery retains its durable startup gate");
    rejects([&] { WorkspaceSession session(root); }, "No workspace opens the partially staged saved-data generation");
    permissions.restore();
    check(recoverReset(root) == ResetRecovery::Restored && !resetPending(root), "Retry restores the complete generation after permissions recover");
    for (const auto& [path, hash] : before) check(sha256File(path) == hash, "Permission recovery restores every original database byte");
}
void verifiedRemovalTests() {
    Fixture fixture; const auto user = fixture.user();
    const auto file = user.dataRoot / "statistics/results.sqlite3";
    write(file, "opaque approved-file fixture");
    Target target{file, user.dataRoot, user.account, Category::Records, "Isolated fixture", identifyFile(file)};
    const auto staging = removalStagingPath(target, randomToken(), 0);
    const auto legacy = user.dataRoot / "preserved.msx"; write(legacy, "opaque old recording");
    const auto legacyHash = sha256File(legacy);
    write(file, "changed file with same old approval");
    rejects([&] { removeVerifiedTarget(target, staging); }, "Exact native removal refuses changed approved bytes");
    check(entryExists(file) && !entryExists(staging), "Changed files are left in place without an orphan staging directory");
    target.identity = identifyFile(file);
#ifdef _WIN32
    const auto lock = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "Reserve a native locked approved-file fixture");
    rejects([&] { removeVerifiedTarget(target, staging); }, "Locked native file objects are preserved for retry");
    CloseHandle(lock);
#else
    rejects([&] { removeVerifiedTarget(target, staging, [&](std::string_view point) {
        if (point == "staged") throw std::runtime_error("isolated interruption after durable quarantine");
    }); }, "Interrupted native removal retains the exact staged file");
    check(!entryExists(file) && entryExists(staging / "item"), "The interrupted removal step is recoverable through its planned quarantine");
#endif
    removeVerifiedTarget(target, staging);
    check(!entryExists(file) && !entryExists(staging), "Native exact-object removal/retry leaves no quarantine");
    check(sha256File(legacy) == legacyHash, "Native removal preserves unrelated legacy recordings");
    Target preserved{legacy, user.dataRoot, user.account, Category::Cache, "Misleading client claim", identifyFile(legacy)};
    rejects([&] { removeVerifiedTarget(preserved, removalStagingPath(preserved, randomToken(), 1)); }, "Even an asserted target cannot remove a named legacy recording");
#ifndef _WIN32
    write(file, "approved original"); target.identity = identifyFile(file);
    const auto replacedStage = removalStagingPath(target, randomToken(), 1);
    fs::path movedStage = user.dataRoot / "quarantine-renamed-by-another-process";
    std::string movedReport;
    try { removeVerifiedTarget(target, replacedStage, [&](std::string_view point) {
        if (point == "verified") {
            fs::rename(replacedStage, movedStage);
            fs::create_directory(replacedStage);
        }
    }); } catch (const std::exception& error) { movedReport = error.what(); }
    check(movedReport.find(quotedReportField(pathUtf8(movedStage))) != std::string::npos,
        "A substituted quarantine is blocked and its actual kernel-resolved remaining path is reported");
    check(entryExists(replacedStage) && fs::is_empty(replacedStage) && entryExists(movedStage), "Native handle cleanup preserves the substituted directory and reports the real quarantine separately");
    check(!entryExists(movedStage / "item"), "Approved content is removed only through the original pinned quarantine descriptor");
    fs::remove(movedStage); fs::remove(replacedStage);
    const auto link = user.dataRoot / "owned-link";
    fs::create_symlink("preserved.msx", link);
    Target linkTarget{link, user.dataRoot, user.account, Category::Application, "Declared internal payload link", identifySymbolicLink(link)};
    removeVerifiedTarget(linkTarget, removalStagingPath(linkTarget, randomToken(), 2));
    check(!entryExists(link) && sha256File(legacy) == legacyHash, "Native payload-link removal never follows or changes its preserved destination");
#endif
}
void elevationRequestTests() {
    ElevationRequest request{"/private/tmp/marksix-ipc-" + randomToken() + "/channel", randomToken(), {123, 456, "501", {}, false}};
    const auto script = macosBootstrapCommand(request);
    check(script.find("/Applications/Mark Six Studio.app/Contents/Helpers/mark-six-maintenance") != std::string::npos &&
        script.find("/bin/cp -p") != std::string::npos && script.find("/usr/bin/env -i") != std::string::npos,
        "macOS bootstrap uses a fixed protected source, preserved ownership and clean native environment");
    check(script.find("rm -rf") == std::string::npos && script.find("com.brucerry.marksixstudio.maintenance") != std::string::npos,
        "Administrator bootstrap verifies native helper identity and never composes recursive deletion");
    check(macosAuthorizationScript().find("item 1 of argv") != std::string::npos, "AppleScript treats the fixed bootstrap as an argv value rather than injected AppleScript source");
    auto hostile = request; hostile.endpoint += "'; /bin/false; '";
    rejects([&] { macosBootstrapCommand(hostile); }, "Hostile shell coordinates are rejected before authorization");
    hostile = request; hostile.nonce = "../../other-root";
    rejects([&] { macosBootstrapCommand(hostile); }, "A nonce cannot select an elevated staging path");
    hostile = request; hostile.agent.account = "501'";
    rejects([&] { macosBootstrapCommand(hostile); }, "macOS account claims are bounded native UID syntax");
    request.endpoint = "\\\\.\\pipe\\marksix-maintenance-" + randomToken(); request.agent.account = "S-1-5-21-123-456-789-1001";
    check(windowsBootstrapParameters(request).find("/M6FULLCLEANUP=1 ") == 0, "Full Windows cleanup requires a distinct Inno administrative mode");
    hostile = request; hostile.endpoint += " /SILENT";
    rejects([&] { windowsBootstrapParameters(hostile); }, "Unapproved native-uninstaller switches cannot enter the handoff");
    hostile = request; hostile.agent.account = "S-1-5-21 /OTHER=1";
    rejects([&] { windowsBootstrapParameters(hostile); }, "Alternative administrator input cannot replace the initiating SID mapping");
}
void authorizationLifecycleTests() {
    for (const int scenario : {0, 1, 2, 3}) {
        Fixture fixture; auto user = fixture.user(); user.account = currentAccount(); const bool retry = scenario != 0;
        database(user.dataRoot / "statistics/results.sqlite3");
        const auto legacy = user.dataRoot / "legacy-保留.MSX"; write(legacy, "opaque recording; never parsed");
        const auto stage = fixture.root / ("marksix-maintenance-" + randomToken()); makePrivateDirectory(stage);
        const auto executable = stage / "mark-six-maintenance"; write(executable, "unexecuted private fixture image");
        write(stage / "request.bin", "private durable request");
        Inventory previous; preserveLegacyFile(previous, legacy, user.account);
        if (retry) write(stage / "remaining.txt", preservedLegacyReport(previous));
        const auto before = contents(fixture.root); const auto gate = readMachineGate();
        int launches = 0, finishes = 0, progress = 0;
        bool identityPreserved = false;
        UninstallAuthorizationHooks hooks;
        hooks.launch = [&](const auto& request, const auto& app, bool resumed) {
            ++launches;
            identityPreserved = request.agent.account == user.account && request.agent.pid == currentProcessId() && resumed == retry && app == fixture.root / "application";
            return scenario == 2 ? 1 : 1223;
        };
        hooks.waiting = [&](const auto& text) { ++progress; check(text.find("No current-app files have been deleted") != std::string::npos, "Authorization progress cannot claim deletion before consent"); };
        hooks.finishCancelled = [&](const auto& image, bool retire, const auto& report, const auto& root) {
            ++finishes;
            check(image == executable && root == user.dataRoot && retire == !retry, "Fresh refusal retires only its own staging; refused recovery remains runnable");
            check(report.find("cancelled at native administrator authorization") != std::string::npos, "Authorization refusal has an explicit cancelled outcome");
            if (retry) check(report.find(quotedReportField(pathUtf8(legacy))) != std::string::npos && report.find(user.account) != std::string::npos,
                "Refused retry retains the previous full preserved path and native account");
            else check(report.find("no empty-list claim") != std::string::npos, "Uninspected refusal does not assert that no legacy files exist");
            if (scenario == 3) throw std::runtime_error("private finalizer failure");
            if (retire) (void)retireUserHelper(image); else durableWrite(stage / "remaining.txt", report);
        };
        std::string observed; bool failed = false;
        try { check(runUninstallAgent(executable, {}, fixture.root / "application", user.dataRoot, randomToken(), retry, observed, hooks) == 0,
            "Refusal returns cancellation without launching a broker"); }
        catch (const std::exception&) { failed = true; }
        check(identityPreserved, "Authorization launch retains the actual native initiating identity and original request");
        check(failed == (scenario >= 2) && launches == 1 && progress > 0 && finishes == (scenario == 2 ? 0 : 1),
            "Failed launch/finalization and denied authorization follow distinct agent outcomes");
        for (const auto& [path, digest] : before) if (path != executable && path != stage / "request.bin" && path != stage / "remaining.txt")
            check(sha256File(path) == digest, "Authorization refusal/failure byte-preserves every private app/data/legacy target");
        check(entryExists(stage) == retry && (!retry || (entryExists(executable) && entryExists(stage / "request.bin"))),
            "Recovery refusal/failure keeps its standalone executable and durable request");
        if (retry) check(observed.find(quotedReportField(pathUtf8(legacy))) != std::string::npos, "Even a failing finalizer retains the previous preserved list for the caller's error report");
        const auto afterGate = readMachineGate();
        check(bool(gate) == bool(afterGate) && (!gate || encodeMachineGate(*gate) == encodeMachineGate(*afterGate)), "Authorization-only failures never publish or alter the system startup gate");
    }
#ifdef __APPLE__
    const auto cancelled = runNativeCommand("/usr/bin/osascript", {"-e", "error number -128"}); // Script error only; no administrator prompt.
    check(macosAuthorizationExitCode(cancelled.status, cancelled.output) == 1223, "Actual AppleScript cancellation output maps to the native refusal status");
#endif
    check(macosAuthorizationExitCode(0, "(-128)") == 0 && macosAuthorizationExitCode(1, "different bootstrap error") == 1,
        "Successful or unrelated bootstrap output cannot become an authorization refusal");
}
void userLifecycleTests() {
    Fixture fixture; const auto user = fixture.user();
    const auto root = user.dataRoot;
    database(root / "statistics/results.sqlite3");
    database(root / "statistics/adaptive.sqlite3", 0x4d364144, 1, "lineages,predictions,head");
    database(root / "statistics/forecast_lab.sqlite3", 0x4d36464c, 1, "lineages,forecasts,head");
    const auto legacy = root / "legacy.MSX"; write(legacy, "opaque old recording");
    const auto cached = user.cacheRoot / "graphics.bin"; write(cached, "disposable graphics");
    write(user.downloads / "unrelated.pkg", "personal browser download");
    bool idle = true, approve = false, changed = false; int reviews = 0;
    UserCleanupEnvironment environment;
    environment.users = [&](bool all, auto&) { check(!all, "Cache/reset native lifecycle requests only its authenticated account"); return std::vector<UserStorage>{user}; };
    environment.writersIdle = [&](const auto&) { return idle; };
    environment.review = [&](const auto&, const auto& report, const auto&, const auto&) {
        ++reviews;
        check(!FileLease::acquire(sessionLeasePath(root), FileLease::Mode::Shared), "Final review holds exclusive access to the closed workspace");
        check(!FileLease::acquire(accountLeasePath(user), FileLease::Mode::Shared), "Final review blocks another desktop before Qt startup even when it would use a different data root");
        check(report.find(quotedReportField(pathUtf8(legacy))) != std::string::npos && report.find(user.account) != std::string::npos,
            "Native final consent includes full preserved legacy path and account");
        if (changed) write(cached, "changed during approval");
        return approve;
    };
    std::string observed;
    const UserCleanupRequest cache{Operation::ClearCache, root, false};
    auto result = executeUserCleanup(cache, user.account, observed, environment);
    check(result.find("cancelled") != std::string::npos && entryExists(cached), "Declined final consent deletes no cache");
    check(result.find("Preserved legacy files") != std::string::npos, "Cancellation retains the labelled preserved list");
    approve = true; changed = true;
    rejects([&] { executeUserCleanup(cache, user.account, observed, environment); }, "Changed cache invalidates approval before any deletion");
    check(entryExists(cached) && observed.find(quotedReportField(pathUtf8(legacy))) != std::string::npos, "Blocked result still retains the full preserved-file report");
    changed = false;
    const auto before = contents(fixture.root);
    result = executeUserCleanup(cache, user.account, observed, environment);
    check(!entryExists(cached) && result.find("completed with legacy files preserved") != std::string::npos, "Cache lifecycle completes with explicit intentional legacy preservation");
    for (const auto& [path, hash] : before) if (path != cached) check(sha256File(path) == hash, "Complete cache lifecycle preserves saved databases, legacy files and browser downloads");
    idle = false; const auto oldReviews = reviews;
    rejects([&] { executeUserCleanup({Operation::ResetRecords, root, false}, user.account, observed, environment); }, "Live or older app instance blocks native cleanup");
    check(reviews == oldReviews && observed.find("inspection has not completed") != std::string::npos, "Blocked preflight makes no empty preserved-list claim");
    idle = true;
    result = executeUserCleanup({Operation::ResetRecords, root, false}, user.account, observed, environment);
    check(!entryExists(root / "statistics/results.sqlite3") && !entryExists(root / "statistics/adaptive.sqlite3") && !entryExists(root / "statistics/forecast_lab.sqlite3"),
        "Native reset lifecycle purges all three stores together");
    check(!resetPending(root) && result.find("completed with legacy files preserved") != std::string::npos, "Successful reset removes recovery staging and retains the explicit legacy result");
    check(sha256File(legacy) == before.at(legacy), "Successful reset preserves opaque old recordings byte-for-byte");
}
void uninstallInspectionTests() {
    Fixture fixture; auto first = fixture.user();
    auto second = storageForProfile({"second-native-account", fixture.root / "second", fixture.root / "second/local", fixture.root / "second/Downloads", false}, HostPlatform::MacOS);
    database(first.dataRoot / "statistics/results.sqlite3");
    database(second.dataRoot / "statistics/results.sqlite3");
    const auto legacy = second.dataRoot / "old.MSX"; write(legacy, "opaque saved recording");
    std::vector<UserStorage> nativeUsers{first, second};
    bool allUsers = false;
    UninstallDiscovery discovery;
    discovery.users = [&](bool all, std::vector<InventoryIssue>&) { allUsers = all; return nativeUsers; };
    discovery.applications = [](const auto&, auto&) { return std::vector<InstalledApplication>{}; };
    discovery.catalog = [] { return ReleaseCatalog{}; };
    const auto before = contents(fixture.root);
    const auto inspection = inspectUninstall({}, first.account, discovery);
    check(allUsers && inspection.users.size() == 2, "Native full inspection independently discovers all accounts");
    check(has(inspection.inventory, second.dataRoot / "statistics/results.sqlite3"), "Independent all-user inventory retains account mapping");
    check(!inspection.inventory.complete(), "Unresolved native profile coverage cannot become empty or successful cleanup");
    const auto review = uninstallReview(inspection);
    check(review.find("Preserved legacy files (1)") != std::string::npos && review.find(quotedReportField(pathUtf8(legacy))) != std::string::npos &&
        review.find(second.account) != std::string::npos, "Administrator review retains full labelled legacy paths and account attribution");
    auto changed = inspection; changed.users[1].home = fixture.root / "replacement-profile";
    check(uninstallDigest(changed) != uninstallDigest(inspection), "Approval binds native account/home mappings even when file identities are unchanged");
    changed = inspection; changed.originalAccount = second.account;
    check(uninstallDigest(changed) != uninstallDigest(inspection), "Alternate administrator identity cannot replace the initiating account");
    rejects([&] { inspectUninstall({}, "unknown-account", discovery); }, "Unresolved initiating account is rejected");
    UninstallScope scope; scope.additional.push_back({first.account, fixture.root / "selected custom root", LocationKind::SavedData});
    database(scope.additional.front().path / "statistics/results.sqlite3");
    const auto bytes = encodeUninstallScope(scope);
    const auto roundtrip = decodeUninstallScope(bytes);
    check(encodeUninstallScope(roundtrip) == bytes, "Privilege handoff carries bounded versioned discovery scope only");
    rejects([&] { decodeUninstallScope(bytes + "unapproved targets"); }, "Unapproved trailing target data is rejected");
    const auto additional = inspectUninstall(roundtrip, first.account, discovery);
    check(has(additional.inventory, scope.additional.front().path / "statistics/results.sqlite3"), "Selected custom root is independently validated rather than accepted as a file list");
    scope.additional.front().path = second.home / "documents";
    rejects([&] { inspectUninstall(scope, first.account, discovery); }, "A discovery hint cannot relabel another native profile");
    scope.additional.front().path = second.home;
    rejects([&] { inspectUninstall(scope, first.account, discovery); }, "No OS home can become a removal boundary");
    for (const auto& [path, hash] : before) check(sha256File(path) == hash, "All-user inventory causes no writes, migration or legacy-content changes");
}
void registeredLegacyCacheTests() {
    Fixture fixture; const auto user = fixture.user();
    const auto renamed = user.cacheRoot / "recording-renamed.dat";
#ifdef _WIN32
    write(renamed, "legacy bytes");
#else
    fs::create_directories(renamed.parent_path());
    check(mkfifo(renamed.c_str(), 0600) == 0, "Create opaque registered-recording fixture");
#endif
    OwnershipManifest manifest; manifest.recordings.push_back({renamed, std::string(64, '0')});
    writeManifest(user.manifestPath, manifest);
#ifdef _WIN32
    const auto lock = CreateFileW(renamed.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "Reserve recording against content reads");
#endif
    const auto inventory = inspectStorage({Operation::ClearCache, {user}, user.dataRoot, {}});
#ifdef _WIN32
    CloseHandle(lock);
#endif
    check(inventory.complete() && intentionallyPreserved(inventory, renamed) && !has(inventory, renamed),
        "Registered renamed recordings are recognized before generic cache hashing without opening their contents");
    write(user.manifestPath, "corrupt ownership metadata");
    const auto unknown = inspectStorage({Operation::Uninstall, {user}, {}, {}});
    check(!unknown.complete() && unknown.targets.empty(), "Unreadable legacy hints keep this account's storage out of partial deletion targets");
}
void uninstallExecutionTests() {
    Fixture fixture; const auto user = fixture.user();
    database(user.dataRoot / "statistics/results.sqlite3");
    const auto legacy = user.dataRoot / fs::path(u8"old recordings/保留.MSX"); write(legacy, "opaque legacy bytes");
    const auto foreign = user.dataRoot / "personal.txt"; write(foreign, "personal document");
    UninstallInspection inspection; inspection.originalAccount = user.account; inspection.users = {user};
    inspection.inventory = inspectStorage({Operation::Uninstall, {user}, {}, {}});
    inspectUninstallDirectories(inspection);
    const auto stage = fixture.root / "private-root-ledger"; makePrivateDirectory(stage);
    auto journal = UninstallJournal::create(stage, randomToken(), {}, inspection);
    bool idle = false; int registrationCalls = 0;
    UninstallExecutionEnvironment environment;
    environment.idle = [&] { return idle; };
    environment.removeRegistrations = [&](const auto&) { ++registrationCalls; };
    rejects([&] { executeUninstall(journal, journal.reviewDigest(), false, environment); }, "Full uninstall blocks live writers before any deletion");
    check(entryExists(user.dataRoot / "statistics/results.sqlite3") && registrationCalls == 0, "Blocked uninstall retains data and registration");
    idle = true;
    rejects([&] { executeUninstall(journal, std::string(64, '0'), false, environment); }, "Full uninstall rejects a substituted approval digest");
    const auto result = executeUninstall(journal, journal.reviewDigest(), false, environment);
    check(result.complete && registrationCalls == 1 && !entryExists(user.dataRoot / "statistics/results.sqlite3"), "Approved native lifecycle removes owned data and registrations");
    check(readBoundedFile(legacy) == "opaque legacy bytes" && readBoundedFile(foreign) == "personal document", "Native lifecycle preserves opaque recordings and personal documents byte for byte");
    check(result.report.find(pathUtf8(legacy)) != std::string::npos && result.report.find(user.account) != std::string::npos &&
        result.report.find("Legacy simulator recording") != std::string::npos, "Final native result explicitly lists preserved paths, accounts and reasons");
    check(entryExists(user.dataRoot) && !entryExists(user.dataRoot / "statistics"), "Only empty identified product directories are removed; legacy parents remain");
    const auto replacement = fixture.root / "replacement"; fs::create_directory(replacement);
    const auto identity = identifyDirectory(replacement); const auto original = fixture.root / "old-empty";
    fs::rename(replacement, original); fs::create_directory(replacement);
    rejects([&] { removeVerifiedEmptyDirectory(replacement, identity); }, "Substituted empty directory is protected by native identity");
    check(entryExists(replacement) && entryExists(original), "Changed directory rejection preserves both entries");
    const auto nonempty = identifyDirectory(user.dataRoot);
    check(!removeVerifiedEmptyDirectory(user.dataRoot, nonempty), "Nonempty directory is intentionally retained without recursion");

    Fixture retry; const auto retryUser = retry.user(); database(retryUser.dataRoot / "statistics/results.sqlite3");
    UninstallInspection retryPlan; retryPlan.originalAccount = retryUser.account; retryPlan.users = {retryUser};
    retryPlan.inventory = inspectStorage({Operation::Uninstall, {retryUser}, {}, {}}); inspectUninstallDirectories(retryPlan);
    const auto retryStage = retry.root / "protected-ledger"; makePrivateDirectory(retryStage);
    auto retryJournal = UninstallJournal::create(retryStage, randomToken(), {}, retryPlan);
    environment.removeRegistrations = [](const auto&) { throw std::runtime_error("Native registration is locked"); };
    const auto partial = executeUninstall(retryJournal, retryJournal.reviewDigest(), false, environment);
    check(!partial.complete && partial.report.find("Native registration is locked") != std::string::npos, "Native registration failure cannot report complete removal");
    auto reopened = UninstallJournal::open(retryStage);
    environment.removeRegistrations = [](const auto&) {};
    check(executeUninstall(reopened, reopened.reviewDigest(), false, environment).complete, "Standalone journal retries registration cleanup after payload files are gone");
}
void nativeCachePreservationTests() {
    Fixture fixture; const auto user = fixture.user();
    const auto directory = user.cacheRoot / "opaque-legacy.MSX";
    const auto unread = directory / "old-recording.dat";
    write(unread, "opaque recording container bytes");
    write(user.cacheRoot / "graphics/shader.bin", "disposable");
    const auto databasePath = user.dataRoot / "statistics/results.sqlite3"; database(databasePath);
    const auto original = sha256File(databasePath);
#ifdef __APPLE__
    const auto native = user.home / "Library/Caches/com.brucerry.marksixstudio";
    write(native / "graphics/renderer.bin", "native disposable graphics");
    const auto renamed = native / "renamed-recording.dat";
    fs::create_directories(renamed.parent_path()); check(mkfifo(renamed.c_str(), 0600) == 0, "Reserve unreadable opaque native-cache legacy hint");
    OwnershipManifest manifest; manifest.recordings.push_back({renamed, std::string(64, '0')}); writeManifest(user.manifestPath, manifest);
#endif
    const auto inventory = inspectUserCleanup({Operation::ClearCache, user.dataRoot, false}, user);
    check(inventory.complete() && intentionallyPreserved(inventory, directory) && !has(inventory, unread), "Opaque legacy candidate directories are preserved without inspecting their contents");
#ifdef __APPLE__
    check(has(inventory, native / "graphics/renderer.bin") && intentionallyPreserved(inventory, renamed), "Native bundle cache uses the same ownership and legacy-hint rules as Qt caches");
#endif
    clearUserCache(inventory, user, inventoryDigest(Operation::ClearCache, inventory));
    check(!entryExists(user.cacheRoot / "graphics/shader.bin") && readBoundedFile(unread) == "opaque recording container bytes" && sha256File(databasePath) == original,
        "Cache cleanup preserves legacy containers and saved database bytes");
#ifdef __APPLE__
    check(!entryExists(native / "graphics/renderer.bin") && entryExists(renamed), "Both native cache locations clear while an opaque renamed recording remains");
#endif
    const auto emptyLegacy = user.cacheRoot / "empty.MSX"; fs::create_directory(emptyLegacy);
    check(!removeVerifiedEmptyDirectory(emptyLegacy, identifyDirectory(emptyLegacy)) && entryExists(emptyLegacy), "Even an empty legacy candidate directory is intentionally retained");
}
void protectedRetryLegacyTests() {
    Fixture fixture; const auto user = fixture.user();
    const auto renamed = user.cacheRoot / "renamed-legacy.dat";
    fs::create_directories(renamed.parent_path());
#ifdef _WIN32
    write(renamed, "opaque fixture recording");
    const auto locked = CreateFileW(renamed.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "Reserve opaque legacy data against hashing");
#else
    check(mkfifo(renamed.c_str(), 0600) == 0, "Reserve an opaque recording without readable regular-file bytes");
#endif
    OwnershipManifest manifest; manifest.recordings.push_back({renamed, std::string(64, '0')}); writeManifest(user.manifestPath, manifest);
    UninstallDiscovery discovery; discovery.users = [&](bool, auto&) { return std::vector<UserStorage>{user}; };
    discovery.applications = [](const auto&, auto&) { return std::vector<InstalledApplication>{}; }; discovery.catalog = [] { return ReleaseCatalog{}; };
    const auto original = inspectUninstall({}, user.account, discovery);
    check(original.inventory.complete() && intentionallyPreserved(original.inventory, renamed), "Native initial inventory retains renamed legacy hints without reading them");
    fs::remove(user.manifestPath); discovery.retainedLegacy = original.inventory.preservedLegacy;
    const auto retried = inspectUninstall({}, user.account, discovery);
    check(retried.inventory.complete() && intentionallyPreserved(retried.inventory, renamed) && !has(retried.inventory, renamed),
        "Protected retry hints precede cache/payload hashing after the original manifest has been removed");
#ifdef _WIN32
    CloseHandle(locked);
#endif
    const auto file = user.dataRoot / "statistics/results.sqlite3"; write(file, "private owned-object fixture");
    UninstallInspection plan; plan.originalAccount = user.account; plan.users = {user};
    plan.inventory.targets.push_back({file, user.dataRoot, user.account, Category::Records, "Isolated native fixture", identifyFile(file)});
    const auto stage = fixture.root / "protected-recovery"; makePrivateDirectory(stage);
    auto journal = UninstallJournal::create(stage, randomToken(), {}, plan);
    const auto moved = user.dataRoot / "moved-statistics"; fs::rename(file.parent_path(), moved); fs::create_directory(file.parent_path());
    check(journal.currentStates().front() == RemovalState::Changed && !journal.filesRemoved(),
        "A substituted parent cannot turn moved records into a false verified-absence result");
    check(readBoundedFile(moved / file.filename()) == "private owned-object fixture", "Native absence rejection preserves relocated data bytes");
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--delay-native-command") { std::this_thread::sleep_for(std::chrono::seconds(10)); return 0; }
    if (argc == 3 && std::string_view(argv[1]) == "--echo-native-argument") {
#ifdef _WIN32
        int count = 0; const auto values = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!values || count != 3) return 2;
        const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, values[2], -1, nullptr, 0, nullptr, nullptr);
        std::string text(std::size_t(bytes), '\0');
        if (!bytes || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, values[2], -1, text.data(), bytes, nullptr, nullptr)) { LocalFree(values); return 2; }
        LocalFree(values); text.pop_back(); std::cout << text;
#else
        std::cout << argv[2];
#endif
        return 0;
    }
    try {
        manifestTests(); inventoryTests(); unknownAndBoundaryTests(); profileTests(); applicationIdentityTests(); payloadTests(); windowsPayloadTests(); preservedLegacyTests(); runtimeTests(); channelTests(); resetTests(); cacheCleanupTests(); installerLeaseTests(); jsonTests(); downloadTests(); payloadAuthorityTests(); workspaceRegistrationTests(); nativeReportTests(); nativeCommandTests(); uninstallMetadataTests(); uninstallJournalTests(); resetPermissionTests(); verifiedRemovalTests(); elevationRequestTests(); userLifecycleTests(); uninstallInspectionTests(); registeredLegacyCacheTests();
        uninstallExecutionTests();
        nativeCachePreservationTests();
        protectedRetryLegacyTests();
        authorizationLifecycleTests();
        std::cout << assertions << " storage inventory assertions passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
