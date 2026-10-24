#pragma once
#include "storage_inventory.hpp"

namespace marksix::maintenance {
// Additional fixed product metadata. Already-held paths must come from actual
// native leases owned by the elevated provider, never from an IPC request.
void inspectUninstallMetadata(Inventory& inventory, const std::vector<UserStorage>& users,
    const std::vector<fs::path>& alreadyHeldLeases = {});
}
