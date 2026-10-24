#pragma once
#include "storage_inventory.hpp"
#include <map>
#include "machine_gate.hpp"

namespace marksix::maintenance {
struct FullCleanupFinalization { std::uintptr_t waitHandle = 0; fs::path protectedStage; bool cancelled = false; };
// Called after operation leases are released. Failure retains a standalone
// retry executable/state; successful finalization removes both and the stage.
void finishUserHelper(const fs::path& executable, bool success, const std::string& report,
    const fs::path& application, const fs::path& dataRoot, const FullCleanupFinalization& full = {});
FullCleanupFinalization launchPrivilegedFinalizer(const fs::path& executable, const MachineGate& gate, const ProcessIdentity& userAgent);
// macOS can retire its loaded image before the privileged broker's final
// epilogue. Retained bytes allow restoration if the broker reports a failure.
std::map<fs::path, std::string> retireUserHelper(const fs::path& executable);
void restoreUserHelper(const fs::path& executable, const std::map<fs::path, std::string>& files);
}
