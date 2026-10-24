#include "reset_journal.hpp"
#include "durable_files.hpp"
#include "wire_codec.hpp"
#include <algorithm>
#include <regex>
#include <set>
#include <stdexcept>

namespace marksix::maintenance {
namespace {
struct Entry { std::string name; FileIdentity identity; };
struct Journal { bool purging = false; std::vector<Entry> entries; };
void checkpoint(const ResetCheckpoint& action, std::string_view point) { if (action) action(point); }
bool allowedName(const std::string& name) {
    static const std::regex pattern("((results|adaptive|forecast_lab)\\.sqlite3(-(wal|shm|journal))?|results\\.sqlite3\\.before-v3-[a-f0-9]{24}\\.sqlite3)");
    return std::regex_match(name, pattern);
}
fs::path journalPath(const fs::path& root) { return resetGatePath(root) / "journal.bin"; }
fs::path stagedPath(const fs::path& root, std::size_t index) { return resetGatePath(root) / "files" / std::to_string(index); }
void save(const fs::path& root, const Journal& journal) {
    Encoder out; out.text("M6RESET1"); out.path(root); out.number(journal.purging); out.number(journal.entries.size());
    for (const auto& entry : journal.entries) { out.text(entry.name); out.identity(entry.identity); }
    durableWrite(journalPath(root), out.bytes);
}
Journal load(const fs::path& root) {
    const auto bytes = readBoundedFile(journalPath(root)); Decoder in(bytes);
    if (in.text() != "M6RESET1" || in.path() != root) throw std::runtime_error("Reset journal does not match this data root");
    const auto phase = in.number(); if (phase > 1) throw std::runtime_error("Invalid reset journal phase");
    Journal journal; journal.purging = phase != 0;
    const auto count = in.count(); std::set<std::string> names;
    for (std::size_t i = 0; i < count; ++i) {
        auto name = in.text(); const auto identity = in.identity();
        if (!allowedName(name) || !names.insert(name).second || !identity.linkTarget.empty() || identity.sha256.size() != 64)
            throw std::runtime_error("Unsafe reset journal entry");
        journal.entries.push_back({std::move(name), identity});
    }
    in.end();
    const auto files = resetGatePath(root) / "files";
    validateContainedPath(files / ".probe", files);
    if (entryExists(files)) for (const auto& file : fs::directory_iterator(files)) {
        bool known = false;
        for (std::size_t i = 0; i < count; ++i) if (file.path() == stagedPath(root, i)) { known = true; break; }
        if (!known) throw std::runtime_error("Unexpected content in reset recovery directory");
    }
    return journal;
}
bool movedIdentityMatches(const fs::path& path, const FileIdentity& expected) {
    const auto actual = identifyFile(path);
    return actual.volume == expected.volume && actual.file == expected.file && actual.bytes == expected.bytes &&
        actual.modified == expected.modified && actual.sha256 == expected.sha256 && actual.linkTarget.empty();
}
void finish(const fs::path& root) {
    // Single-entry removals refuse any unexpected content. A failed epilogue
    // remains gated; it never reports that an incomplete reset succeeded.
    if (entryExists(resetGatePath(root) / "files")) durableRemove(resetGatePath(root) / "files");
    if (entryExists(journalPath(root))) durableRemove(journalPath(root));
    durableRemove(resetGatePath(root));
}
}
std::string inventoryDigest(Operation operation, const Inventory& inventory) {
    Encoder out; out.text("M6APPROVAL1"); out.number(std::uint64_t(operation));
    auto targets = inventory.targets;
    std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return pathUtf8(a.path) < pathUtf8(b.path); });
    out.number(targets.size());
    for (const auto& target : targets) {
        out.path(target.path); out.path(target.boundary); out.text(target.account); out.number(std::uint64_t(target.category));
        out.text(target.evidence); out.identity(target.identity);
    }
    auto preserved = inventory.preservedLegacy;
    std::sort(preserved.begin(), preserved.end(), [](const auto& a, const auto& b) {
        return a.account == b.account ? pathUtf8(a.path) < pathUtf8(b.path) : a.account < b.account;
    });
    out.number(preserved.size());
    for (const auto& file : preserved) { out.path(file.path); out.text(file.account); out.text(file.reason); }
    out.number(inventory.issues.size());
    for (const auto& issue : inventory.issues) { out.path(issue.path); out.text(issue.account); out.text(issue.reason); out.number(issue.unresolved); }
    return sha256Bytes(out.bytes);
}
fs::path resetGatePath(const fs::path& dataRoot) { return dataRoot / "statistics/.marksix-reset"; }
bool resetPending(const fs::path& root) {
    validateContainedPath(resetGatePath(root), root);
    return entryExists(resetGatePath(root));
}
void resetRecords(const fs::path& root, const Inventory& approved, const std::string& digest, const ResetCheckpoint& action) {
    validateBoundary(root, {});
    if (!approved.complete() || inventoryDigest(Operation::ResetRecords, approved) != digest)
        throw std::runtime_error("Reset needs a complete, unchanged approved inventory");
    if (resetPending(root)) throw std::runtime_error("An earlier reset must be recovered first");
    Journal journal;
    for (const auto& target : approved.targets) {
        if ((target.category != Category::Records && target.category != Category::RecoveryCopy) ||
            target.path.parent_path() != root / "statistics" || !allowedName(pathUtf8(target.path.filename())) ||
            intentionallyPreserved(approved, target.path) || !stillMatches(target))
            throw std::runtime_error("Reset target changed or is outside the active archive");
        journal.entries.push_back({pathUtf8(target.path.filename()), target.identity});
    }
    if (journal.entries.empty()) return;
    makePrivateDirectory(resetGatePath(root));
    makePrivateDirectory(resetGatePath(root) / "files");
    save(root, journal); checkpoint(action, "journal");
    for (std::size_t i = 0; i < journal.entries.size(); ++i) {
        const auto original = root / "statistics" / journal.entries[i].name;
        const auto& target = approved.targets[i];
        if (!stillMatches(target)) throw std::runtime_error("Reset target changed before staging");
        durableRename(original, stagedPath(root, i));
        if (!movedIdentityMatches(stagedPath(root, i), journal.entries[i].identity)) throw std::runtime_error("Staged reset file changed");
        checkpoint(action, "staged");
    }
    journal.purging = true; save(root, journal); checkpoint(action, "purging");
    (void)recoverReset(root, action);
}
ResetRecovery recoverReset(const fs::path& root, const ResetCheckpoint& action) {
    if (!resetPending(root)) return ResetRecovery::NothingPending;
    if (!entryExists(journalPath(root))) {
        // A crash while reserving/finalizing an empty gate has no data to
        // recover. Unknown entries still make these nonrecursive removals fail.
        finish(root); return ResetRecovery::NothingPending;
    }
    auto journal = load(root);
    for (std::size_t i = 0; i < journal.entries.size(); ++i) {
        const auto original = root / "statistics" / journal.entries[i].name;
        const auto staged = stagedPath(root, i);
        const bool hasOriginal = entryExists(original), hasStaged = entryExists(staged);
        if ((hasOriginal && hasStaged) || (hasOriginal && !movedIdentityMatches(original, journal.entries[i].identity)) ||
            (hasStaged && !movedIdentityMatches(staged, journal.entries[i].identity)))
            throw std::runtime_error("Reset recovery encountered changed or conflicting files; workspace remains blocked");
        if (journal.purging && hasOriginal) throw std::runtime_error("A writer recreated an archive during reset; workspace remains blocked");
        if (!journal.purging && !hasOriginal && !hasStaged) throw std::runtime_error("A reset recovery file is missing; workspace remains blocked");
    }
    for (std::size_t i = 0; i < journal.entries.size(); ++i) {
        const auto staged = stagedPath(root, i);
        if (!entryExists(staged)) continue;
        if (!movedIdentityMatches(staged, journal.entries[i].identity)) throw std::runtime_error("Recovery file changed before removal");
        if (journal.purging) { durableRemove(staged); checkpoint(action, "purged"); }
        else { durableRename(staged, root / "statistics" / journal.entries[i].name); checkpoint(action, "restored"); }
    }
    checkpoint(action, "finalizing"); finish(root);
    return journal.purging ? ResetRecovery::Purged : ResetRecovery::Restored;
}
}
