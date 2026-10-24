#pragma once
#include "native_runtime.hpp"

namespace marksix::maintenance {
struct MachineGate {
    std::string nonce;
    fs::path stage;
    ProcessIdentity executor;
};
std::string encodeMachineGate(const MachineGate& gate);
MachineGate decodeMachineGate(std::string_view bytes);
std::optional<MachineGate> readMachineGate(); // Read-only; malformed state blocks startup.
void publishMachineGate(const MachineGate& gate); // Administrator only; never replaces another operation.
void updateMachineGateExecutor(const MachineGate& gate); // Same nonce/stage; caller holds protected ledger lease.
// The caller must also hold its private ledger lease, preventing two retries
// of the same operation from racing finalization.
void removeMachineGate(const std::string& operationNonce); // Administrator only; exact operation.
}
