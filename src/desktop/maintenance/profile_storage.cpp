#include "storage_inventory.hpp"
#include <stdexcept>

namespace marksix::maintenance {
std::vector<fs::path> nativeCacheRoots(const UserStorage& user) {
    std::vector<fs::path> roots{user.cacheRoot};
#ifdef __APPLE__
    const auto native = user.home / "Library/Caches/com.brucerry.marksixstudio";
    if (native != user.cacheRoot) roots.push_back(native);
#endif
    return roots;
}
UserStorage storageForProfile(const NativeProfile& profile, HostPlatform platform) {
    if (profile.account.empty() || !profile.home.is_absolute() || profile.home != profile.home.lexically_normal())
        throw std::runtime_error("Invalid native account identity or home");
    UserStorage storage;
    storage.account = profile.account;
    storage.home = profile.home;
    storage.locationsResolved = profile.locationsResolved;
    if (platform == HostPlatform::Windows) {
        const auto local = profile.localData.empty() ? profile.home / "AppData/Local" : profile.localData;
        storage.dataRoot = local / "MarkSixEmulator";
        storage.cacheRoot = local / "Mark Six Studio/cache";
        storage.manifestPath = local / "Mark Six Studio/maintenance/ownership.bin";
        if (profile.localData.empty()) storage.locationsResolved = false;
    } else {
        storage.dataRoot = profile.home / "Library/Application Support/MarkSixStudio";
        storage.cacheRoot = profile.home / "Library/Caches/Mark Six Studio";
        storage.manifestPath = storage.dataRoot / "maintenance/ownership.bin";
    }
    storage.downloads = profile.downloads.empty() ? profile.home / "Downloads" : profile.downloads;
    if (profile.downloads.empty() && platform == HostPlatform::Windows) storage.locationsResolved = false;
    for (const auto& path : {storage.dataRoot, storage.cacheRoot, storage.manifestPath, storage.downloads})
        if (!path.is_absolute() || path != path.lexically_normal()) throw std::runtime_error("Unresolved native account location");
    return storage;
}
}
