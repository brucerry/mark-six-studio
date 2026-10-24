#include "storage_inventory.hpp"
#include "durable_files.hpp"
#include "json_reader.hpp"
#include <algorithm>
#include <fstream>
#include <set>
#include <map>
#include <stdexcept>

namespace marksix::maintenance {
namespace {
constexpr std::uintmax_t maximumManifest = 8 * 1024 * 1024;
int nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    throw std::runtime_error("Invalid payload path encoding");
}
std::string decode(const std::string& value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        unsigned char byte = static_cast<unsigned char>(value[i]);
        if (byte == '%') {
            if (i + 2 >= value.size()) throw std::runtime_error("Truncated payload path encoding");
            byte = static_cast<unsigned char>((nibble(value[i + 1]) << 4) | nibble(value[i + 2])); i += 2;
        } else if (byte <= 32 || byte >= 127) throw std::runtime_error("Noncanonical payload path encoding");
        if (!byte) throw std::runtime_error("Invalid payload path");
        result += char(byte);
    }
    return result;
}
bool contained(const fs::path& file, const fs::path& root) {
    const auto relative = file.lexically_relative(root);
    return !relative.empty() && relative != "." && *relative.begin() != ".." && !relative.is_absolute();
}
fs::path memberPath(const fs::path& root, const std::string& encoded) {
    const auto relative = pathFromUtf8(decode(encoded));
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative != relative.lexically_normal())
        throw std::runtime_error("Invalid payload member path");
    for (const auto& part : relative) if (part == ".." || part == ".") throw std::runtime_error("Payload path traversal rejected");
    const auto path = root / relative;
    validateContainedPath(path, root);
    return path;
}
bool absent(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return true;
    if (error) throw std::runtime_error("Payload member is inaccessible");
    return status.type() == fs::file_type::not_found;
}
#ifdef _WIN32
void validatePackageMetadata(const fs::path& path, const fs::path& root, const Inventory& inventory) {
    JsonDocument json(readBoundedFile(path));
    auto format = json.query("SELECT json_extract(?1, '$.format'), json_extract(?1, '$.qtVersion'), json_type(?1, '$.files')");
    if (!format.next() || format.text(0) != "marksix-statistics-package-v1" || format.text(1) != "6.10.2" || format.text(2) != "array")
        throw std::runtime_error("Unrecognized product packaging metadata");
    auto files = json.query("SELECT json_extract(value, '$.path'), json_extract(value, '$.sha256') FROM json_each(?1, '$.files')");
    std::set<fs::path> found;
    std::map<fs::path, std::string> digests;
    for (const auto& target : inventory.targets) digests.emplace(target.path, target.identity.sha256);
    while (files.next()) {
        const auto relative = pathFromUtf8(files.text(0)); auto digest = files.text(1);
        if (relative.is_absolute() || relative.has_root_name() || relative != relative.lexically_normal()) throw std::runtime_error("Unsafe packaging metadata path");
        const auto file = root / relative; validateContainedPath(file, root);
        if (!found.insert(file).second) throw std::runtime_error("Duplicate packaging metadata path");
        if (intentionallyPreserved(inventory, file)) continue;
        for (auto& byte : digest) if (byte >= 'A' && byte <= 'F') byte = char(byte - 'A' + 'a');
        const auto target = digests.find(file);
        if (target == digests.end() || target->second != digest) throw std::runtime_error("Packaging metadata does not match the validated payload");
    }
    for (const auto& target : inventory.targets) if (!found.contains(target.path)) throw std::runtime_error("Packaging metadata omits a validated payload member");
}
#endif
}
void preserveApplicationRecordingHints(Inventory& inventory, const std::vector<UserStorage>& users,
    const std::vector<InstalledApplication>& applications) {
    for (const auto& user : users) {
        try {
            if (intentionallyPreserved(inventory, user.manifestPath)) throw std::runtime_error("Ownership metadata is covered by a protected legacy hint; retained without reading");
            for (const auto& hint : readManifest(user.manifestPath).recordings) for (const auto& app : applications) {
                const auto relative = hint.path.lexically_relative(app.root);
                if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") continue;
                validateContainedPath(hint.path, app.root);
                if (entryExists(hint.path)) preserveLegacyFile(inventory, hint.path, user.account,
                    "Registered legacy recording inside a reviewed application location; preserved without reading its contents.");
            }
        } catch (const std::exception& error) { inventory.issues.push_back({user.manifestPath, user.account, error.what(), true}); }
    }
}
PayloadInspection inspectApplicationPayload(const InstalledApplication& application, const std::vector<fs::path>& protectedHomes, const std::vector<PreservedFile>& retainedLegacy) {
    PayloadInspection result;
    for (const auto& file : retainedLegacy) {
        const auto relative = file.path.lexically_relative(application.root);
        if (file.path == application.root || (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")) preserveLegacyFile(result.inventory, file.path, file.account, file.reason);
    }
    const auto& root = application.root;
#ifdef _WIN32
    const auto manifest = root / "maintenance-payload.txt";
#else
    const auto manifest = root / "Contents/Resources/maintenance-payload.txt";
#endif
    const auto issue = [&](const fs::path& path, const std::string& text) {
        result.inventory.issues.push_back({path, application.account, text, true});
    };
    try {
        if (intentionallyPreserved(result.inventory, application.executable) || intentionallyPreserved(result.inventory, manifest))
            throw std::runtime_error("A legacy hint covers required product identity metadata; this copy is retained without further payload inspection");
#ifdef __APPLE__
        if (intentionallyPreserved(result.inventory, root / "Contents/_CodeSignature/CodeResources")) throw std::runtime_error("Legacy preservation covers native identity metadata; this copy is retained");
#endif
        if (protectedHomes.empty()) throw std::runtime_error("Native profile boundaries must be resolved before application inspection");
        validateBoundary(root, {});
        for (const auto& home : protectedHomes) validateBoundary(root, home);
        if (!hasProductIdentity(root)) throw std::runtime_error("Unverified installed product identity");
        discoverPreservedLegacy(result.inventory, root, application.account);
        validateContainedPath(manifest, root);
        const auto manifestIdentity = identifyFile(manifest);
        if (manifestIdentity.bytes > maximumManifest) throw std::runtime_error("Payload manifest exceeds limit");
        std::ifstream input(manifest, std::ios::binary);
        std::string line;
        if (!std::getline(input, line) || line != "M6PAYLOAD\t1\tcom.brucerry.marksixstudio")
            throw std::runtime_error("Unsupported product payload manifest");
        std::set<fs::path> declared{manifest};
        std::size_t count = 0;
        while (std::getline(input, line)) {
            if (++count > 100000 || line.size() > 32768) throw std::runtime_error("Payload inventory exceeds limit");
            const auto first = line.find('\t'), second = line.find('\t', first == std::string::npos ? 0 : first + 1);
            if (first != 1 || second == std::string::npos || line.find('\t', second + 1) != std::string::npos)
                throw std::runtime_error("Malformed payload entry");
            const auto evidence = line.substr(first + 1, second - first - 1);
            if (line[0] != 'D' && line[0] != 'F' && line[0] != 'L') throw std::runtime_error("Unknown payload member type");
            if (line[0] == 'D' && evidence != "-") throw std::runtime_error("Invalid directory entry");
            if (line[0] == 'F' && (evidence.size() != 64 || evidence.find_first_not_of("0123456789abcdef") != std::string::npos))
                throw std::runtime_error("Invalid payload content digest");
            const auto file = memberPath(root, line.substr(second + 1));
            if (!declared.insert(file).second) throw std::runtime_error("Duplicate payload entry");
            if (absent(file)) continue;
            if (intentionallyPreserved(result.inventory, file)) continue;
            try {
                if (line[0] == 'D') {
                    if (evidence != "-") throw std::runtime_error("Invalid directory entry");
                    validateContainedPath(file / ".probe", root);
                    result.directories.push_back(file);
                    continue;
                }
                FileIdentity identity;
                if (line[0] == 'F') {
                    if (evidence.size() != 64 || evidence.find_first_not_of("0123456789abcdef") != std::string::npos)
                        throw std::runtime_error("Invalid payload content digest");
                    identity = identifyFile(file);
                    if (identity.sha256 != evidence) throw std::runtime_error("Payload file changed; review required");
                } else if (line[0] == 'L') {
                    const auto targetText = decode(evidence);
                    const auto target = pathFromUtf8(targetText);
                    if (target.is_absolute() || target.has_root_name() || !contained((file.parent_path() / target).lexically_normal(), root))
                        throw std::runtime_error("Payload link escapes application boundary");
                    identity = identifySymbolicLink(file);
                    if (identity.linkTarget != targetText) throw std::runtime_error("Payload link changed; review required");
                } else throw std::runtime_error("Unknown payload member type");
                result.inventory.targets.push_back({file, root, application.account, Category::Application,
                    "Validated product payload manifest member", std::move(identity)});
            } catch (const std::exception& error) { issue(file, error.what()); }
        }
        if (!input.eof() || identifyFile(manifest) != manifestIdentity) throw std::runtime_error("Payload manifest changed during inspection");
        result.inventory.targets.push_back({manifest, root, application.account, Category::Application,
            "Versioned product payload manifest", manifestIdentity});
        std::size_t actualCount = 0;
        for (auto entry = fs::recursive_directory_iterator(root); entry != fs::recursive_directory_iterator(); ++entry) {
            if (++actualCount > 100000) throw std::runtime_error("Installed file inventory exceeds limit");
            const auto file = entry->path();
            if (intentionallyPreserved(result.inventory, file)) { entry.disable_recursion_pending(); continue; }
#ifdef _WIN32
            if (!declared.contains(file) && file == root / "manifest.json") {
                try {
                    const auto before = identifyFile(file); validatePackageMetadata(file, root, result.inventory);
                    if (identifyFile(file) != before) throw std::runtime_error("Packaging metadata changed during inspection");
                    result.inventory.targets.push_back({file, root, application.account, Category::Application,
                        "Typed product packaging metadata matching the validated payload", before});
                } catch (const std::exception& error) { issue(file, error.what()); }
                continue;
            }
#endif
#ifdef __APPLE__
            // Final signing changes both the main executable's signature slots
            // and CodeResources. Hashing either through this manifest creates a
            // cycle. The privileged executor must first validate the complete
            // root code seal; inspection alone is never removal authority.
            if (!declared.contains(file) && (file == root / "Contents/_CodeSignature/CodeResources" ||
                file == root / "Contents/MacOS/mark-six-studio")) {
                validateContainedPath(file, root);
                result.inventory.targets.push_back({file, root, application.account, Category::Application,
                    "Root product code seal; native signature validation required before removal", identifyFile(file)});
                continue;
            }
            if (!declared.contains(file) && file == root / "Contents/_CodeSignature" && fs::is_directory(entry->symlink_status())) {
                result.directories.push_back(file); continue;
            }
#endif
            if (!declared.contains(file)) issue(file, "Not in the product payload manifest; retained for review");
            if (fs::is_symlink(entry->symlink_status())) entry.disable_recursion_pending();
        }
        result.directories.push_back(root);
        std::erase_if(result.directories, [&](const auto& directory) {
            return std::any_of(result.inventory.preservedLegacy.begin(), result.inventory.preservedLegacy.end(),
                [&](const auto& preserved) { return contained(preserved.path, directory); });
        });
    } catch (const std::exception& error) { issue(manifest, error.what()); }
    return result;
}
}
