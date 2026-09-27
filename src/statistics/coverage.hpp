#pragma once
#include "domain.hpp"

namespace marksix::statistics {
struct KnownOmission { std::string drawId, date, evidence; };
struct SourceSummary {
    std::string id, url;
    size_t records = 0;
    std::vector<std::string> lineages;
    // Agreement alone never establishes source independence.
    bool independentCorroborationEstablished = false;
};
struct Coverage {
    size_t total = 0, eligibleCurrent49 = 0, earlierQuarantined = 0, conflicts = 0;
    std::array<size_t, 3> sourceStatuses{}; // Unverified / official / corroborated public.
    std::string earliest, latest, latestRetrievalUtc;
    bool expectedInventoryVerified = false;
    bool complete = false;
    std::vector<KnownOmission> knownOmissions;
    std::vector<SourceSummary> sources;
    std::string explanation;
};
Coverage coverage(const Snapshot& snapshot, std::vector<KnownOmission> omissions = {});
// Browse even quarantined/unverified records. This is NOT the analysis eligibility filter.
std::vector<Result> browseEra(const Snapshot& snapshot, Era era);
}
