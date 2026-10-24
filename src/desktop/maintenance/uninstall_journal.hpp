#pragma once
#include "uninstall_inventory.hpp"
#include "verified_removal.hpp"

namespace marksix::maintenance {
enum class RemovalState { Pending, Staged, Removed, Changed };
class UninstallJournal {
public:
    // Caller supplies an independently generated native inspection and owns
    // the protected stage/ledger lease. This is never an IPC target-list API.
    static UninstallJournal create(const fs::path& stage, const std::string& nonce,
        const UninstallScope& scope, const UninstallInspection& inspection);
    static UninstallJournal open(const fs::path& stage);
    void refresh(const UninstallInspection& independentlyDiscovered);
    const UninstallInspection& inspection() const { return inspection_; }
    const UninstallScope& scope() const { return scope_; }
    const std::string& nonce() const { return nonce_; }
    std::vector<RemovalState> currentStates() const;
    std::string reviewDigest() const;
    void approve(const std::string& digest, bool permitIncomplete = false);
    // Native provider can order data/payload/metadata phases while retaining a
    // single approved file inventory. Changed or failed targets stay pending.
    void execute(const std::function<bool(const Target&)>& selected = {}, const ResetCheckpoint& checkpoint = {});
    const std::vector<InventoryIssue>& failures() const { return failures_; }
    bool filesRemoved() const;
    bool executionStarted() const { return executionStarted_; }
private:
    void save() const;
    fs::path stage_;
    std::string nonce_, approved_;
    UninstallScope scope_;
    UninstallInspection inspection_;
    std::vector<InventoryIssue> failures_;
    bool approvedHere_ = false;
    bool executionStarted_ = false;
};
}
