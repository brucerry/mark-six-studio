#include "storage_inventory.hpp"
#include <pwd.h>
#include <unistd.h>
#include <cerrno>
#include <mutex>
#include <set>

namespace marksix::maintenance {
std::vector<UserStorage> discoverUsers(bool allUsers, std::vector<InventoryIssue>& issues) {
    static std::mutex passwordDatabase;
    const std::lock_guard lock(passwordDatabase);
    std::vector<NativeProfile> profiles;
    const auto current = getuid();
    const auto append = [&](const passwd& account) {
        if (!account.pw_dir || !*account.pw_dir) return;
        // Directory-services user accounts, plus the initiating account. Include
        // root when it actually has product storage, without sweeping service homes.
        auto home = fs::path(account.pw_dir);
        if (!home.is_absolute() || home == home.root_path() || home == "/var/empty") return;
        if (account.pw_uid < 500 && account.pw_uid != current) {
            std::error_code error;
            const bool hasData = fs::exists(home / "Library/Application Support/MarkSixStudio", error);
            if (!hasData && !error) return;
        }
        const auto identity = std::to_string(account.pw_uid);
        std::error_code error;
        const auto canonical = fs::canonical(home, error);
        const bool resolved = !error;
        if (resolved) home = canonical;
        else issues.push_back({home, identity, "Account home is inaccessible or offline", true});
        profiles.push_back({identity, home.lexically_normal(), {}, {}, resolved});
    };
    if (allUsers) {
        setpwent();
        errno = 0;
        while (const auto* account = getpwent()) { append(*account); errno = 0; }
        const auto failure = errno;
        endpwent();
        if (failure) issues.push_back({{}, {}, "OS account enumeration did not complete", true});
    } else {
        const auto* account = getpwuid(current);
        if (account) append(*account);
        else issues.push_back({{}, std::to_string(current), "Initiating OS account could not be resolved", true});
    }
    std::set<std::string> seen;
    std::vector<UserStorage> users;
    for (const auto& profile : profiles) {
        if (!seen.insert(profile.account).second) continue;
        try { users.push_back(storageForProfile(profile, HostPlatform::MacOS)); }
        catch (const std::exception& error) { issues.push_back({profile.home, profile.account, error.what(), true}); }
    }
    if (seen.find(std::to_string(current)) == seen.end())
        issues.push_back({{}, std::to_string(current), "Initiating account is absent from OS account inventory", true});
    return users;
}
}
