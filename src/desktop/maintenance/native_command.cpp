#include "native_command.hpp"
#include <algorithm>
#include <array>
#include <thread>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <spawn.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace marksix::maintenance {
namespace {
constexpr std::size_t outputLimit = 4 * 1024 * 1024;
void append(std::string& output, const char* bytes, std::size_t count) {
    if (count > outputLimit - output.size()) throw std::runtime_error("Native command output exceeds limit");
    output.append(bytes, count);
}
#ifdef _WIN32
struct Handle { HANDLE value = nullptr; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\""; std::size_t slashes = 0;
    for (const auto letter : value) {
        if (letter == L'\\') { ++slashes; continue; }
        if (letter == L'"') { result.append(slashes * 2 + 1, L'\\'); result += letter; }
        else { result.append(slashes, L'\\'); result += letter; }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\'); return result + L'"';
}
#else
struct Descriptor { int value = -1; ~Descriptor() { if (value >= 0) close(value); } };
#endif
}
NativeCommandResult runNativeCommand(const fs::path& executable, const std::vector<std::string>& arguments, std::chrono::seconds timeout) {
    if (!executable.is_absolute() || executable != executable.lexically_normal() || timeout.count() <= 0 || timeout > std::chrono::minutes(30) || arguments.size() > 128)
        throw std::runtime_error("Invalid native command invocation");
    for (const auto& argument : arguments) if (argument.size() > 32768 || argument.find('\0') != std::string::npos) throw std::runtime_error("Invalid native command argument");
    NativeCommandResult result; const auto deadline = std::chrono::steady_clock::now() + timeout;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle input, output;
    if (!CreatePipe(&input.value, &output.value, &security, 0) || !SetHandleInformation(input.value, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("Cannot capture native command output");
    Handle nullInput{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr)};
    if (nullInput.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot reserve native command input");
    auto command = quote(executable.native());
    for (const auto& argument : arguments) command += L' ' + quote(pathFromUtf8(argument).native());
    if (command.size() >= 32767) throw std::runtime_error("Native command line exceeds system limit");
    std::array<wchar_t, 32768> directory{}; const auto count = GetSystemDirectoryW(directory.data(), UINT(directory.size()));
    if (!count || count >= directory.size()) throw std::runtime_error("Native system directory unavailable");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput.value; startup.hStdOutput = output.value; startup.hStdError = output.value;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, directory.data(), &startup, &child)) throw std::runtime_error("Cannot start native command");
    Handle process{child.hProcess}, thread{child.hThread}; result.process = processIdentity(child.dwProcessId);
    CloseHandle(output.value); output.value = nullptr;
    try {
        bool exited = false;
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(input.value, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() != ERROR_BROKEN_PIPE) throw std::runtime_error("Native output capture failed");
                if (exited) break;
            } else if (available) {
                std::array<char, 65536> bytes{}; DWORD received = 0;
                if (!ReadFile(input.value, bytes.data(), std::min<DWORD>(available, DWORD(bytes.size())), &received, nullptr)) throw std::runtime_error("Native output capture interrupted");
                append(result.output, bytes.data(), received); continue;
            } else if (exited) break;
            exited = WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0;
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Native command timed out; completion remains unverified");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        DWORD status = 0; if (!GetExitCodeProcess(process.value, &status)) throw std::runtime_error("Native command exit is unverified");
        result.status = int(status); return result;
    } catch (...) { TerminateProcess(process.value, 1); WaitForSingleObject(process.value, 5000); throw; } // Only this owned child.
#else
    int pipe[2]{};
    if (::pipe(pipe)) throw std::runtime_error("Cannot capture native command output");
    Descriptor input{pipe[0]}, output{pipe[1]};
    if (fcntl(input.value, F_SETFD, FD_CLOEXEC) || fcntl(output.value, F_SETFD, FD_CLOEXEC) || fcntl(input.value, F_SETFL, O_NONBLOCK))
        throw std::runtime_error("Cannot secure native command handles");
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) throw std::runtime_error("Cannot prepare native process");
    struct Actions { posix_spawn_file_actions_t* value; ~Actions() { posix_spawn_file_actions_destroy(value); } } actionGuard{&actions};
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) ||
        posix_spawn_file_actions_adddup2(&actions, output.value, STDOUT_FILENO) || posix_spawn_file_actions_adddup2(&actions, output.value, STDERR_FILENO))
        throw std::runtime_error("Cannot prepare native command streams");
    std::vector<std::string> strings{pathUtf8(executable)}; strings.insert(strings.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv; for (auto& string : strings) argv.push_back(string.data()); argv.push_back(nullptr);
    // No DYLD, shell startup, caller PATH or executable-injection environment.
    std::vector<std::string> environment{"PATH=/usr/bin:/bin:/usr/sbin:/sbin", "LANG=en_US.UTF-8"};
    std::vector<char*> env; for (auto& string : environment) env.push_back(string.data()); env.push_back(nullptr);
    pid_t pid = 0; if (posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), env.data())) throw std::runtime_error("Cannot start native command");
    result.process = processIdentity(std::uint32_t(pid));
    close(output.value); output.value = -1;
    bool exited = false; int status = 0;
    try {
        for (;;) {
            std::array<char, 65536> bytes{}; const auto count = read(input.value, bytes.data(), bytes.size());
            if (count > 0) { append(result.output, bytes.data(), std::size_t(count)); continue; }
            if (count < 0 && errno != EAGAIN && errno != EINTR) throw std::runtime_error("Native output capture interrupted");
            if (!exited) {
                const auto waited = waitpid(pid, &status, WNOHANG);
                if (waited < 0) throw std::runtime_error("Native process exit cannot be verified");
                exited = waited == pid;
            }
            if (exited && !count) break;
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Native command timed out; completion remains unverified");
            pollfd wait{input.value, POLLIN, 0}; (void)poll(&wait, 1, 10);
        }
        result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1; return result;
    } catch (...) {
        if (!exited) { (void)kill(pid, SIGTERM); (void)kill(pid, SIGKILL); (void)waitpid(pid, &status, 0); } // Only this unreaped, owned child.
        throw;
    }
#endif
}
}
