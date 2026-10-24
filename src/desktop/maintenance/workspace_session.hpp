#pragma once
#include "native_runtime.hpp"
#include "reset_journal.hpp"
#include "machine_gate.hpp"
#include <functional>
#include <stdexcept>

namespace marksix::maintenance {
class WorkspaceSession {
public:
    using GateReader = std::function<std::optional<MachineGate>()>;
    explicit WorkspaceSession(const fs::path& root, const GateReader& machine = readMachineGate) {
        if (machine()) throw std::runtime_error("System uninstall/cleanup is active or interrupted. Finish its standalone native helper before opening any workspace.");
        if (resetPending(root)) throw std::runtime_error("An interrupted records reset must be recovered before opening this workspace.");
        lease_ = FileLease::acquire(sessionLeasePath(root), FileLease::Mode::Shared);
        if (!lease_ || resetPending(root) || machine()) throw std::runtime_error("Storage maintenance is active or its lock is unavailable. Close other instances and retry.");
    }
private:
    std::unique_ptr<FileLease> lease_;
};
}
