#include "private_channel.hpp"
#include "wire_codec.hpp"
#include "durable_files.hpp"
#include "user_cleanup.hpp"
#include "helper_lifecycle.hpp"
#include "native_dialog.hpp"
#include "download_inventory.hpp"
#include "payload_authority.hpp"
#include "full_uninstall.hpp"
#include "machine_gate.hpp"
#include "installer_lease.hpp"
#include <chrono>
#include <charconv>
#include <iostream>
#include <thread>

using namespace marksix::maintenance;
namespace {
std::uint64_t integer(const char* text) {
    const std::string value(text); std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !result) throw std::runtime_error("Invalid maintenance process identity");
    return result;
}
struct Request { UserCleanupRequest cleanup; fs::path application; std::string account; UninstallScope scope; std::string fullNonce; };
std::string encode(const Request& request) {
    Encoder out; out.text("M6USERREQUEST1"); out.number(std::uint64_t(request.cleanup.operation));
    out.path(request.cleanup.dataRoot); out.number(request.cleanup.recovery); out.path(request.application); out.text(request.account);
    if (request.cleanup.operation == Operation::Uninstall) { out.text(encodeUninstallScope(request.scope)); out.text(request.fullNonce); }
    return out.bytes;
}
Request decode(std::string_view bytes) {
    Decoder in(bytes);
    if (in.text() != "M6USERREQUEST1") throw std::runtime_error("Unsupported maintenance request");
    const auto operation = in.number(); if (operation > 2) throw std::runtime_error("Unsupported maintenance operation");
    Request request; request.cleanup.operation = Operation(operation); request.cleanup.dataRoot = in.path();
    const auto recovery = in.number(); if (recovery > 1) throw std::runtime_error("Invalid recovery request");
    request.cleanup.recovery = recovery != 0; request.application = in.path(); request.account = in.text();
    if (request.cleanup.operation == Operation::Uninstall) { request.scope = decodeUninstallScope(in.text()); request.fullNonce = in.text(); (void)privilegedUninstallStage(request.fullNonce); }
    in.end();
    if (request.account != currentAccount()) throw std::runtime_error("Maintenance request belongs to another OS account");
    return request;
}

}
int main(int argc, char** argv) {
#ifdef _WIN32
    if (argc == 2 && std::string_view(argv[1]) == "--secure-install-registration") {
        try { secureNativeInstallationRegistration(); return 0; }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
#endif
#ifdef __APPLE__
    if (argc == 2 && std::string_view(argv[1]) == "--list-registered-applications") {
        try { std::cout << nativeApplicationRegistrations(); return 0; }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
    }
    if (argc == 3 && std::string_view(argv[1]) == "--verify-unregistered-application") {
        try { return applicationRegistered(normalizePlatformPath(fs::path(argv[2]))) ? 1 : 0; }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
    }
#endif
    if (argc == 7 && (std::string_view(argv[1]) == "--uninstall-broker" || std::string_view(argv[1]) == "--uninstall-bootstrap")) {
        try {
            const auto pid = integer(argv[4]); if (pid > UINT32_MAX) throw std::runtime_error("Invalid original-user PID");
            const auto agent = processIdentity(std::uint32_t(pid));
            if (!agent || agent->uncertain || agent->started != integer(argv[5]) || agent->account != argv[6]) throw std::runtime_error("Original-user native identity changed");
            const ElevationRequest request{argv[2], argv[3], *agent};
            if (std::string_view(argv[1]) == "--uninstall-bootstrap") return bootstrapUninstallWindows(request);
            return runUninstallBroker(request);
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    if (argc == 2 && std::string_view(argv[1]) == "--verify-runtime") {
        std::cout << "M6NATIVEHELPER1\n"; return 0; // Read-only packaging/dependency check.
    }
    if (argc == 2 && std::string_view(argv[1]) == "--verify-installation-state") {
        const auto state = nativeInstallationState();
        std::cout << "Native completed installation: " << state.complete << "\nVersion: " << state.version << "\nProof: " << state.stamp << '\n';
        return state.complete ? 0 : 1; // Read-only disposable installer verification.
    }
    if (argc == 2 && std::string_view(argv[1]) == "--verify-release-catalog") {
        const auto catalog = fetchReleaseCatalog();
        std::cout << "Published product assets: " << catalog.assets.size() << "\nComplete catalogue: " << catalog.complete << '\n';
        for (const auto& limit : catalog.limits) std::cerr << limit << '\n';
        return catalog.complete ? 0 : 1; // Metadata only; no installer is downloaded or run.
    }
    if (argc == 3 && std::string_view(argv[1]) == "--verify-application-code") {
        const auto root = normalizePlatformPath(fs::path(argv[2]));
#ifdef __APPLE__
        const auto executable = root / "Contents/MacOS/mark-six-studio";
#else
        const auto executable = root / "mark-six-studio.exe";
#endif
        const auto valid = nativeProductCodeValid({root, executable, {}, {}});
        std::cout << "Native product code: " << valid << '\n'; return valid ? 0 : 1;
    }
    fs::path executable; Request request; bool stateReady = false; std::string operationReport;
    std::unique_ptr<NativeProgress> progress;
    try {
        const auto self = processIdentity(currentProcessId());
        if (!self || self->uncertain) throw std::runtime_error("Cannot resolve native helper identity");
        executable = self->executable; const auto stage = executable.parent_path();
        if (!pathUtf8(stage.filename()).starts_with("marksix-maintenance-")) throw std::runtime_error("Run maintenance from Mark Six Studio, or use its disclosed standalone retry helper");
        const auto state = stage / "request.bin";
        if (argc == 6 && std::string_view(argv[1]) == "--channel") {
            const std::string nonce(argv[3]);
            if (nonce.size() != 64 || nonce.find_first_not_of("0123456789abcdef") != std::string::npos) throw std::runtime_error("Invalid operation nonce");
            const auto pid = integer(argv[4]);
            if (pid > UINT32_MAX) throw std::runtime_error("Invalid initiating PID");
            const auto parent = processIdentity(std::uint32_t(pid));
            if (!parent || parent->started != integer(argv[5])) throw std::runtime_error("Initiating process changed");
            {
                PrivateChannel channel(argv[2], *parent);
                Encoder hello; hello.text("M6HELLO1"); hello.text(nonce); channel.send(hello.bytes);
                const auto bytes = channel.receive(); Decoder input(bytes);
                if (input.text() != nonce) throw std::runtime_error("Operation nonce mismatch");
                request = decode(input.text()); input.end();
                request.application = parent->executable;
                if (request.cleanup.operation == Operation::Uninstall) {
#ifdef _WIN32
                    const auto appRoot = request.application.parent_path();
#else
                    const auto appRoot = request.application.parent_path().parent_path().parent_path();
#endif
                    request.scope.additional.push_back({request.account, appRoot, LocationKind::Application});
                }
                durableWrite(state, encode(request)); stateReady = true;
                Encoder ready; ready.text("M6READY1"); ready.text(nonce); channel.send(ready.bytes);
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            while (sameProcessAlive(*parent)) {
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("The application has not exited. No cleanup started; close it before retrying");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else if (argc == 1) { request = decode(readBoundedFile(state)); stateReady = true; }
        else throw std::runtime_error("Invalid maintenance invocation");
        if (request.cleanup.operation == Operation::Uninstall) {
#ifdef _WIN32
            const auto applicationRoot = request.application.parent_path();
#else
            const auto applicationRoot = request.application.parent_path().parent_path().parent_path();
#endif
            const auto protectedStage = privilegedUninstallStage(request.fullNonce);
            // Protected root recovery can outlive the original application.
            // Discovery hints remain the original reviewed request on retry.
            return runUninstallAgent(executable, request.scope, applicationRoot, request.cleanup.dataRoot, request.fullNonce, entryExists(protectedStage), operationReport);
        }
        UserCleanupEnvironment environment;
        environment.progress = [&](const auto& message, auto completed, auto total) {
            if (!progress) progress = std::make_unique<NativeProgress>();
            progress->update(message, completed, total);
        };
        operationReport = executeUserCleanup(request.cleanup, request.account, operationReport, environment);
        progress.reset();
        finishUserHelper(executable, true, operationReport, request.application, request.cleanup.dataRoot);
        return 0;
    } catch (const std::exception& error) {
        progress.reset();
        try {
            if (stateReady) finishUserHelper(executable, false, std::string("Cleanup blocked or partially completed.\n\n") + error.what() + "\n\n" + operationReport, request.application, request.cleanup.dataRoot);
            else nativeReview("Maintenance could not start", error.what());
        } catch (const std::exception& finalError) { nativeReview("Maintenance needs attention", std::string(error.what()) + "\n\n" + finalError.what() + "\nHelper: " + pathUtf8(executable) + "\n\n" + operationReport); }
        return 1;
    }
}
