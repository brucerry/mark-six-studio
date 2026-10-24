#include "download_inventory.hpp"
#include "json_reader.hpp"
#include <algorithm>
#include <regex>
#include <set>
#include <optional>

namespace marksix::maintenance {
namespace {
const std::string repository = "https://github.com/brucerry/mark-six-studio";
std::optional<AssetRole> role(const std::string& name, const std::string& version) {
    for (const auto& suffix : {std::string("-unsigned"), std::string()}) {
        const auto windows = "MarkSixStudio-" + version + "-windows-x64" + suffix + "-setup.exe";
        const auto mac = "MarkSixStudio-" + version + "-macos-universal" + suffix;
        if (name == windows || name == mac + ".pkg") return AssetRole::Installer;
        if (name == windows + ".sha256" || name == mac + ".sha256") return AssetRole::Checksum;
        if (name == mac + ".manifest.json") return AssetRole::Manifest;
    }
    return {};
}
bool candidate(const fs::path& path) {
    const auto name = pathUtf8(path.filename());
    return name.starts_with("MarkSixStudio-") && (name.ends_with(".exe") || name.ends_with(".pkg") ||
        name.ends_with(".sha256") || name.ends_with(".manifest.json"));
}
}
std::size_t appendPublishedAssets(ReleaseCatalog& catalog, const std::string& bytes) {
    JsonDocument page(bytes); auto type = page.query("SELECT json_type(?1)");
    if (!type.next() || type.text(0) != "array") throw std::runtime_error("Release catalogue is not an array");
    auto releases = page.query("SELECT value FROM json_each(?1)"); std::size_t count = 0;
    while (releases.next()) {
        if (++count > 100) throw std::runtime_error("Release page exceeds requested count");
        JsonDocument release(releases.text(0));
        auto fields = release.query("SELECT json_type(?1, '$.draft'), json_type(?1, '$.prerelease'), json_extract(?1, '$.tag_name'), json_extract(?1, '$.html_url'), json_type(?1, '$.assets')");
        if (!fields.next()) throw std::runtime_error("Missing release metadata");
        if (fields.text(0) == "true" || fields.text(1) == "true") continue;
        const auto tag = fields.text(2);
        static const std::regex version("v(0|[1-9][0-9]{0,9})\\.(0|[1-9][0-9]{0,9})\\.(0|[1-9][0-9]{0,9})");
        if (fields.text(0) != "false" || fields.text(1) != "false" || !std::regex_match(tag, version) ||
            fields.text(3) != repository + "/releases/tag/" + tag || fields.text(4) != "array")
            throw std::runtime_error("Unrecognized published release identity");
        auto assets = release.query("SELECT json_extract(value, '$.name'), json_extract(value, '$.state'), json_extract(value, '$.browser_download_url'), json_extract(value, '$.size'), json_extract(value, '$.digest') FROM json_each(?1, '$.assets')");
        std::set<std::string> names; std::size_t assetCount = 0;
        while (assets.next()) {
            if (++assetCount > 1000) throw std::runtime_error("Release asset count exceeds limit");
            const auto name = assets.text(0); const auto kind = role(name, tag.substr(1));
            if (!kind) continue;
            if (!names.insert(name).second) throw std::runtime_error("Duplicate published product asset");
            try {
                const auto size = assets.integer(3); auto digest = assets.text(4);
                if (assets.text(1) != "uploaded" || assets.text(2) != repository + "/releases/download/" + tag + '/' + name ||
                    size <= 0 || size > 2LL * 1024 * 1024 * 1024 || !digest.starts_with("sha256:"))
                    throw std::runtime_error("Unverified release asset metadata");
                digest.erase(0, 7);
                for (auto& byte : digest) if (byte >= 'A' && byte <= 'F') byte = char(byte - 'A' + 'a');
                if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos)
                    throw std::runtime_error("Missing published SHA-256 identity");
                const auto duplicate = std::find_if(catalog.assets.begin(), catalog.assets.end(), [&](const auto& asset) { return asset.name == name; });
                if (duplicate != catalog.assets.end()) throw std::runtime_error("Duplicate product asset across catalogue pages");
                catalog.assets.push_back({name, digest, tag, std::uint64_t(size), *kind});
            } catch (const std::exception& error) {
                catalog.complete = false; catalog.limits.push_back(name + ": " + error.what());
            }
        }
    }
    return count;
}
ReleaseCatalog fetchReleaseCatalog() {
    ReleaseCatalog catalog;
    try {
        for (unsigned page = 1; page <= 10; ++page) {
            if (appendPublishedAssets(catalog, fetchReleasePage(page)) < 100) return catalog;
        }
        catalog.complete = false; catalog.limits.push_back("Only the first 1,000 published releases were inspected");
    } catch (const std::exception& error) { catalog.complete = false; catalog.limits.push_back(error.what()); }
    return catalog;
}
void inspectDownloads(Inventory& inventory, const std::vector<UserStorage>& users,
                      const ReleaseCatalog& catalog, const std::vector<fs::path>& protectedPackages) {
    std::uint64_t verifiedBytes = 0;
    for (const auto& user : users) {
        std::set<fs::path> roots{user.downloads}; roots.insert(user.additionalDownloadRoots.begin(), user.additionalDownloadRoots.end());
        for (const auto& root : roots) {
            try {
                validateBoundary(root, user.home);
                std::error_code error; const auto status = fs::symlink_status(root, error);
                if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) continue;
                if (error || !fs::is_directory(status)) throw std::runtime_error("Downloads location is unavailable or redirected");
                discoverPreservedLegacy(inventory, root, user.account);
                if (isDevelopmentLocation(root)) {
                    inventory.issues.push_back({root, user.account, "Developer checkout or build archive; downloaded artifacts here are outside application cleanup scope", false});
                    continue;
                }
                std::size_t count = 0;
                for (const auto& entry : fs::directory_iterator(root)) {
                    if (++count > 100000) throw std::runtime_error("Download entry limit reached; review a narrower folder");
                    const auto path = entry.path(); if (!candidate(path) || intentionallyPreserved(inventory, path)) continue;
                    try {
                        validateContainedPath(path, root);
                        (void)pathFromUtf8(pathUtf8(path));
                        if (!fs::is_regular_file(entry.symlink_status())) throw std::runtime_error("Candidate installer is redirected or not a regular file");
                        if (std::any_of(protectedPackages.begin(), protectedPackages.end(), [&](const auto& item) {
                            const auto relative = path.lexically_relative(item);
                            return path == item || (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..");
                        }))
                            throw std::runtime_error("A native installer still needs this package");
                        const auto size = fs::file_size(path);
                        if (std::none_of(catalog.assets.begin(), catalog.assets.end(), [&](const auto& asset) { return asset.bytes == size; }))
                            throw std::runtime_error("No matching published product identity; file preserved");
                        if (size > 8ULL * 1024 * 1024 * 1024 - verifiedBytes) throw std::runtime_error("Download verification byte limit reached; file preserved");
                        verifiedBytes += size; const auto identity = identifyFile(path);
                        const auto found = std::find_if(catalog.assets.begin(), catalog.assets.end(), [&](const auto& asset) {
                            return asset.bytes == identity.bytes && asset.sha256 == identity.sha256;
                        });
                        if (found == catalog.assets.end()) throw std::runtime_error("File bytes do not match a published product asset; preserved");
                        if (found->role != AssetRole::Installer && pathUtf8(path.filename()) != found->name)
                            throw std::runtime_error("Companion identity requires its attributable published asset name; preserved");
                        if (!std::any_of(inventory.targets.begin(), inventory.targets.end(), [&](const auto& target) { return target.path == path; }))
                            inventory.targets.push_back({path, root, user.account, Category::Installer,
                                "Exact SHA-256 and size of official " + found->tag + " asset " + found->name, identity});
                    } catch (const std::exception& failure) { inventory.issues.push_back({path, user.account, failure.what(), true}); }
                }
                inventory.issues.push_back({root, user.account, "Installer inspection covers files directly in this folder. Select any nonstandard download folder or nested installer location separately", false});
            } catch (const std::exception& failure) { inventory.issues.push_back({root, user.account, failure.what(), true}); }
        }
    }
    for (const auto& limit : catalog.limits) inventory.issues.push_back({{}, {}, "Published release verification incomplete: " + limit, true});
}
}
