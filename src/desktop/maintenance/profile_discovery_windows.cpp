#include "storage_inventory.hpp"
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <KnownFolders.h>
#include <sddl.h>
#include <array>
#include <optional>
#include <set>
#include <stdexcept>

namespace marksix::maintenance {
namespace {
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};
std::optional<std::wstring> registryString(HKEY key, const wchar_t* name) {
    std::array<wchar_t, 32768> value{};
    DWORD bytes = DWORD(value.size() * sizeof(wchar_t));
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
        nullptr, value.data(), &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t)) return {};
    const auto count = bytes / sizeof(wchar_t);
    if (value[count - 1] != L'\0') return {};
    return std::wstring(value.data(), count - 1);
}
std::optional<fs::path> resolvePath(std::wstring text, const fs::path& home, bool systemOnly = false) {
    if (!systemOnly) {
        for (const auto& pair : {std::pair{std::wstring(L"%USERPROFILE%"), home.native()},
            std::pair{std::wstring(L"%HOMEDRIVE%"), home.root_name().native()},
            std::pair{std::wstring(L"%HOMEPATH%"), home.relative_path().native().empty() ? std::wstring{} : L"\\" + home.relative_path().native()}}) {
            for (auto pos = text.find(pair.first); pos != std::wstring::npos; pos = text.find(pair.first))
                text.replace(pos, pair.first.size(), pair.second);
        }
    }
    // Profile paths may use only machine-global substitutions. Never expand an
    // arbitrary other user's path with the elevated administrator's environment.
    std::array<wchar_t, 32768> system{};
    const auto count = GetWindowsDirectoryW(system.data(), UINT(system.size()));
    if (!count || count >= system.size()) return {};
    const fs::path windows(std::wstring(system.data(), count));
    for (const auto& pair : {std::pair{std::wstring(L"%SystemDrive%"), windows.root_name().native()},
        std::pair{std::wstring(L"%SystemRoot%"), windows.native()}}) {
        for (auto pos = text.find(pair.first); pos != std::wstring::npos; pos = text.find(pair.first)) text.replace(pos, pair.first.size(), pair.second);
    }
    const fs::path path(text);
    if (text.find(L'%') != std::wstring::npos || !path.is_absolute()) return {};
    return path.lexically_normal();
}
std::wstring currentSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("Cannot inspect initiating account token");
    DWORD bytes = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> data(bytes);
    const bool ok = GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes) != 0; CloseHandle(token);
    if (!ok) throw std::runtime_error("Cannot inspect initiating account identity");
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &text)) throw std::runtime_error("Cannot format OS account identity");
    std::wstring result(text); LocalFree(text); return result;
}
std::optional<fs::path> currentFolder(REFKNOWNFOLDERID folder) {
    PWSTR text = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, KF_FLAG_DONT_VERIFY, nullptr, &text))) return {};
    fs::path result(text); CoTaskMemFree(text); return result.lexically_normal();
}
}

std::vector<UserStorage> discoverUsers(bool allUsers, std::vector<InventoryIssue>& issues) {
    std::vector<UserStorage> users;
    std::wstring initiating;
    try { initiating = currentSid(); }
    catch (const std::exception& error) { issues.push_back({{}, {}, error.what(), true}); return users; }
    Key profiles;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList", 0,
        KEY_READ | KEY_WOW64_64KEY, &profiles.value) != ERROR_SUCCESS) {
        issues.push_back({{}, {}, "OS profile list is inaccessible", true}); return users;
    }
    std::set<std::string> seen;
    for (DWORD index = 0;; ++index) {
        std::array<wchar_t, 256> name{}; DWORD length = DWORD(name.size());
        const auto status = RegEnumKeyExW(profiles.value, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) { issues.push_back({{}, {}, "OS profile enumeration did not complete", true}); break; }
        const std::wstring sid(name.data(), length);
        if (!allUsers && sid != initiating) continue;
        if (sid != initiating && sid.find(L"S-1-5-21-") != 0 && sid.find(L"S-1-12-1-") != 0) continue;
        const auto account = pathUtf8(fs::path(sid));
        Key profile;
        if (RegOpenKeyExW(profiles.value, name.data(), 0, KEY_READ, &profile.value) != ERROR_SUCCESS) {
            issues.push_back({{}, account, "OS profile metadata is inaccessible", true}); continue;
        }
        const auto raw = registryString(profile.value, L"ProfileImagePath");
        const auto home = raw ? resolvePath(*raw, {}, true) : std::optional<fs::path>{};
        if (!home) { issues.push_back({{}, account, "OS profile home could not be resolved", true}); continue; }
        NativeProfile native{account, *home, {}, {}, true};
        if (sid == initiating) {
            const auto local = currentFolder(FOLDERID_LocalAppData), downloads = currentFolder(FOLDERID_Downloads);
            if (local) native.localData = *local;
            if (downloads) native.downloads = *downloads;
        } else {
            Key hive;
            auto opened = RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_READ, &hive.value);
            // Do not load NTUSER.DAT with RegLoadAppKey: that API expects a
            // RegSaveKey hive and can create a missing file. An unavailable
            // profile stays unresolved for the authorized native executor.
            Key folders;
            if (opened == ERROR_SUCCESS && RegOpenKeyExW(hive.value,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", 0, KEY_READ, &folders.value) == ERROR_SUCCESS) {
                const auto local = registryString(folders.value, L"Local AppData");
                const auto downloads = registryString(folders.value, L"{374DE290-123F-4565-9164-39C4925E467B}");
                if (local) { const auto resolved = resolvePath(*local, *home); if (resolved) native.localData = *resolved; }
                if (downloads) { const auto resolved = resolvePath(*downloads, *home); if (resolved) native.downloads = *resolved; }
            }
        }
        try {
            validateContainedPath(*home / ".profile-inspection", *home);
            std::error_code error;
            if (!fs::is_directory(fs::symlink_status(*home, error)) || error) throw std::runtime_error("Account home is unavailable or offline");
        } catch (const std::exception& error) {
            native.locationsResolved = false; issues.push_back({*home, account, error.what(), true});
        }
        if (native.localData.empty() || native.downloads.empty()) {
            native.locationsResolved = false;
            issues.push_back({*home, account, "Account known folders are inaccessible or have unresolved redirection", true});
        }
        try { users.push_back(storageForProfile(native, HostPlatform::Windows)); seen.insert(account); }
        catch (const std::exception& error) { issues.push_back({*home, account, error.what(), true}); }
    }
    if (!seen.contains(pathUtf8(fs::path(initiating)))) issues.push_back({{}, pathUtf8(fs::path(initiating)), "Initiating account is absent from OS profile inventory", true});
    return users;
}
}
