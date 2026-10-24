#include "native_runtime.hpp"
#include <array>
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <bcrypt.h>
#include <sddl.h>
#else
#include <unistd.h>
#include <libproc.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#endif

namespace marksix::maintenance {
#ifdef _WIN32
namespace {
std::string accountFor(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
    DWORD bytes = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> data(bytes);
    const bool ok = GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes) != 0;
    CloseHandle(token);
    if (!ok) return {};
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(data.data())->User.Sid, &text)) return {};
    const auto result = pathUtf8(fs::path(text)); LocalFree(text); return result;
}
}
#endif
std::uint32_t currentProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return std::uint32_t(getpid());
#endif
}
std::string currentAccount() {
#ifdef _WIN32
    const auto result = accountFor(GetCurrentProcess());
    if (result.empty()) throw std::runtime_error("Cannot resolve current OS account");
    return result;
#else
    return std::to_string(getuid());
#endif
}
bool administrator() {
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{}; DWORD bytes = sizeof(elevation);
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) && elevation.TokenIsElevated;
    CloseHandle(token); return elevated;
#else
    return geteuid() == 0;
#endif
}
std::optional<ProcessIdentity> processIdentity(std::uint32_t pid) {
#ifdef _WIN32
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    FILETIME started{}, exited{}, kernel{}, user{};
    DWORD code = 0;
    std::array<wchar_t, 32768> path{}; DWORD count = DWORD(path.size());
    const bool ok = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE &&
        GetProcessTimes(process, &started, &exited, &kernel, &user) && QueryFullProcessImageNameW(process, 0, path.data(), &count);
    const auto account = accountFor(process); CloseHandle(process);
    if (!ok) return {};
    return ProcessIdentity{pid, (std::uint64_t(started.dwHighDateTime) << 32) | started.dwLowDateTime,
        account, fs::path(std::wstring(path.data(), count)), account.empty()};
#else
    proc_bsdinfo info{};
    if (proc_pidinfo(int(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) {
        // Full BSD info has a same-user restriction. The public BSD process
        // snapshot still supplies the native start time and credentials of an
        // administrator peer; PID alone is never substituted for its birth.
        int mib[]{CTL_KERN, KERN_PROC, KERN_PROC_PID, int(pid)}; kinfo_proc kernel{}; std::size_t bytes = sizeof(kernel);
        if (sysctl(mib, 4, &kernel, &bytes, nullptr, 0) || bytes != sizeof(kernel) || kernel.kp_proc.p_pid != int(pid) || kernel.kp_proc.p_stat == SZOMB) return {};
        info.pbi_start_tvsec = std::uint64_t(kernel.kp_proc.p_starttime.tv_sec); info.pbi_start_tvusec = std::uint64_t(kernel.kp_proc.p_starttime.tv_usec);
        info.pbi_uid = kernel.kp_eproc.e_ucred.cr_uid;
    }
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
    const bool known = proc_pidpath(int(pid), path.data(), std::uint32_t(path.size())) > 0;
    return ProcessIdentity{pid, info.pbi_start_tvsec * 1000000ULL + info.pbi_start_tvusec,
        std::to_string(info.pbi_uid), known ? fs::path(path.data()) : fs::path(info.pbi_name), !known};
#endif
}
bool sameProcessAlive(const ProcessIdentity& process) {
    const auto now = processIdentity(process.pid);
    return now && now->started == process.started && (now->account.empty() || process.account.empty() || now->account == process.account);
}
std::vector<ProcessIdentity> runningApplications(const std::string& account) {
    std::vector<ProcessIdentity> result;
    const auto add = [&](std::uint32_t pid, const fs::path& hint) {
        auto info = processIdentity(pid);
        if (!info) info = ProcessIdentity{pid, 0, {}, hint, true};
        auto name = pathUtf8(info->executable.filename());
        for (auto& letter : name) if (letter >= 'A' && letter <= 'Z') letter = char(letter - 'A' + 'a');
        if (name != "mark-six-studio" && name != "mark-six-studio.exe") return;
        if (!account.empty() && !info->account.empty() && account != info->account) return;
        result.push_back(std::move(*info));
    };
#ifdef _WIN32
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot inspect active application instances");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do { add(entry.th32ProcessID, fs::path(entry.szExeFile)); } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
#else
    const auto bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0) throw std::runtime_error("Cannot inspect active application instances");
    std::vector<pid_t> pids(std::size_t(bytes) / sizeof(pid_t) + 128);
    const auto received = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), int(pids.size() * sizeof(pid_t)));
    if (received <= 0) throw std::runtime_error("Cannot inspect active application instances");
    for (int i = 0; i < received / int(sizeof(pid_t)); ++i) if (pids[std::size_t(i)] > 0) {
        std::array<char, 1024> name{};
        proc_name(pids[std::size_t(i)], name.data(), std::uint32_t(name.size()));
        add(std::uint32_t(pids[std::size_t(i)]), fs::path(name.data()));
    }
#endif
    return result;
}
std::string randomToken() {
    std::array<unsigned char, 32> bytes{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes.data(), ULONG(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) throw std::runtime_error("Random source unavailable");
#else
    arc4random_buf(bytes.data(), bytes.size());
#endif
    std::string text;
    for (const auto byte : bytes) { text += "0123456789abcdef"[byte >> 4]; text += "0123456789abcdef"[byte & 15]; }
    return text;
}
fs::path normalizePlatformPath(const fs::path& path) {
    auto result = fs::absolute(path).lexically_normal();
#ifndef _WIN32
    // Darwin's fixed /var and /tmp aliases are used by native temporary-folder
    // APIs. Resolve only these OS aliases, never a user-selected link below
    // them. All other ancestors remain subject to no-follow validation.
    for (const auto* name : {"var", "tmp"}) {
        const auto alias = fs::path("/") / name;
        if (result == alias || (!result.lexically_relative(alias).empty() && *result.lexically_relative(alias).begin() != "..")) {
            std::error_code error;
            if (fs::read_symlink(alias, error) != fs::path("private") / name || error)
                throw std::runtime_error("Unexpected platform storage alias");
            result = (fs::path("/private") / name / result.lexically_relative(alias)).lexically_normal();
        }
    }
#endif
    return result;
}
fs::path sessionLeasePath(const fs::path& dataRoot) { return dataRoot / "statistics/.marksix-session.lock"; }
fs::path accountLeasePath(const UserStorage& user) { return user.manifestPath.parent_path() / ".desktop.lock"; }
std::unique_ptr<FileLease> acquireCurrentDesktopLease() {
    std::vector<InventoryIssue> issues; const auto users = discoverUsers(false, issues);
    const auto account = currentAccount();
    const auto found = std::find_if(users.begin(), users.end(), [&](const auto& user) { return user.account == account; });
    if (found == users.end()) throw std::runtime_error("Cannot resolve the current account's native maintenance coordination path");
    auto lease = FileLease::acquire(accountLeasePath(*found), FileLease::Mode::Shared);
    if (!lease) throw std::runtime_error("Storage cleanup is active for this OS account. Finish its native helper before opening another app instance.");
    return lease;
}
std::unique_ptr<FileLease> FileLease::acquire(const fs::path& path, Mode mode, bool create) {
    validateContainedPath(path, path.parent_path());
    if (create) fs::create_directories(path.parent_path());
    validateContainedPath(path, path.parent_path());
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    OVERLAPPED overlap{};
    if (!GetFileInformationByHandle(handle, &info) || info.nNumberOfLinks != 1 || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        !LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY | (mode == Mode::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0, 1, 0, &overlap)) {
        CloseHandle(handle); return {};
    }
    return std::unique_ptr<FileLease>(new FileLease(reinterpret_cast<std::intptr_t>(handle)));
#else
    const int handle = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0), 0644);
    if (handle < 0) return {};
    struct stat info{};
    if (fstat(handle, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 || flock(handle, LOCK_NB | (mode == Mode::Exclusive ? LOCK_EX : LOCK_SH))) {
        close(handle); return {};
    }
    return std::unique_ptr<FileLease>(new FileLease(handle));
#endif
}
bool FileLease::matchesEmptyFile(const FileIdentity& expected) const {
    if (handle_ == -1 || expected.bytes || !expected.linkTarget.empty() || expected.sha256 != sha256Bytes("")) return false;
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(reinterpret_cast<HANDLE>(handle_), &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    return expected.volume == info.dwVolumeSerialNumber && expected.file == ((std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow) &&
        !info.nFileSizeHigh && !info.nFileSizeLow && expected.modified == std::int64_t((std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime);
#else
    struct stat info{};
    return !fstat(int(handle_), &info) && S_ISREG(info.st_mode) && info.st_nlink == 1 && !info.st_size && expected.volume == std::uint64_t(info.st_dev) &&
        expected.file == std::uint64_t(info.st_ino) && expected.modified == info.st_mtimespec.tv_sec * 1000000000LL + info.st_mtimespec.tv_nsec;
#endif
}
FileLease::~FileLease() {
#ifdef _WIN32
    if (handle_ != -1) { OVERLAPPED overlap{}; UnlockFileEx(reinterpret_cast<HANDLE>(handle_), 0, 1, 0, &overlap); CloseHandle(reinterpret_cast<HANDLE>(handle_)); }
#else
    if (handle_ != -1) { flock(int(handle_), LOCK_UN); close(int(handle_)); }
#endif
}
}
