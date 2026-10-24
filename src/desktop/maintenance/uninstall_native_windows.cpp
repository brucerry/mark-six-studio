#include "uninstall_execution.hpp"
#include "native_command.hpp"
#include "payload_authority.hpp"
#include "durable_files.hpp"
#include "wire_codec.hpp"
#include "installer_lease.hpp"
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <ShlObj.h>
#include <aclapi.h>
#include <sddl.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <regex>
#include <thread>

namespace marksix::maintenance {
namespace {
constexpr wchar_t productKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{3B52BF71-65D1-4C1F-A25E-0A3B16A7B3C8}_is1";
struct Key { HKEY value = nullptr; ~Key() { if (value) RegCloseKey(value); } };
void rejectRegistrationLink(HKEY key) {
    DWORD type = 0;
    if (RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr) != ERROR_FILE_NOT_FOUND)
        throw std::runtime_error("Redirected or unsupported native product registration is retained");
}
void protectedRegistration(HKEY key) {
    rejectRegistrationLink(key);
    PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(reinterpret_cast<HANDLE>(key), SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) throw std::runtime_error("Cannot authenticate native product registration");
    struct Security { PSECURITY_DESCRIPTOR value; ~Security() { LocalFree(value); } } guard{descriptor};
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> admin{}, system{};
    DWORD adminSize = DWORD(admin.size()), systemSize = DWORD(system.size());
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin.data(), &adminSize) ||
        !CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &systemSize) || !owner || !acl ||
        (!EqualSid(owner, admin.data()) && !EqualSid(owner, system.data()))) throw std::runtime_error("Native product registration has an untrusted owner");
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* entry = nullptr;
        if (!GetAce(acl, index, &entry)) throw std::runtime_error("Cannot inspect native registration permissions");
        if (reinterpret_cast<ACE_HEADER*>(entry)->AceFlags & INHERIT_ONLY_ACE) continue; // Not effective on this key.
        const auto type = reinterpret_cast<ACE_HEADER*>(entry)->AceType;
        if (type == ACCESS_DENIED_ACE_TYPE) continue;
        if (type != ACCESS_ALLOWED_ACE_TYPE) throw std::runtime_error("Unsupported native registration permissions");
        const auto* ace = reinterpret_cast<ACCESS_ALLOWED_ACE*>(entry); const auto sid = const_cast<DWORD*>(&ace->SidStart);
        constexpr DWORD writes = KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
        if ((ace->Mask & writes) && !EqualSid(sid, admin.data()) && !EqualSid(sid, system.data())) {
            LPWSTR raw = nullptr; std::string account = "unknown SID";
            if (ConvertSidToStringSidW(sid, &raw)) { account = pathUtf8(fs::path(raw)); LocalFree(raw); }
            throw std::runtime_error("Native product registration allows unprivileged changes: " + account + ", mask " + std::to_string(ace->Mask));
        }
    }
}
std::wstring text(HKEY key, const wchar_t* name) {
    std::array<wchar_t, 32768> value{}; DWORD bytes = DWORD(value.size() * sizeof(wchar_t));
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t) || bytes > value.size() * sizeof(wchar_t))
        throw std::runtime_error("Native uninstall registration is incomplete");
    return value.data();
}
fs::path nativeUninstaller(const InstalledApplication& app) {
    Key key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, productKey, REG_OPTION_OPEN_LINK, KEY_READ | KEY_WOW64_64KEY, &key.value) != ERROR_SUCCESS)
        throw std::runtime_error("Native machine uninstall registration is unavailable");
    protectedRegistration(key.value);
    auto location = fs::path(text(key.value, L"InstallLocation")).lexically_normal();
    if (location.filename().empty()) location = location.parent_path();
    if (location != app.root) throw std::runtime_error("Native uninstall registration changed location");
    const auto command = text(key.value, L"UninstallString");
    if (command.size() < 3 || command.front() != L'"' || command.back() != L'"') throw std::runtime_error("Unsupported native uninstall command");
    const auto path = fs::path(command.substr(1, command.size() - 2));
    if (path.parent_path() != app.root || !std::regex_match(pathUtf8(path.filename()), std::regex("unins[0-9]{3}\\.exe")))
        throw std::runtime_error("Unverified native uninstaller location");
    return path;
}
std::string registrationProof(const fs::path& expectedRoot) {
    Key key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, productKey, REG_OPTION_OPEN_LINK, KEY_READ | KEY_WOW64_64KEY, &key.value) != ERROR_SUCCESS)
        throw std::runtime_error("Native product registration is unavailable");
    protectedRegistration(key.value);
    auto root = fs::path(text(key.value, L"InstallLocation")).lexically_normal(); if (root.filename().empty()) root = root.parent_path();
    if (root != expectedRoot) throw std::runtime_error("Native product registration changed location");
    Encoder out; out.text("M6NATIVEREGPROOF1");
    for (const auto* field : {L"InstallLocation", L"UninstallString", L"DisplayName", L"DisplayVersion", L"Publisher"}) out.text(pathUtf8(fs::path(text(key.value, field))));
    return sha256Bytes(out.bytes);
}
void artifact(UninstallInspection& inspection, const fs::path& path, const fs::path& boundary, const std::string& evidence, bool protectedRequired = true) {
    if (!entryExists(path) || intentionallyPreserved(inspection.inventory, path)) return;
    validateContainedPath(path, boundary);
    const auto identity = identifyFile(path);
    if (protectedRequired && !protectedProductFile(path, identity)) throw std::runtime_error("Native product artifact is not protected");
    if (std::none_of(inspection.inventory.targets.begin(), inspection.inventory.targets.end(), [&](const auto& target) { return target.path == path; }))
        inspection.inventory.targets.push_back({path, boundary, {}, Category::Registration, evidence, identity});
    std::erase_if(inspection.inventory.issues, [&](const auto& issue) { return issue.path == path && issue.reason == "Not in the product payload manifest; retained for review"; });
}
void inspectShortcuts(UninstallInspection& inspection) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct Com { HRESULT initialized; ~Com() { if (SUCCEEDED(initialized)) CoUninitialize(); } } com{initialized};
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) throw std::runtime_error("Cannot inspect native product shortcuts");
    for (const auto* folder : {&FOLDERID_CommonPrograms, &FOLDERID_PublicDesktop}) {
        PWSTR raw = nullptr;
        if (FAILED(SHGetKnownFolderPath(*folder, KF_FLAG_DONT_VERIFY, nullptr, &raw)) || !raw) throw std::runtime_error("Native common shortcut location is unavailable");
        const fs::path boundary(raw); CoTaskMemFree(raw); const auto path = boundary / "Mark Six Studio.lnk";
        if (!entryExists(path) || intentionallyPreserved(inspection.inventory, path)) continue;
        validateContainedPath(path, boundary); const auto before = identifyFile(path);
        Microsoft::WRL::ComPtr<IShellLinkW> link; Microsoft::WRL::ComPtr<IPersistFile> persistence;
        // An elevated COM client ignores per-user class registrations. Use the
        // native Shell Link reader without resolving or executing its target.
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.GetAddressOf()))) ||
            FAILED(link.As(&persistence)) || FAILED(persistence->Load(path.c_str(), STGM_READ))) throw std::runtime_error("Product-named shortcut has unverified native metadata; retained");
        std::array<wchar_t, 32768> target{};
        if (FAILED(link->GetPath(target.data(), int(target.size()), nullptr, SLGP_RAWPATH))) throw std::runtime_error("Cannot resolve a product shortcut's stored target");
        const auto matches = std::any_of(inspection.applications.begin(), inspection.applications.end(), [&](const auto& app) {
            if (app.registration.empty() || !app.account.empty()) return false;
            try { (void)registrationProof(app.root); } catch (const std::exception&) { return false; }
            const auto stored = fs::path(target.data()).lexically_normal();
            if (_wcsicmp(stored.c_str(), app.executable.c_str()) == 0) return true;
            std::error_code error; return fs::equivalent(stored, app.executable, error) && !error;
        });
        if (identifyFile(path) != before) throw std::runtime_error("Product shortcut changed during native inspection");
        if (matches) artifact(inspection, path, boundary, "Native Shell Link to a reviewed product executable in the installer's common shortcut location", false);
        else inspection.inventory.issues.push_back({path, {}, "Product-named shortcut points elsewhere and is intentionally retained", false});
    }
}
}
NativeInstallationState nativeInstallationState() {
    try {
        Key key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, productKey, REG_OPTION_OPEN_LINK, KEY_READ | KEY_WOW64_64KEY, &key.value) != ERROR_SUCCESS) return {};
        protectedRegistration(key.value);
        auto root = fs::path(text(key.value, L"InstallLocation")).lexically_normal(); if (root.filename().empty()) root = root.parent_path();
        const auto version = pathUtf8(fs::path(text(key.value, L"DisplayVersion")));
        if (!hasProductIdentity(root)) return {};
        const auto main = root / "mark-six-studio.exe", manifest = root / "maintenance-payload.txt";
        const auto mainId = identifyFile(main), manifestId = identifyFile(manifest);
        if (!protectedProductFile(main, mainId) || !protectedProductFile(manifest, manifestId)) return {};
        DWORD ignored = 0; const auto size = GetFileVersionInfoSizeW(main.c_str(), &ignored); if (!size || size > 1024 * 1024) return {};
        std::vector<unsigned char> bytes(size);
        if (!GetFileVersionInfoW(main.c_str(), 0, size, bytes.data())) return {};
        void* value = nullptr; UINT length = 0;
        if (!VerQueryValueW(bytes.data(), L"\\StringFileInfo\\040904b0\\ProductVersion", &value, &length) || !length ||
            pathUtf8(fs::path(std::wstring(static_cast<const wchar_t*>(value), length - 1))) != version) return {};
        return {version, sha256Bytes(version + mainId.sha256 + manifestId.sha256), true};
    } catch (const std::exception&) { return {}; }
}
void secureNativeInstallationRegistration() {
    const auto self = processIdentity(currentProcessId());
    if (!administrator() || !self || self->uncertain || self->executable.filename() != L"mark-six-maintenance.exe")
        throw std::runtime_error("Registration protection requires the installed administrator helper");
    const auto root = self->executable.parent_path();
    const auto main = root / "mark-six-studio.exe", manifest = root / "maintenance-payload.txt";
    if (!protectedProductFile(self->executable) || !protectedProductFile(main) || !protectedProductFile(manifest) || !hasProductIdentity(root))
        throw std::runtime_error("Installed product files do not have protected native ownership");
    Key key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, productKey, REG_OPTION_OPEN_LINK, KEY_READ | WRITE_DAC | WRITE_OWNER | KEY_WOW64_64KEY, &key.value) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot protect native product registration");
    rejectRegistrationLink(key.value);
    auto registered = fs::path(text(key.value, L"InstallLocation")).lexically_normal();
    if (registered.filename().empty()) registered = registered.parent_path();
    if (registered != root) throw std::runtime_error("Registration does not match this independently verified installed helper");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;BU)", SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("Cannot construct protected product registration permissions");
    struct Security { PSECURITY_DESCRIPTOR value; ~Security() { LocalFree(value); } } guard{descriptor};
    PSID owner = nullptr; PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) || !GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) || !present || !acl ||
        SetSecurityInfo(reinterpret_cast<HANDLE>(key.value), SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            owner, nullptr, acl, nullptr) != ERROR_SUCCESS || RegFlushKey(key.value) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot apply protected product registration permissions");
    protectedRegistration(key.value);
}
bool nativeInstallersIdle() {
    for (const auto* name : {L"Global\\_MSIExecute", L"_MSIExecute"}) {
        const auto mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
        if (!mutex) { if (GetLastError() != ERROR_FILE_NOT_FOUND) return false; continue; }
        const auto status = WaitForSingleObject(mutex, 0);
        if (status == WAIT_OBJECT_0 || status == WAIT_ABANDONED) ReleaseMutex(mutex);
        CloseHandle(mutex);
        if (status != WAIT_OBJECT_0) return false;
    }
    Key installation;
    const auto transaction = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer\\InProgress", 0, KEY_READ | KEY_WOW64_64KEY, &installation.value);
    if (transaction != ERROR_FILE_NOT_FOUND && transaction != ERROR_PATH_NOT_FOUND) return false;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W process{}; process.dwSize = sizeof(process);
    bool idle = true;
    if (!Process32FirstW(snapshot, &process)) idle = false;
    else do {
        std::wstring name(process.szExeFile);
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c; });
        if (name == L"vc_redist.x64.exe" || name.starts_with(L"marksixstudio-") ||
            (name.starts_with(L"unins") && name.ends_with(L".exe"))) { idle = false; break; }
    } while (Process32NextW(snapshot, &process));
    CloseHandle(snapshot); return idle;
}
void inspectNativeRegistrations(UninstallInspection& inspection) {
    Encoder state; state.text("M6WINREG2");
    std::vector<std::pair<fs::path, std::string>> registrations;
    for (const auto& app : inspection.applications) {
        if (app.registration.empty()) continue;
        try {
            if (!app.account.empty()) throw std::runtime_error("Unsupported per-account native product registration; retained for explicit review");
            const auto exe = nativeUninstaller(app); auto log = exe; log.replace_extension(".dat");
            registrations.emplace_back(app.root, registrationProof(app.root));
            artifact(inspection, exe, app.root, "Protected Inno executable bound to the compiled product GUID and native uninstall registration");
            artifact(inspection, log, app.root, "Protected Inno log paired with the independently registered product uninstaller");
            inspection.nativeReview += "Remove the matching Inno product registration using native Windows APIs after exact approved artifact removal: " + quotedReportField(pathUtf8(app.root)) + "\n";
        } catch (const std::exception& error) { inspection.inventory.issues.push_back({app.root, app.account, error.what(), true}); }
    }
    try { inspectShortcuts(inspection); }
    catch (const std::exception& error) { inspection.inventory.issues.push_back({{}, {}, error.what(), true}); }
    state.number(registrations.size());
    for (const auto& [root, proof] : registrations) { state.path(root); state.text(proof); }
    inspection.nativeState = state.bytes;
}
void removeNativeRegistrations(const UninstallInspection& inspection) {
    Decoder state(inspection.nativeState);
    if (state.text() != "M6WINREG2") throw std::runtime_error("Unsupported native Windows recovery state");
    const auto count = state.count(); if (count > 1) throw std::runtime_error("Ambiguous native machine product registration");
    for (std::size_t i = 0; i < count; ++i) {
        const auto root = state.path(); const auto proof = state.text(); Key key;
        const auto opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, productKey, REG_OPTION_OPEN_LINK, KEY_READ | KEY_WOW64_64KEY, &key.value);
        if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) continue;
        if (opened != ERROR_SUCCESS || registrationProof(root) != proof) throw std::runtime_error("Native product registration changed; retained for fresh review");
        // The ordinary Inno log can remove changed or preserved paths without
        // checking this inventory. Complete cleanup instead uses the same
        // Inno administrator bootstrap and exact native file/registry APIs.
        // Ordinary OS uninstall remains unchanged and data-retaining.
        RegCloseKey(key.value); key.value = nullptr;
        if (RegDeleteKeyExW(HKEY_LOCAL_MACHINE, productKey, KEY_WOW64_64KEY, 0) != ERROR_SUCCESS)
            throw std::runtime_error("Native product registration remains or has unsupported subkeys");
    }
    state.end(); Key key;
    const auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, productKey, 0, KEY_READ | KEY_WOW64_64KEY, &key.value);
    if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) throw std::runtime_error("Product uninstall registration remains or cannot be inspected");
}
}
