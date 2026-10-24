#pragma once
#include "storage_inventory.hpp"
#include <functional>

namespace marksix::maintenance {
std::string inventoryDigest(Operation operation, const Inventory& inventory);
fs::path resetGatePath(const fs::path& dataRoot);
bool resetPending(const fs::path& dataRoot);
enum class ResetRecovery { NothingPending, Restored, Purged };
// The caller owns the exclusive storage lease and has already stopped writers.
// The optional checkpoint observes durable boundaries for native progress and
// isolated-fixture interruption tests. It never supplies paths or authority.
using ResetCheckpoint = std::function<void(std::string_view)>;
void resetRecords(const fs::path& dataRoot, const Inventory& approved,
    const std::string& approvalDigest, const ResetCheckpoint& checkpoint = {});
ResetRecovery recoverReset(const fs::path& dataRoot, const ResetCheckpoint& checkpoint = {});
}
