#include "installer_lease.hpp"
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

namespace marksix::maintenance {
void launchNativeInstaller(const fs::path& package, InstallerCompletion completed) {
    SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"open"; info.lpFile = package.c_str(); info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        completed({false, {}, GetLastError() == ERROR_CANCELLED ? "Installer authorization was cancelled." : "The native installer could not be opened."}); return;
    }
    std::optional<ProcessIdentity> identity;
    if (info.hProcess) { identity = processIdentity(GetProcessId(info.hProcess)); CloseHandle(info.hProcess); }
    completed({true, identity, {}});
}
}
