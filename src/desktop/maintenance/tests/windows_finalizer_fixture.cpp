#include "helper_lifecycle.hpp"
#include "native_runtime.hpp"
#include "durable_files.hpp"
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

using namespace marksix::maintenance;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Handle { HANDLE value = nullptr; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
void write(const fs::path& path, std::string_view value) { durableWrite(path, value); }
struct WindowSearch { DWORD pid; HWND window = nullptr; };
HWND findWindow(DWORD pid) {
    WindowSearch result{pid};
    EnumWindows([](HWND window, LPARAM context) -> BOOL {
        auto& result = *reinterpret_cast<WindowSearch*>(context); DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == result.pid && IsWindowVisible(window)) { result.window = window; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result.window;
}
std::wstring reportText(HWND window) {
    std::wstring text;
    EnumChildWindows(window, [](HWND child, LPARAM context) -> BOOL {
        std::array<wchar_t, 256> type{}; GetClassNameW(child, type.data(), int(type.size()));
        if (!std::wstring_view(type.data()).starts_with(L"WindowsForms10.EDIT")) return TRUE;
        DWORD_PTR size = 0;
        if (!SendMessageTimeoutW(child, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &size) || !size || size > 1024 * 1024) return TRUE;
        std::wstring value(std::size_t(size) + 1, L'\0'); DWORD_PTR copied = 0;
        if (!SendMessageTimeoutW(child, WM_GETTEXT, WPARAM(size + 1), reinterpret_cast<LPARAM>(value.data()), SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &copied)) return TRUE;
        value.resize(std::size_t(copied));
        *reinterpret_cast<std::wstring*>(context) = std::move(value); return FALSE;
    }, reinterpret_cast<LPARAM>(&text));
    return text;
}
std::optional<ProcessIdentity> finalizerProcess(DWORD parent, std::uint64_t after) {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.value, &entry)) do {
        if (entry.th32ParentProcessID != parent || _wcsicmp(entry.szExeFile, L"powershell.exe")) continue;
        const auto process = processIdentity(entry.th32ProcessID);
        if (process && process->started >= after) return process;
    } while (Process32NextW(snapshot.value, &entry));
    return {};
}
void run(const fs::path& executable, const fs::path& legacy, bool partial) {
    auto command = L"\"" + executable.native() + L"\" --finish-fixture";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION information{};
    check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        executable.parent_path().c_str(), &startup, &information) != 0, "Could not start isolated finalizer fixture");
    Handle process{information.hProcess}, thread{information.hThread};
    const auto identity = processIdentity(information.dwProcessId); check(bool(identity), "Fixture child identity unavailable");
    std::optional<ProcessIdentity> finalizer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    bool observed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!finalizer) finalizer = finalizerProcess(information.dwProcessId, identity->started);
        if (finalizer && sameProcessAlive(*finalizer)) {
            const auto window = findWindow(finalizer->pid);
            if (window) {
                const auto text = reportText(window);
                DWORD owner = 0; GetWindowThreadProcessId(window, &owner);
                if (owner == finalizer->pid && sameProcessAlive(*finalizer)) PostMessageW(window, WM_CLOSE, 0, 0);
                check(text.find(L"Preserved legacy files (1)") != std::wstring::npos, "Native result omitted preserved-file section");
                check(text.find(pathFromUtf8(quotedReportField(pathUtf8(legacy))).native()) != std::wstring::npos, "Native result lost the full Unicode legacy path");
                if (partial) {
                    check(entryExists(executable) && entryExists(executable.parent_path() / "remaining.txt"), "Locked helper did not retain runnable/readable recovery");
                    check(text.find(L"Partial cleanup") != std::wstring::npos, "Locked helper incorrectly reported completion");
                } else check(!entryExists(executable.parent_path()), "Native epilogue left its helper, state or report behind");
                observed = true; break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!observed) { TerminateProcess(process.value, 1); throw std::runtime_error("No native result appeared for the isolated finalizer fixture"); }
    check(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0, "Fixture helper did not exit");
    DWORD status = 1; check(GetExitCodeProcess(process.value, &status) && !status, "Fixture helper failed before handoff");
}
}
int main(int argc, char** argv) {
    fs::path root;
    try {
        std::array<wchar_t, 32768> runner{}, actions{};
        check(GetEnvironmentVariableW(L"GITHUB_ACTIONS", actions.data(), DWORD(actions.size())) && std::wstring_view(actions.data()) == L"true",
            "This fixture runs only on a disposable GitHub runner");
        check(GetEnvironmentVariableW(L"RUNNER_TEMP", runner.data(), DWORD(runner.size())) != 0, "Runner temporary directory is unavailable");
        const auto self = processIdentity(currentProcessId()); check(bool(self), "Fixture image unavailable");
        if (argc == 2 && std::string_view(argv[1]) == "--finish-fixture") {
            const auto stage = self->executable.parent_path();
            const auto base = stage.parent_path();
            validateContainedPath(stage, fs::canonical(fs::path(runner.data())));
            check(pathUtf8(base.filename()).starts_with("marksix-finalizer-fixture-"), "Fixture image is outside its reserved runner tree");
            Inventory preserved; preserveLegacyFile(preserved, base / pathFromUtf8("legacy-測試.MSX"), currentAccount(), "Isolated fixture recording; preserved without reading its format.");
            finishUserHelper(self->executable, true, "Current-app cleanup completed with legacy files preserved.\n\n" + preservedLegacyReport(preserved), {}, base / "data");
            return 0;
        }
        root = fs::canonical(fs::path(runner.data())) / ("marksix-finalizer-fixture-" + randomToken());
        makePrivateDirectory(root);
        const auto legacy = root / pathFromUtf8("legacy-測試.MSX"); write(legacy, "opaque fixture recording"); const auto digest = sha256File(legacy);
        for (int scenario = 0; scenario < 4; ++scenario) {
            const auto stage = root / ("marksix-maintenance-" + randomToken()); makePrivateDirectory(stage);
            const auto executable = stage / "mark-six-maintenance.exe";
            fs::copy_file(self->executable, executable); write(stage / "request.bin", "isolated fixture request");
            if (scenario == 1 || scenario == 3) write(stage / "remaining.txt", "earlier partial result");
            Handle lock;
            if (scenario >= 2) {
                const auto locked = scenario == 2 ? executable : stage / "remaining.txt";
                lock.value = CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                check(lock.value != INVALID_HANDLE_VALUE, "Cannot reserve locked-image fixture");
            }
            run(executable, legacy, scenario >= 2);
            if (scenario >= 2) { CloseHandle(lock.value); lock.value = nullptr; run(executable, legacy, false); }
            check(sha256File(legacy) == digest, "Finalization changed legacy fixture bytes");
        }
        fs::remove(legacy); fs::remove(root); root.clear();
        std::cout << "Native Windows finalizer: success, retry-report removal, locked image/report, retry and full Unicode preserved list passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture retained at " << pathUtf8(root) << '\n';
        return argc == 1 && !GetEnvironmentVariableW(L"GITHUB_ACTIONS", nullptr, 0) ? 77 : 1;
    }
}
