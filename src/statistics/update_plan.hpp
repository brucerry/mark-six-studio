#pragma once
#include "domain.hpp"
#include <stop_token>

namespace marksix::statistics {
struct DateRange {
    std::string from, through; // Inclusive Hong Kong civil dates.
    bool operator==(const DateRange&) const = default;
};
struct UpdatePolicy {
    std::string sourceId, version;
    DateRange available; // Reviewed lower bound and current requested cutoff.
    size_t maximumWindowDays = 0, correctionOverlapDays = 0;
};
// Internal input contract: only receipts committed with accepted provider data.
// Not an import format, authentication proof or a claim of expected inventory.
struct CommittedWindow {
    std::string sourceId, policyVersion;
    DateRange range;
    std::string responseSha256;
};
enum class UpdateDisposition { New, Unchanged, NeedsVerification, Conflict };
struct UpdateComparison {
    std::vector<UpdateDisposition> rows; // Same order as the supplied source rows.
    size_t added = 0, unchanged = 0, needsVerification = 0, conflicts = 0;
};
struct UpdatePlan {
    std::string sourceId, policyVersion, snapshotDigest;
    std::vector<DateRange> requests; // Sorted, disjoint, bounded date requests.
    DateRange correctionOverlap;
    bool latestNeedsReconciliation = false;
};
// Pure, cancellable functions: no network, storage writes or trust promotion.
// Provider enablement and transactional receipt persistence are separate gates.
UpdateComparison compareUpdate(const Snapshot& local, const std::vector<Result>& fetched,
                               std::stop_token stop = {});
UpdatePlan planUpdate(const UpdatePolicy& policy, const Snapshot& local,
                      const std::vector<CommittedWindow>& committed,
                      const std::optional<Result>& latestCompleted = {}, std::stop_token stop = {});
}
