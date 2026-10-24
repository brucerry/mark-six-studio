#include "storage_inventory.hpp"
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <set>
#include <vector>

namespace marksix::maintenance {
bool hasProductIdentity(const fs::path& root) {
    try {
        if (isDevelopmentLocation(root)) return false;
        const auto executable = root / "mark-six-studio.exe";
        validateContainedPath(executable, root);
        const auto before = identifyFile(executable);
        DWORD ignored = 0;
        const auto size = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
        if (!size || size > 1024 * 1024) return false;
        std::vector<unsigned char> information(size);
        if (!GetFileVersionInfoW(executable.c_str(), 0, size, information.data())) return false;
        const std::array<std::pair<const wchar_t*, const wchar_t*>, 3> expected{{
            {L"ProductName", L"Mark Six Studio"}, {L"InternalName", L"mark-six-studio"},
            {L"OriginalFilename", L"mark-six-studio.exe"}}};
        for (const auto& [key, value] : expected) {
            const auto query = std::wstring(L"\\StringFileInfo\\040904b0\\") + key;
            void* text = nullptr; UINT length = 0;
            if (!VerQueryValueW(information.data(), query.c_str(), &text, &length) || !length ||
                std::wstring_view(static_cast<const wchar_t*>(text), length - 1) != value) return false;
        }
        return before == identifyFile(executable);
    } catch (...) { return false; }
}
std::vector<InstalledApplication> discoverApplications(const std::vector<UserStorage>& users,
                                                     std::vector<InventoryIssue>& issues) {
    const wchar_t* product = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{3B52BF71-65D1-4C1F-A25E-0A3B16A7B3C8}_is1";
    std::vector<InstalledApplication> applications;
    std::set<fs::path> seen;
    const auto inspect = [&](HKEY hive, const std::wstring& key, const std::string& account) {
        HKEY handle = nullptr;
        const auto opened = RegOpenKeyExW(hive, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &handle);
        if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return;
        if (opened != ERROR_SUCCESS) { issues.push_back({{}, account, "Product uninstall registration is inaccessible", true}); return; }
        std::array<wchar_t, 32768> text{}; DWORD bytes = DWORD(text.size() * sizeof(wchar_t));
        const auto status = RegGetValueW(handle, nullptr, L"InstallLocation", RRF_RT_REG_SZ, nullptr, text.data(), &bytes);
        RegCloseKey(handle);
        if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t) || bytes > text.size() * sizeof(wchar_t)) {
            issues.push_back({{}, account, "Registered product location could not be resolved", true}); return;
        }
        auto root = fs::path(text.data()).lexically_normal();
        if (root.filename().empty()) root = root.parent_path();
        if (!hasProductIdentity(root)) { issues.push_back({root, account, "Installed copy has unverified product identity or is a development tree", true}); return; }
        if (seen.insert(root).second) applications.push_back({root, root / "mark-six-studio.exe", account, pathUtf8(fs::path(key))});
    };
    inspect(HKEY_LOCAL_MACHINE, product, {});
    for (const auto& user : users) {
        const auto sid = pathFromUtf8(user.account).native();
        inspect(HKEY_USERS, sid + L"\\" + product, user.account);
    }
    return applications;
}
}
