#include "full_uninstall.hpp"
#include "helper_lifecycle.hpp"
#include "native_dialog.hpp"
#include "durable_files.hpp"
#include "payload_authority.hpp"
#include "wire_codec.hpp"
#include <future>
#include <thread>
#include <algorithm>

namespace marksix::maintenance {
namespace {
std::string message(const std::string& kind, const std::string& nonce) { Encoder out; out.text(kind); out.text(nonce); return out.bytes; }
void expect(const std::string& bytes, const std::string& kind, const std::string& nonce) {
    Decoder in(bytes); if (in.text() != kind || in.text() != nonce) throw std::runtime_error("Uninstall lifecycle or nonce mismatch"); in.end();
}
bool idle() { return runningApplications().empty() && nativeInstallersIdle(); }
void retainCoverage(UninstallDiscovery& discovery, const std::vector<UserStorage>& saved) {
    discovery.users = [saved](bool all, std::vector<InventoryIssue>& issues) {
        auto native = discoverUsers(all, issues);
        for (const auto& previous : saved) {
            const auto current = std::find_if(native.begin(), native.end(), [&](const auto& user) { return user.account == previous.account; });
            if (current == native.end()) {
                auto unavailable = previous; unavailable.locationsResolved = false; native.push_back(std::move(unavailable));
                issues.push_back({previous.home, previous.account, "Previously reviewed OS account is unavailable; original cleanup coverage remains unresolved", true});
                continue;
            }
            if (current->home != previous.home || current->dataRoot != previous.dataRoot || current->cacheRoot != previous.cacheRoot || current->downloads != previous.downloads)
                throw std::runtime_error("Native account locations changed; protected recovery cannot silently remap them");
            for (const auto& root : previous.additionalDataRoots) if (std::find(current->additionalDataRoots.begin(), current->additionalDataRoots.end(), root) == current->additionalDataRoots.end()) current->additionalDataRoots.push_back(root);
            for (const auto& root : previous.additionalDownloadRoots) if (std::find(current->additionalDownloadRoots.begin(), current->additionalDownloadRoots.end(), root) == current->additionalDownloadRoots.end()) current->additionalDownloadRoots.push_back(root);
        }
        return native;
    };
}
template<class T> T waitNative(std::future<T>& pending, NativeProgress& progress, const std::string& text) {
    while (pending.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) progress.update(text, 0, 0);
    return pending.get();
}
}
int runUninstallBroker(const ElevationRequest& request, const std::function<ReleaseCatalog()>& releases) {
    const auto self = processIdentity(currentProcessId());
    if (!self || !administrator() || self->uncertain || self->executable.parent_path() != privilegedUninstallStage(request.nonce) ||
        !protectedUninstallStage(self->executable.parent_path()) || !protectedProductFile(self->executable))
        throw std::runtime_error("Uninstall broker must run from authenticated administrator staging");
    const auto stage = self->executable.parent_path();
    if (request.agent.executable.filename() != self->executable.filename() || legacyRecordingName(request.agent.executable) ||
        sha256File(request.agent.executable) != sha256File(self->executable))
        throw std::runtime_error("Original-user review agent is not the same independently trusted native helper image");
    PrivateChannel channel(request.endpoint, request.agent, {request.agent.account, false});
    channel.setTimeout(std::chrono::minutes(30)); channel.send(message("M6BROKERHELLO1", request.nonce));
    const auto bytes = channel.receive(); Decoder input(bytes);
    if (input.text() != "M6FULLSCOPE1" || input.text() != request.nonce) throw std::runtime_error("Uninstall scope nonce mismatch");
    const auto scope = decodeUninstallScope(input.text()); input.end();
    auto operationLease = FileLease::acquire(stage / ".operation.lock", FileLease::Mode::Exclusive);
    if (!operationLease) throw std::runtime_error("Another administrator helper owns this uninstall operation");
    const MachineGate gate{request.nonce, stage, *self};
    std::string report = "Preserved legacy files: inspection did not complete. No empty-list claim is available.\n";
    bool ownsGate = false, executionMayHaveStarted = entryExists(stage / "ledger.bin"), outcomeSent = false;
    const auto retire = [&] {
        expect(channel.receive(), "M6RETIREUSER1", request.nonce);
        operationLease.reset();
        const auto finalizer = finalizeUninstallBroker(self->executable, gate, request.agent, true);
        Encoder final; final.text("M6FINALIZED1"); final.text(request.nonce); final.number(finalizer.waitHandle); final.path(finalizer.protectedStage);
        channel.send(final.bytes);
    };
    try {
        const auto oldGate = readMachineGate();
        if (oldGate) {
            if (oldGate->nonce != gate.nonce || oldGate->stage != stage || sameProcessAlive(oldGate->executor))
                throw std::runtime_error("A different or still-running system cleanup needs completion");
            updateMachineGateExecutor(gate);
        } else publishMachineGate(gate);
        ownsGate = true;
        const auto installerBarrier = reserveInstallerLifecycle();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!idle()) {
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Close Mark Six Studio in every account and finish native installers before retrying cleanup. This attempt has not started deletion.");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        UninstallDiscovery discovery; const auto catalog = releases(); discovery.catalog = [catalog] { return catalog; };
        std::optional<UninstallJournal> previous;
        if (entryExists(stage / "ledger.bin")) {
            previous = UninstallJournal::open(stage);
            executionMayHaveStarted = previous->executionStarted();
            if (previous->nonce() != gate.nonce || previous->inspection().originalAccount != request.agent.account || encodeUninstallScope(previous->scope()) != encodeUninstallScope(scope))
                throw std::runtime_error("Protected uninstall recovery does not match this original account and operation scope");
            retainCoverage(discovery, previous->inspection().users);
            discovery.retainedLegacy = previous->inspection().inventory.preservedLegacy;
            discovery.retainedApplications = previous->inspection().applications;
        }
        auto inspection = inspectUninstall(scope, request.agent.account, discovery);
        inspectNativeRegistrations(inspection);
        report = uninstallReview(inspection);
        for (const auto& issue : inspection.inventory.issues) if (issue.reason.find("still holds this marker") != std::string::npos)
            throw std::runtime_error("A workspace or maintenance writer holds an exclusive coordination lease. Close it before retrying.\n\n" + report);
        auto journal = previous ? std::move(*previous) : UninstallJournal::create(stage, request.nonce, scope, inspection);
        executionMayHaveStarted = journal.executionStarted();
        if (journal.inspection().originalAccount != request.agent.account) throw std::runtime_error("Protected uninstall recovery belongs to another original account");
        if (journal.scope().additional != scope.additional) {
            // Scope encoding, not a client-asserted target list, is the durable
            // binding for this operation. Retry does not silently narrow it.
            if (encodeUninstallScope(journal.scope()) != encodeUninstallScope(scope)) throw std::runtime_error("Retry scope differs from the protected original request");
        }
        journal.refresh(inspection); report = uninstallRecoveryReview(journal);
        report += "\nTemporary cleanup components (removed on successful finalization):\nOriginal-user helper: " + quotedReportField(pathUtf8(request.agent.executable)) +
            "\nProtected administrator helper: " + quotedReportField(pathUtf8(self->executable)) + "\nProtected recovery ledger: " + quotedReportField(pathUtf8(stage / "ledger.bin")) +
            "\nThe protected system startup gate and these helpers' request/report files are also removed.\n";
        sendNativeReport(channel, request.nonce, {"review", journal.reviewDigest(), report, !journal.inspection().inventory.complete(), false});
        const auto approvalBytes = channel.receive(); Decoder approval(approvalBytes);
        if (approval.text() != "M6FULLAPPROVAL1" || approval.text() != request.nonce) throw std::runtime_error("Uninstall approval nonce mismatch");
        const auto approved = approval.number(); const auto digest = approval.text(); approval.end();
        if (approved > 1) throw std::runtime_error("Invalid uninstall approval");
        NativeReport result{"outcome", {}, {}, false, approved == 0};
        if (!approved) {
            result.incomplete = journal.executionStarted();
            result.text = std::string(result.incomplete ? "Retry cancelled. No additional files were removed. Earlier removal and remaining work stay in the protected recovery journal.\n\n" :
                "Uninstall cancelled. No inventoried current-app files were deleted.\n\n") + preservedLegacyReport(journal.inspection().inventory);
        }
        else {
            // Refresh all profile/target identities after human review. The
            // journal itself rechecks every exact object at removal time.
            auto fresh = inspectUninstall(scope, request.agent.account, discovery); inspectNativeRegistrations(fresh);
            report = uninstallReview(fresh);
            if (uninstallDigest(fresh) != uninstallDigest(inspection)) throw std::runtime_error("Files or account coverage changed during review. Retry for an updated inventory; no deletion started.");
            UninstallExecutionEnvironment environment; environment.idle = idle; environment.removeRegistrations = removeNativeRegistrations;
            executionMayHaveStarted = true;
            const auto outcome = executeUninstall(journal, digest, !journal.inspection().inventory.complete(), environment);
            result.text = outcome.report; result.incomplete = !outcome.complete;
            const auto previousPreserved = preservedLegacyReport(journal.inspection().inventory);
            // Verify known namespaces again after removal. A new account,
            // file or recording requires a fresh review, never silent deletion
            // under a previous approval. Original custom roots survive loss
            // of their discovery manifest through the protected snapshot.
            retainCoverage(discovery, journal.inspection().users);
            discovery.retainedLegacy = journal.inspection().inventory.preservedLegacy;
            discovery.retainedApplications = journal.inspection().applications;
            auto remaining = inspectUninstall(scope, request.agent.account, discovery);
            journal.refresh(remaining);
            const auto preservedAt = result.text.find(previousPreserved);
            if (preservedAt != std::string::npos) result.text.replace(preservedAt, previousPreserved.size(), preservedLegacyReport(journal.inspection().inventory));
            if (!journal.filesRemoved() || !journal.inspection().inventory.complete()) {
                result.incomplete = true; result.text += "\nFinal inspection requires review/retry:\n" + uninstallRecoveryReview(journal);
            }
            if (!idle()) throw std::runtime_error("Another application or native installer started during final verification; retry is required");
        }
        report = result.text;
        if (result.incomplete) {
            result.text += "\nProtected standalone recovery: " + pathUtf8(self->executable) + "\n";
            durableWrite(stage / "remaining.txt", result.text); sendNativeReport(channel, request.nonce, result); return 1;
        }
        sendNativeReport(channel, request.nonce, result);
        outcomeSent = true; retire(); return 0;
    } catch (const std::exception& error) {
        if (ownsGate && !executionMayHaveStarted && !outcomeSent) {
            report = "Uninstall stopped before deletion. No inventoried current-app files were deleted. Temporary cleanup components will be retired.\n\n" +
                std::string(error.what()) + "\n\n" + report;
            durableWrite(stage / "remaining.txt", report);
            sendNativeReport(channel, request.nonce, {"outcome", {}, report, false, true});
            retire(); return 0;
        }
        report = "Cleanup blocked or partially completed.\n\n" + std::string(error.what()) + "\n\n" + report +
            "\nProtected retry helper: " + pathUtf8(self->executable) + "\n";
        durableWrite(stage / "remaining.txt", report);
        sendNativeReport(channel, request.nonce, {"outcome", {}, report, true, false}); return 1;
    }
}
int runUninstallAgent(const fs::path& executable, const UninstallScope& scope, const fs::path& application,
    const fs::path& dataRoot, const std::string& nonce, bool retry, std::string& observedReport,
    const UninstallAuthorizationHooks& authorizationHooks) {
    observedReport = "Preserved legacy files: administrator inspection has not completed. No empty-list or complete-cleanup claim is available.\n";
    const auto previousReport = executable.parent_path() / "remaining.txt";
    if (retry && entryExists(previousReport)) observedReport += "\nPrevious result (fresh administrator inspection pending):\n" + readBoundedFile(previousReport, 64 * 1024 * 1024);
    if (!authorizationHooks.launch) throw std::runtime_error("Native administrator authorization is unavailable");
    const auto self = processIdentity(currentProcessId()); if (!self || self->uncertain) throw std::runtime_error("Cannot resolve original-user uninstall agent");
    auto server = std::make_unique<PrivateServer>(ChannelPolicy{{}, true});
    ElevationRequest request{server->endpoint(), nonce, *self};
    auto authorization = std::async(std::launch::async, [&] { return authorizationHooks.launch(request, application, retry); });
    std::unique_ptr<PrivateChannel> channel;
    {
        auto progress = authorizationHooks.waiting ? nullptr : std::make_unique<NativeProgress>();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        while (!channel) {
            const std::string waiting = "Waiting for native administrator authorization. No current-app files have been deleted. You can cancel the OS authorization dialog.";
            if (authorizationHooks.waiting) authorizationHooks.waiting(waiting); else progress->update(waiting, 0, 0);
            if (authorization.valid() && authorization.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                const auto status = authorization.get();
                if (status == 1223) {
                    const auto retained = retry && entryExists(previousReport) ? readBoundedFile(previousReport, 64 * 1024 * 1024) :
                        std::string("Preserved legacy files: privileged inspection did not run; no empty-list claim is available.");
                    observedReport = retry ? "Retry cancelled at native administrator authorization. No additional files were deleted. The protected operation and this original-user retry helper remain available.\n\nPrevious result:\n" + retained :
                        "Uninstall cancelled at native administrator authorization. No inventoried files were deleted.\n\nPreserved legacy files: privileged inspection did not run; no empty-list claim is available.";
                    server.reset();
                    if (authorizationHooks.finishCancelled) authorizationHooks.finishCancelled(executable, !retry, observedReport, dataRoot);
                    else finishUserHelper(executable, !retry, observedReport, {}, dataRoot);
                    return 0;
                }
                if (status != 0) throw std::runtime_error("The protected administrator bootstrap could not start. No deletion was approved.");
#ifdef _WIN32
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
#endif
            }
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Administrator authorization timed out before cleanup approval");
            try { channel = server->accept(std::chrono::milliseconds(100)); }
            catch (const std::exception& error) { if (std::string(error.what()) != "Maintenance connection timed out") throw; }
        }
    }
    if (channel->peer().executable.parent_path() != privilegedUninstallStage(nonce) || !protectedProductFile(channel->peer().executable))
        throw std::runtime_error("Administrator peer does not match protected native staging");
    channel->setTimeout(std::chrono::minutes(30)); expect(channel->receive(), "M6BROKERHELLO1", nonce);
    Encoder requestBytes; requestBytes.text("M6FULLSCOPE1"); requestBytes.text(nonce); requestBytes.text(encodeUninstallScope(scope)); channel->send(requestBytes.bytes);
    NativeReport result;
    {
        NativeProgress progress; auto review = std::async(std::launch::async, [&] { return receiveNativeReport(*channel, nonce); });
        result = waitNative(review, progress, "Inspecting all native accounts, current-app files and preserved legacy recordings. Final deletion consent follows inspection.");
        observedReport = result.text;
    }
    if (result.kind == "review") {
        const auto approved = nativeReview("Permanently uninstall Mark Six Studio", result.text,
            result.incomplete ? "Remove reviewed files (partial)" : "Permanently uninstall", "Cancel");
        Encoder approval; approval.text("M6FULLAPPROVAL1"); approval.text(nonce); approval.number(approved); approval.text(result.approvalDigest); channel->send(approval.bytes);
        NativeProgress progress; auto outcome = std::async(std::launch::async, [&] { return receiveNativeReport(*channel, nonce); });
        result = waitNative(outcome, progress, approved ? "Removing the approved current-app files. Legacy recordings remain protected." : "Cancelling uninstall and cleaning its temporary helpers.");
        observedReport = result.text;
    }
    if (result.kind != "outcome") throw std::runtime_error("Uninstall broker returned an invalid final outcome");
    if (result.incomplete) { server.reset(); finishUserHelper(executable, false, result.text, {}, dataRoot); return 1; }
#ifdef _WIN32
    server.reset(); channel->send(message("M6RETIREUSER1", nonce));
    const auto finalBytes = channel->receive(); Decoder final(finalBytes);
    if (final.text() != "M6FINALIZED1" || final.text() != nonce) throw std::runtime_error("Native finalizer nonce mismatch");
    FullCleanupFinalization full; full.waitHandle = std::uintptr_t(final.number()); full.protectedStage = final.path(); final.end();
    full.cancelled = result.cancelled;
    if (!full.waitHandle || full.protectedStage != privilegedUninstallStage(nonce)) throw std::runtime_error("Native finalizer identity mismatch");
    finishUserHelper(executable, true, result.text, {}, dataRoot, full); return 0;
#else
    server.reset(); auto retired = retireUserHelper(executable);
    try {
        channel->send(message("M6RETIREUSER1", nonce));
        const auto finalBytes = channel->receive(); Decoder final(finalBytes);
        if (final.text() != "M6FINALIZED1" || final.text() != nonce || final.number() != 0 || !final.path().empty()) throw std::runtime_error("Native finalizer state mismatch"); final.end();
    } catch (...) { restoreUserHelper(executable, retired); throw; }
    const auto pending = result.text.find("Final helper cleanup is still pending.");
    if (pending != std::string::npos) result.text.replace(pending, std::string("Final helper cleanup is still pending.").size(), "Temporary helper cleanup is verified.");
    if (!result.cancelled) result.text = "Uninstall completed with the listed legacy files preserved. Temporary helpers and operation state have been removed.\n\n" + result.text;
    nativeReview("Mark Six Studio uninstall result", result.text);
    return 0;
#endif
}
}
