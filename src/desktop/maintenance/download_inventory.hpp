#pragma once
#include "storage_inventory.hpp"

namespace marksix::maintenance {
enum class AssetRole { Installer, Checksum, Manifest };
struct PublishedAsset {
    std::string name, sha256, tag;
    std::uint64_t bytes = 0;
    AssetRole role = AssetRole::Installer;
};
struct ReleaseCatalog {
    std::vector<PublishedAsset> assets;
    std::vector<std::string> limits;
    bool complete = true;
};
// The privileged executor obtains its own catalogue from this fixed origin.
// A client-supplied digest/catalogue is never used as elevation authority.
std::string fetchReleasePage(unsigned page);
std::size_t appendPublishedAssets(ReleaseCatalog& catalog, const std::string& json);
ReleaseCatalog fetchReleaseCatalog();
void inspectDownloads(Inventory& inventory, const std::vector<UserStorage>& users,
    const ReleaseCatalog& catalog, const std::vector<fs::path>& protectedPackages = {});
}
