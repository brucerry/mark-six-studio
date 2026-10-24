#include "uninstall_journal.hpp"
#include "durable_files.hpp"
#include "wire_codec.hpp"
#include <algorithm>
#include <set>

namespace marksix::maintenance {
namespace {
void paths(Encoder& out, const std::vector<fs::path>& paths) { out.number(paths.size()); for (const auto& path : paths) out.path(path); }
std::vector<fs::path> paths(Decoder& in) {
    std::vector<fs::path> result; const auto count = in.count();
    for (std::size_t i = 0; i < count; ++i) result.push_back(in.path());
    return result;
}
void issues(Encoder& out, const std::vector<InventoryIssue>& issues) {
    out.number(issues.size()); for (const auto& issue : issues) { out.path(issue.path); out.text(issue.account); out.text(issue.reason); out.number(issue.unresolved); }
}
std::vector<InventoryIssue> issues(Decoder& in) {
    std::vector<InventoryIssue> result; const auto count = in.count();
    for (std::size_t i = 0; i < count; ++i) {
        auto path = in.path(); auto account = in.text(), reason = in.text(); const auto unresolved = in.number();
        if (unresolved > 1) throw std::runtime_error("Invalid uninstall inspection status");
        result.push_back({std::move(path), std::move(account), std::move(reason), unresolved != 0});
    }
    return result;
}
bool movedMatches(const Target& target, const fs::path& moved) {
    const auto actual = target.identity.linkTarget.empty() ? identifyFile(moved) : identifySymbolicLink(moved);
    return actual.volume == target.identity.volume && actual.file == target.identity.file && actual.bytes == target.identity.bytes &&
        actual.modified == target.identity.modified && actual.sha256 == target.identity.sha256 && actual.linkTarget == target.identity.linkTarget;
}
void verifyAbsentLocation(const Target& target) {
    auto parent = target.path.parent_path();
    while (!entryExists(parent) && parent != parent.root_path()) parent = parent.parent_path();
    const auto actual = identifyDirectory(parent);
    const auto chain = actual.ancestors + std::to_string(actual.volume) + ':' + std::to_string(actual.file) + '/';
    if (actual.volume != target.identity.volume || !target.identity.ancestors.starts_with(chain))
        throw std::runtime_error("The original filesystem or parent identity is unavailable; absence cannot be certified");
}
void validatePlan(const UninstallInspection& inspection, const fs::path& stage) {
    if (inspection.originalAccount.empty() || inspection.users.empty() || inspection.users.size() > 4096) throw std::runtime_error("Missing native uninstall account mapping");
    std::set<std::string> accounts;
    for (const auto& user : inspection.users) if (user.account.empty() || !user.home.is_absolute() || !accounts.insert(user.account).second)
        throw std::runtime_error("Invalid native profile mapping in uninstall recovery");
    if (!accounts.contains(inspection.originalAccount)) throw std::runtime_error("Uninstall initiator is absent from native profile mappings");
    std::set<fs::path> targets;
    for (const auto& directory : inspection.ownedDirectories) {
        for (const auto& user : inspection.users) validateBoundary(directory.path, user.home);
        if (directory.path == stage || directory.path == stage.parent_path()) throw std::runtime_error("Cleanup staging cannot be a directory target");
    }
    for (const auto& target : inspection.inventory.targets) {
        validateContainedPath(target.path, target.boundary);
        for (const auto& user : inspection.users) validateBoundary(target.boundary, user.home);
        if ((!target.account.empty() && !accounts.contains(target.account)) || !targets.insert(target.path).second || legacyRecordingName(target.path) || intentionallyPreserved(inspection.inventory, target.path))
            throw std::runtime_error("Unsafe duplicate or preserved uninstall target");
        const auto relative = stage.lexically_relative(target.boundary);
        if (stage == target.boundary || (!relative.empty() && !relative.is_absolute() && *relative.begin() != ".."))
            throw std::runtime_error("The protected operation ledger must be outside every removal boundary");
    }
}
}
UninstallJournal UninstallJournal::create(const fs::path& stage, const std::string& nonce, const UninstallScope& scope, const UninstallInspection& inspection) {
    if (nonce.size() != 64 || nonce.find_first_not_of("0123456789abcdef") != std::string::npos) throw std::runtime_error("Invalid uninstall operation identity");
    validateContainedPath(stage / "ledger.bin", stage); validatePlan(inspection, stage);
    if (entryExists(stage / "ledger.bin")) throw std::runtime_error("An uninstall operation already needs recovery");
    UninstallJournal journal; journal.stage_ = stage; journal.nonce_ = nonce; journal.scope_ = scope; journal.inspection_ = inspection; journal.save(); return journal;
}
void UninstallJournal::save() const {
    Encoder out; out.text("M6UNINSTALLJOURNAL3"); out.number(executionStarted_); out.text(nonce_); out.text(encodeUninstallScope(scope_)); out.text(inspection_.originalAccount);
    out.number(inspection_.users.size());
    for (const auto& user : inspection_.users) {
        out.text(user.account); out.path(user.home); out.path(user.dataRoot); out.path(user.cacheRoot); out.path(user.downloads); out.path(user.manifestPath);
        paths(out, user.additionalDataRoots); paths(out, user.additionalDownloadRoots); out.number(user.locationsResolved);
    }
    out.number(inspection_.applications.size());
    for (const auto& application : inspection_.applications) { out.path(application.root); out.path(application.executable); out.text(application.account); out.text(application.registration); }
    paths(out, inspection_.payloadDirectories);
    out.number(inspection_.ownedDirectories.size());
    for (const auto& directory : inspection_.ownedDirectories) { out.path(directory.path); out.text(directory.account); out.identity(directory.identity); }
    out.text(inspection_.nativeState); out.text(inspection_.nativeReview);
    out.number(inspection_.inventory.targets.size());
    for (const auto& target : inspection_.inventory.targets) {
        out.path(target.path); out.path(target.boundary); out.text(target.account); out.number(std::uint64_t(target.category)); out.text(target.evidence); out.identity(target.identity);
    }
    issues(out, inspection_.inventory.issues);
    out.number(inspection_.inventory.preservedLegacy.size());
    for (const auto& file : inspection_.inventory.preservedLegacy) { out.path(file.path); out.text(file.account); out.text(file.reason); }
    issues(out, failures_); out.text(sha256Bytes(out.bytes)); durableWrite(stage_ / "ledger.bin", out.bytes);
}
UninstallJournal UninstallJournal::open(const fs::path& stage) {
    const auto bytes = readBoundedFile(stage / "ledger.bin"); Decoder in(bytes);
    const auto format = in.text();
    if (format != "M6UNINSTALLJOURNAL2" && format != "M6UNINSTALLJOURNAL3") throw std::runtime_error("Unsupported uninstall recovery state");
    UninstallJournal journal; journal.stage_ = stage;
    if (format == "M6UNINSTALLJOURNAL3") {
        const auto started = in.number(); if (started > 1) throw std::runtime_error("Invalid uninstall execution state"); journal.executionStarted_ = started != 0;
    } else journal.executionStarted_ = true; // Older ledgers cannot certify that execution never began.
    journal.nonce_ = in.text(); journal.scope_ = decodeUninstallScope(in.text());
    auto& snapshot = journal.inspection_; snapshot.originalAccount = in.text();
    const auto users = in.count(); if (users > 4096) throw std::runtime_error("Uninstall profile limit exceeded");
    for (std::size_t i = 0; i < users; ++i) {
        UserStorage user; user.account = in.text(); user.home = in.path(); user.dataRoot = in.path(); user.cacheRoot = in.path();
        user.downloads = in.path(); user.manifestPath = in.path(); user.additionalDataRoots = paths(in); user.additionalDownloadRoots = paths(in);
        const auto resolved = in.number(); if (resolved > 1) throw std::runtime_error("Invalid profile coverage"); user.locationsResolved = resolved != 0;
        snapshot.users.push_back(std::move(user));
    }
    const auto apps = in.count();
    for (std::size_t i = 0; i < apps; ++i) { auto root = in.path(), executable = in.path(); auto account = in.text(), registration = in.text(); snapshot.applications.push_back({root, executable, account, registration}); }
    snapshot.payloadDirectories = paths(in);
    const auto directories = in.count();
    for (std::size_t i = 0; i < directories; ++i) { auto path = in.path(); auto account = in.text(); auto identity = in.identity(); snapshot.ownedDirectories.push_back({path, account, identity}); }
    snapshot.nativeState = in.text(); snapshot.nativeReview = in.text();
    const auto count = in.count();
    for (std::size_t i = 0; i < count; ++i) {
        Target target; target.path = in.path(); target.boundary = in.path(); target.account = in.text(); const auto kind = in.number();
        if (kind > std::uint64_t(Category::Maintenance)) throw std::runtime_error("Invalid uninstall target category");
        target.category = Category(kind); target.evidence = in.text(); target.identity = in.identity(); snapshot.inventory.targets.push_back(std::move(target));
    }
    snapshot.inventory.issues = issues(in); const auto preserved = in.count();
    for (std::size_t i = 0; i < preserved; ++i) { auto path = in.path(); auto account = in.text(), reason = in.text(); preserveLegacyFile(snapshot.inventory, path, account, reason); }
    journal.failures_ = issues(in);
    const auto prefix = bytes.size() - in.bytes.size(); const auto digest = in.text(); in.end();
    if (sha256Bytes(std::string_view(bytes).substr(0, prefix)) != digest) throw std::runtime_error("Uninstall recovery state failed integrity verification");
    validatePlan(snapshot, stage);
    if (journal.nonce_.size() != 64 || journal.nonce_.find_first_not_of("0123456789abcdef") != std::string::npos) throw std::runtime_error("Invalid uninstall journal nonce");
    return journal; // An earlier process's approval never authorizes this session.
}
void UninstallJournal::refresh(const UninstallInspection& fresh) {
    if (fresh.originalAccount != inspection_.originalAccount) throw std::runtime_error("Uninstall recovery belongs to a different initiating account");
    auto merged = fresh;
    for (const auto& user : inspection_.users) {
        const auto found = std::find_if(merged.users.begin(), merged.users.end(), [&](const auto& candidate) { return candidate.account == user.account; });
        if (found == merged.users.end()) {
            merged.users.push_back(user);
            merged.inventory.issues.push_back({user.home, user.account, "Previously reviewed OS account is no longer discoverable; original coverage remains unresolved", true});
        } else if (found->home != user.home || found->dataRoot != user.dataRoot || found->cacheRoot != user.cacheRoot || found->downloads != user.downloads)
            throw std::runtime_error("Native account locations changed; original recovery coverage cannot be silently remapped");
    }
    for (const auto& target : inspection_.inventory.targets)
        if (std::none_of(merged.inventory.targets.begin(), merged.inventory.targets.end(), [&](const auto& candidate) { return candidate.path == target.path; })) merged.inventory.targets.push_back(target);
    for (const auto& app : inspection_.applications) {
        if (std::none_of(merged.applications.begin(), merged.applications.end(), [&](const auto& candidate) { return candidate.root == app.root; })) merged.applications.push_back(app);
        if (!entryExists(app.executable)) std::erase_if(merged.inventory.issues, [&](const auto& issue) {
            return issue.path == app.root && (issue.reason == "Registered application copy is inaccessible or has unverified product identity" ||
                issue.reason == "Installed copy has unverified product identity or is a development tree");
        });
    }
    for (const auto& directory : inspection_.ownedDirectories)
        if (std::none_of(merged.ownedDirectories.begin(), merged.ownedDirectories.end(), [&](const auto& candidate) { return candidate.path == directory.path; })) merged.ownedDirectories.push_back(directory);
    for (const auto& file : inspection_.inventory.preservedLegacy) preserveLegacyFile(merged.inventory, file.path, file.account, file.reason);
    for (const auto& issue : inspection_.inventory.issues)
        if (issue.unresolved && !issue.path.empty() && entryExists(issue.path) &&
            std::none_of(merged.inventory.issues.begin(), merged.inventory.issues.end(), [&](const auto& candidate) { return candidate.path == issue.path; })) merged.inventory.issues.push_back(issue);
    // A native uninstaller/receipt may outlive the main executable. Its
    // protected original proof remains usable, and changed native metadata
    // still blocks removal rather than being silently reauthorized.
    if (!inspection_.nativeState.empty()) { merged.nativeState = inspection_.nativeState; merged.nativeReview = inspection_.nativeReview; }
    validatePlan(merged, stage_); inspection_ = std::move(merged); approvedHere_ = false; approved_.clear(); save();
}
std::vector<RemovalState> UninstallJournal::currentStates() const {
    std::vector<RemovalState> result;
    for (std::size_t i = 0; i < inspection_.inventory.targets.size(); ++i) {
        const auto& target = inspection_.inventory.targets[i];
        try {
            const auto stage = removalStagingPath(target, nonce_, i);
            const bool present = entryExists(target.path), staged = entryExists(stage);
            if (staged) validateContainedPath(stage / ".recovery-probe", target.boundary);
            if (present) result.push_back(!staged && stillMatches(target) ? RemovalState::Pending : RemovalState::Changed);
            else if (!staged) { verifyAbsentLocation(target); result.push_back(RemovalState::Removed); }
            else if (!entryExists(stage / "item")) result.push_back(fs::is_empty(stage) ? RemovalState::Staged : RemovalState::Changed);
            else result.push_back(movedMatches(target, stage / "item") ? RemovalState::Staged : RemovalState::Changed);
        } catch (const std::exception&) { result.push_back(RemovalState::Changed); }
    }
    return result;
}
std::string UninstallJournal::reviewDigest() const {
    Encoder out; out.text("M6UNINSTALLSESSION1"); out.text(nonce_); out.text(uninstallDigest(inspection_));
    for (const auto state : currentStates()) out.number(std::uint64_t(state)); return sha256Bytes(out.bytes);
}
void UninstallJournal::approve(const std::string& digest, bool permitIncomplete) {
    if (digest != reviewDigest()) throw std::runtime_error("Uninstall review changed; fresh final consent is required");
    if (!inspection_.inventory.complete() && !permitIncomplete) throw std::runtime_error("Incomplete cleanup requires explicit approval of its disclosed partial scope");
    approved_ = digest; approvedHere_ = true;
}
void UninstallJournal::execute(const std::function<bool(const Target&)>& selected, const ResetCheckpoint& checkpoint) {
    if (!approvedHere_ || approved_ != reviewDigest()) throw std::runtime_error("Fresh native approval is required before uninstall execution");
    executionStarted_ = true; save(); // Durable before the first destructive operation.
    const auto states = currentStates();
    for (std::size_t i = 0; i < states.size(); ++i) {
        const auto& target = inspection_.inventory.targets[i];
        if (selected && !selected(target)) continue;
        std::erase_if(failures_, [&](const auto& failure) { return failure.path == target.path; });
        if (states[i] == RemovalState::Removed) continue;
        try {
            if (states[i] == RemovalState::Changed) throw std::runtime_error("Changed or redirected target is preserved; native ownership must be reviewed again");
            // The complete immutable file plan and deterministic quarantine
            // paths are already durable. State is derived from native object
            // identities, so rewriting the entire ledger at every file byte
            // boundary adds no recovery information.
            removeVerifiedTarget(target, removalStagingPath(target, nonce_, i), [&](std::string_view point) { if (checkpoint) checkpoint(point); });
        } catch (const std::exception& error) { failures_.push_back({target.path, target.account, error.what(), true}); save(); }
    }
    // A subsequent phase needs a new digest for the current filesystem state.
    approvedHere_ = false; save();
}
bool UninstallJournal::filesRemoved() const {
    const auto states = currentStates(); return failures_.empty() && std::all_of(states.begin(), states.end(), [](auto state) { return state == RemovalState::Removed; });
}
}
