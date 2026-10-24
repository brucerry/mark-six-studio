// Never packaged. Drives the production broker/providers against an installed
// disposable CI payload, with scripted consent in this test executable only.
#include "full_uninstall.hpp"
#include "durable_files.hpp"
#include "native_command.hpp"
#include "wire_codec.hpp"
#include <future>
#include <iostream>
#include <thread>
#include <array>
#include <cstdlib>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <sddl.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace marksix::maintenance;
namespace {
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
ReleaseCatalog fixtureCatalog() {
    // Independently published v0.1.0 asset identities, pinned in this trusted
    // nonpackaged binary. Network/rate-limit outages are tested separately;
    // they must not make a destructive native fixture nondeterministic.
    ReleaseCatalog catalog;
    catalog.assets = {
        {"MarkSixStudio-0.1.0-macos-universal-unsigned.pkg", "d567cd9d97ce91921be652df40de37f6d161e431e67f4e714e2c06418395390c", "v0.1.0", 47802949, AssetRole::Installer},
        {"MarkSixStudio-0.1.0-windows-x64-unsigned-setup.exe", "2680e167acd58e3638ba11331f115195fa37c46dab336d47fa1673089f9ebe7a", "v0.1.0", 43349208, AssetRole::Installer}};
    return catalog;
}
std::string diagnostic(const fs::path& stage) {
    if (!entryExists(stage / "remaining.txt")) return {};
    const auto report = readBoundedFile(stage / "remaining.txt");
    if (report.size() <= 20000) return report;
    return report.substr(0, 16000) + "\n[Full inventory retained in remaining.txt]\n" + report.substr(report.size() - 4000);
}
fs::path runnerTemp() {
    const auto actions = std::getenv("GITHUB_ACTIONS"), temporary = std::getenv("RUNNER_TEMP");
    check(actions && std::string(actions) == "true" && temporary && *temporary, "Native removal fixture requires a disposable GitHub runner");
    return normalizePlatformPath(pathFromUtf8(temporary));
}
struct State { fs::path app, data, legacy; std::string nonce; unsigned mode = 0; };
std::string encode(const State& state) { Encoder out; out.text("M6NATIVEFIXTURE2"); out.path(state.app); out.path(state.data); out.path(state.legacy); out.text(state.nonce); out.number(state.mode); return out.bytes; }
State decode(std::string_view bytes) {
    Decoder in(bytes); check(in.text() == "M6NATIVEFIXTURE2", "Invalid native fixture state"); State result{in.path(), in.path(), in.path(), in.text()};
    const auto mode = in.number(); check(mode <= 3, "Invalid fixture mode"); result.mode = unsigned(mode); in.end(); return result;
}
std::string control(const char* kind, const std::string& nonce) { Encoder out; out.text(kind); out.text(nonce); return out.bytes; }
void expect(std::string_view bytes, const char* kind, const std::string& nonce) { Decoder in(bytes); check(in.text() == kind && in.text() == nonce, "Native fixture protocol mismatch"); in.end(); }
NativeCommandResult fixtureCommand(const fs::path& executable, std::vector<std::string> arguments, std::chrono::seconds timeout) {
#ifdef _WIN32
    return runNativeCommand(executable, arguments, timeout);
#else
    // Production native commands intentionally strip caller environment.
    // This nonpackaged fixture explicitly forwards only its two CI guards.
    arguments.insert(arguments.begin(), pathUtf8(executable));
    arguments.insert(arguments.begin(), "RUNNER_TEMP=" + pathUtf8(runnerTemp()));
    arguments.insert(arguments.begin(), "GITHUB_ACTIONS=true");
    return runNativeCommand("/usr/bin/env", arguments, timeout);
#endif
}
ElevationRequest coordinates(char** argv) {
    const auto pid = std::stoull(argv[4]); check(pid > 0 && pid <= UINT32_MAX, "Invalid fixture native PID");
    const auto process = processIdentity(std::uint32_t(pid));
    check(process && process->started == std::stoull(argv[5]) && process->account == argv[6], "Fixture peer identity changed");
    return {argv[2], argv[3], *process};
}
void stageBroker(const fs::path& source, const ElevationRequest& request) {
    check(administrator(), "Disposable broker staging requires existing administrator authorization");
    const auto stage = privilegedUninstallStage(request.nonce); makePrivateDirectory(stage);
#ifdef _WIN32
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;0x1200a9;;;BU)", SDDL_REVISION_1, &descriptor, nullptr) != 0, "Fixture stage ACL unavailable");
    const bool secured = SetFileSecurityW(stage.c_str(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor) != 0;
    LocalFree(descriptor); check(secured, "Fixture stage ACL failed");
    const auto image = stage / "mark-six-maintenance.exe";
#else
    check(chmod(stage.c_str(), 0711) == 0, "Cannot expose readonly fixture stage");
    const auto image = stage / "mark-six-maintenance";
#endif
    fs::copy_file(source, image);
#ifdef _WIN32
    descriptor = nullptr;
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)", SDDL_REVISION_1, &descriptor, nullptr) != 0, "Fixture image ACL unavailable");
    const bool imageSecured = SetFileSecurityW(image.c_str(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor) != 0;
    LocalFree(descriptor); check(imageSecured, "Fixture image ACL failed");
#else
    check(chmod(image.c_str(), 0755) == 0, "Cannot execute protected fixture image");
#endif
    check(protectedUninstallStage(stage) && sha256File(image) == sha256File(source), "Fixture protected image differs");
}
int agent(const fs::path& self) {
    const auto state = decode(readBoundedFile(self.parent_path() / "request.bin"));
    validateContainedPath(state.data, runnerTemp()); validateContainedPath(state.legacy, runnerTemp());
    const auto process = *processIdentity(currentProcessId());
    auto server = std::make_unique<PrivateServer>(ChannelPolicy{{}, true});
    const ElevationRequest request{server->endpoint(), state.nonce, process};
    const bool retry = entryExists(privilegedUninstallStage(state.nonce));
    auto launch = std::async(std::launch::async, [&] {
        std::vector<std::string> args{retry ? "--fixture-broker" : "--fixture-bootstrap", request.endpoint, request.nonce,
            std::to_string(request.agent.pid), std::to_string(request.agent.started), request.agent.account};
#ifdef _WIN32
        check(administrator(), "Windows fixture must already be elevated; no UAC automation is performed");
        const auto result = fixtureCommand(retry ? privilegedUninstallStage(state.nonce) / "mark-six-maintenance.exe" : self, args, std::chrono::minutes(5));
#else
        args.insert(args.begin(), pathUtf8(retry ? privilegedUninstallStage(state.nonce) / "mark-six-maintenance" : self));
        args.insert(args.begin(), "RUNNER_TEMP=" + pathUtf8(runnerTemp()));
        args.insert(args.begin(), "GITHUB_ACTIONS=true"); args.insert(args.begin(), "/usr/bin/env");
        const auto result = runNativeCommand("/usr/bin/sudo", args, std::chrono::minutes(5));
#endif
        if (result.status && !result.output.empty()) std::cerr << "Native fixture broker: " << result.output << '\n';
        return result.status;
    });
    auto channel = server->accept(std::chrono::minutes(2)); channel->setTimeout(std::chrono::minutes(5));
    expect(channel->receive(), "M6BROKERHELLO1", state.nonce);
    UninstallScope scope{{{currentAccount(), state.data, LocationKind::SavedData}, {currentAccount(), state.app, LocationKind::Application}}};
    Encoder requestBytes; requestBytes.text("M6FULLSCOPE1"); requestBytes.text(state.nonce); requestBytes.text(encodeUninstallScope(scope)); channel->send(requestBytes.bytes);
    auto result = receiveNativeReport(*channel, state.nonce);
    check(result.text.find(quotedReportField(pathUtf8(state.legacy))) != std::string::npos && result.text.find(currentAccount()) != std::string::npos,
        "Native review lost the preserved path or original OS account");
    if (state.mode == 2) check(result.kind == "outcome" && result.cancelled && !result.incomplete &&
        result.text.find("before deletion") != std::string::npos, "Untouched blocked attempt did not retire safely:\n" + result.text);
    else {
        check(result.kind == "review", "Broker preflight did not reach review:\n" + result.text);
        if (state.mode == 3) {
            const auto archive = state.data / "statistics/results.sqlite3";
            fs::rename(archive, state.data / "review-change-original.fixture");
            durableWrite(archive, "Foreign replacement after final review; must not be deleted");
        }
        Encoder approval; approval.text("M6FULLAPPROVAL1"); approval.text(state.nonce); approval.number(state.mode == 0 || state.mode == 3); approval.text(result.approvalDigest); channel->send(approval.bytes);
        result = receiveNativeReport(*channel, state.nonce);
        if (state.mode == 3) check(result.cancelled && !result.incomplete && result.text.find("changed during review") != std::string::npos,
            "A changed reviewed target was not rejected before deletion:\n" + result.text);
    }
    check(result.kind == "outcome" && result.text.find(quotedReportField(pathUtf8(state.legacy))) != std::string::npos,
        "Native result lost its preserved legacy path");
    durableWrite(self.parent_path() / "remaining.txt", result.text);
    if (result.incomplete) { channel.reset(); server.reset(); (void)launch.get(); return 10; }
    server.reset();
#ifdef _WIN32
    channel->send(control("M6RETIREUSER1", state.nonce));
    const auto finalBytes = channel->receive(); Decoder final(finalBytes);
    check(final.text() == "M6FINALIZED1" && final.text() == state.nonce, "Native finalization nonce changed");
    FullCleanupFinalization completion{std::uintptr_t(final.number()), final.path(), result.cancelled}; final.end();
    check(completion.waitHandle && completion.protectedStage == privilegedUninstallStage(state.nonce), "Native finalization stage changed");
    finishUserHelper(self, true, result.text, {}, state.data, completion);
#else
    const auto retired = retireUserHelper(self);
    try {
        channel->send(control("M6RETIREUSER1", state.nonce)); const auto finalBytes = channel->receive(); Decoder final(finalBytes);
        check(final.text() == "M6FINALIZED1" && final.text() == state.nonce && !final.number() && final.path().empty(), "Native finalization state changed"); final.end();
    } catch (...) { restoreUserHelper(self, retired); throw; }
#endif
    (void)launch.get(); return 0;
}
#ifdef _WIN32
std::optional<ProcessIdentity> resultProcess(const ProcessIdentity& parent) {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (snapshot == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry); std::optional<ProcessIdentity> found;
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ParentProcessID != parent.pid || _wcsicmp(entry.szExeFile, L"powershell.exe")) continue;
        const auto candidate = processIdentity(entry.th32ProcessID); if (candidate && candidate->started >= parent.started) { found = candidate; break; }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot); return found;
}
void closeResult(const ProcessIdentity& parent, const fs::path& legacy, bool cancelled = false) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto child = resultProcess(parent);
        if (child && sameProcessAlive(*child)) {
            struct Search { DWORD pid; HWND window = nullptr; } search{child->pid};
            EnumWindows([](HWND window, LPARAM raw) -> BOOL { auto& context = *reinterpret_cast<Search*>(raw); DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
                if (pid == context.pid && IsWindowVisible(window)) { context.window = window; return FALSE; } return TRUE;
            }, reinterpret_cast<LPARAM>(&search));
            if (search.window) {
                std::wstring text;
                EnumChildWindows(search.window, [](HWND item, LPARAM raw) -> BOOL {
                    std::array<wchar_t, 128> name{}; GetClassNameW(item, name.data(), int(name.size()));
                    if (!std::wstring_view(name.data()).starts_with(L"WindowsForms10.EDIT")) return TRUE;
                    DWORD_PTR size = 0;
                    if (!SendMessageTimeoutW(item, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &size) || !size || size > 1024 * 1024) return TRUE;
                    std::wstring value(std::size_t(size) + 1, L'\0'); DWORD_PTR copied = 0;
                    if (!SendMessageTimeoutW(item, WM_GETTEXT, WPARAM(size + 1), reinterpret_cast<LPARAM>(value.data()), SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &copied)) return TRUE;
                    value.resize(std::size_t(copied));
                    *reinterpret_cast<std::wstring*>(raw) = std::move(value); return FALSE;
                }, reinterpret_cast<LPARAM>(&text));
                const auto stage = parent.executable.parent_path();
                std::string recovery;
                if (entryExists(stage / "request.bin")) recovery = diagnostic(privilegedUninstallStage(decode(readBoundedFile(stage / "request.bin")).nonce));
                if (sameProcessAlive(*child)) PostMessageW(search.window, WM_CLOSE, 0, 0);
                check(text.starts_with(cancelled ? L"Uninstall cancelled" : L"Uninstall completed") &&
                    text.find(L"Temporary helpers and operation state have been removed.") != std::wstring::npos &&
                    text.find(pathFromUtf8(quotedReportField(pathUtf8(legacy))).native()) != std::wstring::npos,
                    "Native finalization did not verify completion:\n" + pathUtf8(fs::path(text.substr(0, 12000))) + "\n" + diagnostic(stage) + recovery);
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("Native full-uninstall result did not appear");
}
#endif
}
int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        int nativeCount = 0; const auto native = CommandLineToArgvW(GetCommandLineW(), &nativeCount);
        check(native != nullptr, "Native fixture command line unavailable");
        std::vector<std::string> strings; for (int i = 0; i < nativeCount; ++i) strings.push_back(pathUtf8(fs::path(native[i])));
        LocalFree(native); std::vector<char*> values; for (auto& value : strings) values.push_back(value.data()); values.push_back(nullptr);
        argc = nativeCount; argv = values.data();
#endif
#ifdef __APPLE__
        if (argc == 2 && std::string_view(argv[1]) == "--list-registered-applications") { std::cout << nativeApplicationRegistrations(); return 0; }
        if (argc == 3 && std::string_view(argv[1]) == "--verify-unregistered-application") return applicationRegistered(normalizePlatformPath(pathFromUtf8(argv[2]))) ? 1 : 0;
#endif
        const auto temporary = runnerTemp(); const auto self = *processIdentity(currentProcessId());
        if (argc == 7 && std::string_view(argv[1]) == "--fixture-broker") return runUninstallBroker(coordinates(argv), fixtureCatalog);
        if (argc == 7 && std::string_view(argv[1]) == "--fixture-bootstrap") {
            const auto request = coordinates(argv); stageBroker(self.executable, request);
#ifdef _WIN32
            const auto image = privilegedUninstallStage(request.nonce) / "mark-six-maintenance.exe";
#else
            const auto image = privilegedUninstallStage(request.nonce) / "mark-six-maintenance";
#endif
            const auto result = fixtureCommand(image, {"--fixture-broker", request.endpoint, request.nonce, std::to_string(request.agent.pid), std::to_string(request.agent.started), request.agent.account}, std::chrono::minutes(5));
            if (!result.output.empty()) std::cerr << result.output << '\n'; return result.status;
        }
        if (argc == 1 && pathUtf8(self.executable.parent_path().filename()).starts_with("marksix-maintenance-")) return agent(self.executable);
        check(argc == 4, "Expected installed application, isolated data and legacy fixture paths");
        State state{normalizePlatformPath(pathFromUtf8(argv[1])), normalizePlatformPath(pathFromUtf8(argv[2])), normalizePlatformPath(pathFromUtf8(argv[3])), randomToken()};
        validateContainedPath(state.data, temporary); validateContainedPath(state.legacy, temporary);
#ifdef _WIN32
        validateContainedPath(state.app, temporary);
#else
        check(state.app == "/Applications/Mark Six Studio.app", "Refusing an unexpected installed application");
#endif
        check(hasProductIdentity(state.app), "CI product identity is unavailable");
        const auto archive = state.data / "statistics/results.sqlite3";
        check(entryExists(archive) && legacyRecordingName(state.legacy), "Isolated saved-data and legacy fixtures are required");
        const auto legacyHash = sha256File(state.legacy);
        const auto archiveHash = sha256File(archive);
        for (const unsigned mode : {1u, 2u, 3u}) {
            auto cancelled = state; cancelled.nonce = randomToken(); cancelled.mode = mode;
            const auto userStage = temporary / ("marksix-maintenance-" + randomToken()); makePrivateDirectory(userStage);
#ifdef _WIN32
            const auto userImage = userStage / "mark-six-maintenance.exe";
#else
            const auto userImage = userStage / "mark-six-maintenance";
#endif
            fs::copy_file(self.executable, userImage); durableWrite(userStage / "request.bin", encode(cancelled));
#ifndef _WIN32
            check(chmod(userImage.c_str(), 0700) == 0, "Cannot execute cancellation fixture");
#endif
            auto writer = mode == 2 ? FileLease::acquire(sessionLeasePath(state.data), FileLease::Mode::Shared) : nullptr;
            check(mode != 2 || bool(writer), "Cannot reserve a live workspace fixture");
            const auto result = fixtureCommand(userImage, {}, std::chrono::minutes(5)); writer.reset();
            check(result.status == 0, "Cancellation/preflight retirement failed:\n" + result.output + diagnostic(userStage));
#ifdef _WIN32
            check(bool(result.process), "Native cancellation parent identity missing"); closeResult(*result.process, state.legacy, true);
#endif
            check(!entryExists(userStage) && !entryExists(privilegedUninstallStage(cancelled.nonce)) && !readMachineGate(), "Untouched cancellation left temporary staging or a global startup gate");
            if (mode == 3) {
                check(readBoundedFile(archive) == "Foreign replacement after final review; must not be deleted", "Changed foreign replacement was deleted under old consent");
                durableRemove(archive); fs::rename(state.data / "review-change-original.fixture", archive);
            }
            check(hasProductIdentity(state.app) && sha256File(archive) == archiveHash && sha256File(state.legacy) == legacyHash,
                "Untouched cancellation/preflight changed installed code, saved data or legacy bytes");
        }
        const auto stage = temporary / ("marksix-maintenance-" + randomToken()); makePrivateDirectory(stage);
#ifdef _WIN32
        const auto image = stage / "mark-six-maintenance.exe";
#else
        const auto image = stage / "mark-six-maintenance";
#endif
        fs::copy_file(self.executable, image); durableWrite(stage / "request.bin", encode(state));
#ifdef _WIN32
        const auto locked = CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        check(locked != INVALID_HANDLE_VALUE, "Cannot reserve locked-data fixture");
#else
        check(chmod(image.c_str(), 0700) == 0 && chflags(archive.c_str(), UF_IMMUTABLE) == 0, "Cannot reserve immutable-data fixture");
#endif
        const auto first = fixtureCommand(image, {}, std::chrono::minutes(5));
#ifdef _WIN32
        CloseHandle(locked);
#else
        check(chflags(archive.c_str(), 0) == 0, "Cannot release immutable-data fixture");
#endif
        check(first.status == 10 && entryExists(archive), "Locked current-app file did not produce a partial native result:\n" + first.output + diagnostic(stage));
#ifdef _WIN32
        const auto application = state.app / "mark-six-studio.exe";
#else
        const auto application = state.app / "Contents/MacOS/mark-six-studio";
#endif
        check(!entryExists(application) && entryExists(privilegedUninstallStage(state.nonce) / "ledger.bin"), "Partial removal did not retain standalone recovery after the main executable disappeared");
        const auto second = fixtureCommand(image, {}, std::chrono::minutes(5));
        check(second.status == 0, "Standalone native retry failed:\n" + second.output + diagnostic(stage));
#ifdef _WIN32
        check(bool(second.process), "Native result parent identity missing"); closeResult(*second.process, state.legacy);
#endif
        check(!entryExists(stage) && !entryExists(privilegedUninstallStage(state.nonce)) && !readMachineGate(), "Native completion left helper staging or a system gate");
        check(!entryExists(archive) && sha256File(state.legacy) == legacyHash, "Native retry changed legacy bytes or retained approved records");
        std::cout << "PASS native cancellation, changed-review rejection, untouched blocked-attempt retirement, all-user uninstall, locked-file partial outcome, standalone retry after payload removal, preserved full-path report and final helper/gate absence\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
