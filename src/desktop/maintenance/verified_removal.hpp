#pragma once
#include "reset_journal.hpp"

namespace marksix::maintenance {
// The privileged lifecycle must first persist this deterministic staging path
// in its protected operation ledger. Never accepts a UI-supplied Target.
fs::path removalStagingPath(const Target& target, const std::string& operationNonce, std::size_t index);
// One exact approved inode/Windows file object, never a recursive directory
// removal. Darwin quarantines through pinned directory handles before checking
// identity/content again; the same step can resume after interruption.
void removeVerifiedTarget(const Target& target, const fs::path& staging,
    const ResetCheckpoint& checkpoint = {});
// Returns false for a nonempty directory. No recursive deletion, no following
// links, and no reliance on a pathname-only check followed by remove_all.
bool removeVerifiedEmptyDirectory(const fs::path& path, const FileIdentity& identity);
}
