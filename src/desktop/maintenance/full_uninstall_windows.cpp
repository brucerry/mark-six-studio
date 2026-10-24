#include "full_uninstall.hpp"
#include "durable_files.hpp"
#include "payload_authority.hpp"
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <aclapi.h>
#include <sddl.h>
#include <array>
#include <regex>

namespace marksix::maintenance {
namespace {
void protect(const fs::path& path, bool directory) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto sddl = directory ? L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;0x1200a9;;;BU)" : L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) throw std::runtime_error("Cannot secure protected uninstall staging");
    const bool okay = SetFileSecurityW(path.c_str(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor) != 0;
    LocalFree(descriptor); if (!okay) throw std::runtime_error("Cannot apply protected uninstall permissions");
}
fs::path registeredUninstaller(const fs::path& application) {
    HKEY key = nullptr;
    constexpr wchar_t product[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{3B52BF71-65D1-4C1F-A25E-0A3B16A7B3C8}_is1";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, product, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) throw std::runtime_error("Native machine uninstall registration is unavailable");
    std::array<wchar_t, 32768> value{}; DWORD bytes = DWORD(value.size() * sizeof(wchar_t));
    const auto status = RegGetValueW(key, nullptr, L"UninstallString", RRF_RT_REG_SZ, nullptr, value.data(), &bytes); RegCloseKey(key);
    if (status != ERROR_SUCCESS || bytes < 3 * sizeof(wchar_t) || bytes > value.size() * sizeof(wchar_t)) throw std::runtime_error("Invalid native uninstall registration");
    const std::wstring command(value.data());
    if (command.front() != L'"' || command.back() != L'"') throw std::runtime_error("Unsupported native uninstall command");
    const fs::path path(command.substr(1, command.size() - 2));
    if (path.parent_path() != application || !std::regex_match(pathUtf8(path.filename()), std::regex("unins[0-9]{3}\\.exe")) || !protectedProductFile(path))
        throw std::runtime_error("The original installed native uninstaller is not trusted");
    return path;
}
}
fs::path privilegedUninstallStage(const std::string& nonce) {
    if (nonce.size() != 64 || nonce.find_first_not_of("0123456789abcdef") != std::string::npos) throw std::runtime_error("Invalid protected uninstall identity");
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DONT_VERIFY, nullptr, &value)) || !value) throw std::runtime_error("Native ProgramData location is unavailable");
    const fs::path parent(value); CoTaskMemFree(value);
    return parent / ("MarkSixStudio-Maintenance-" + nonce);
}
bool protectedUninstallStage(const fs::path& stage) {
    try { identifyDirectory(stage); } catch (const std::exception&) { return false; }
    PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(const_cast<LPWSTR>(stage.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> admin{}, system{}; DWORD a = DWORD(admin.size()), s = DWORD(system.size());
    bool okay = CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin.data(), &a) && CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &s) &&
        owner && acl && (EqualSid(owner, admin.data()) || EqualSid(owner, system.data()));
    for (DWORD i = 0; okay && i < acl->AceCount; ++i) {
        void* entry = nullptr;
        if (!GetAce(acl, i, &entry) || reinterpret_cast<ACE_HEADER*>(entry)->AceType != ACCESS_ALLOWED_ACE_TYPE) { okay = false; break; }
        const auto* ace = reinterpret_cast<ACCESS_ALLOWED_ACE*>(entry); const auto sid = const_cast<DWORD*>(&ace->SidStart);
        constexpr DWORD writes = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
        if ((ace->Mask & writes) && !EqualSid(sid, admin.data()) && !EqualSid(sid, system.data())) okay = false;
    }
    LocalFree(descriptor); return okay;
}
std::shared_ptr<void> reserveInstallerLifecycle() {
    const auto handle = CreateMutexW(nullptr, TRUE, L"Global\\MarkSixStudioInstaller-3B52BF71-65D1-4C1F-A25E-0A3B16A7B3C8");
    const auto status = GetLastError();
    if (!handle || status == ERROR_ALREADY_EXISTS) { if (handle) CloseHandle(handle); throw std::runtime_error("A native product setup is active; finish it before retrying cleanup"); }
    return std::shared_ptr<void>(handle, [](void* value) { ReleaseMutex(value); CloseHandle(value); });
}
int launchUninstallAuthorization(const ElevationRequest& request, const fs::path& application, bool retry) {
    const auto parameters = windowsBootstrapParameters(request); // Strict, bounded coordinates only.
    const auto executable = retry ? privilegedUninstallStage(request.nonce) / "mark-six-maintenance.exe" : registeredUninstaller(application);
    if (!protectedProductFile(executable)) throw std::runtime_error("Native authorization source is not protected");
    std::string arguments;
    if (retry) arguments = "--uninstall-broker " + request.endpoint + ' ' + request.nonce + ' ' + std::to_string(request.agent.pid) + ' ' + std::to_string(request.agent.started) + ' ' + request.agent.account;
    else arguments = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART " + parameters;
    const auto native = pathFromUtf8(arguments).native();
    SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas"; info.lpFile = executable.c_str(); info.lpParameters = native.c_str(); info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) return GetLastError() == ERROR_CANCELLED ? 1223 : 1;
    if (info.hProcess) CloseHandle(info.hProcess); return 0;
}
int bootstrapUninstallWindows(const ElevationRequest& request) {
    (void)windowsBootstrapParameters(request);
    const auto self = processIdentity(currentProcessId());
    if (!self || self->uncertain || !administrator() || !protectedProductFile(self->executable)) throw std::runtime_error("Inno bootstrap helper is not protected administrator code");
    if (request.agent.executable.filename() != self->executable.filename() || sha256File(request.agent.executable) != sha256File(self->executable))
        throw std::runtime_error("Original-user review agent does not match the installed trusted helper");
    const auto stage = privilegedUninstallStage(request.nonce);
    makePrivateDirectory(stage); protect(stage, true);
    const auto executable = stage / "mark-six-maintenance.exe";
    if (!CopyFileW(self->executable.c_str(), executable.c_str(), TRUE)) throw std::runtime_error("Cannot stage the standalone administrator helper");
    protect(executable, false);
    if (sha256File(executable) != sha256File(self->executable) || !protectedProductFile(executable) || !protectedUninstallStage(stage)) throw std::runtime_error("Protected helper copy failed identity checks");
    auto command = L"\"" + executable.native() + L"\" --uninstall-broker " + pathFromUtf8(request.endpoint + ' ' + request.nonce + ' ' +
        std::to_string(request.agent.pid) + ' ' + std::to_string(request.agent.started) + ' ' + request.agent.account).native();
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, stage.c_str(), &startup, &child)) throw std::runtime_error("Cannot start protected uninstall broker");
    CloseHandle(child.hThread); CloseHandle(child.hProcess); return 0;
}
FullCleanupFinalization finalizeUninstallBroker(const fs::path& executable, const MachineGate& gate, const ProcessIdentity& userAgent, bool success) {
    if (!success) return {};
    if (!administrator() || gate.stage != privilegedUninstallStage(gate.nonce) || executable.parent_path() != gate.stage || !protectedUninstallStage(gate.stage) || !protectedProductFile(executable))
        throw std::runtime_error("Protected Windows finalization failed native authentication");
    return launchPrivilegedFinalizer(executable, gate, userAgent);
}
}
