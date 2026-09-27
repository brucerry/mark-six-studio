#include "coverage.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace marksix::statistics {
namespace {
std::string timeKey(const std::string& utc) {
    const auto fraction = utc.size() > 20 ? utc.substr(20, utc.size() - 21) : "";
    return utc.substr(0, 19) + fraction + std::string(7 - fraction.size(), '0');
}
}
Coverage coverage(const Snapshot& snapshot, std::vector<KnownOmission> omissions) {
    Coverage out; out.total = snapshot.records().size(); out.expectedInventoryVerified = snapshot.inventoryVerified();
    std::map<std::pair<std::string, std::string>, SourceSummary> sources;
    std::set<std::string> present, missing;
    for (const auto& r : snapshot.records()) {
        present.insert(r.date.substr(0, 4) + ":" + r.id);
        if (out.earliest.empty()) out.earliest = r.date; out.latest = r.date;
        if (out.latestRetrievalUtc.empty() || timeKey(r.source.retrievedUtc) > timeKey(out.latestRetrievalUtc))
            out.latestRetrievalUtc = r.source.retrievedUtc;
        if (eligible(r)) ++out.eligibleCurrent49;
        if (r.era == Era::EarlierUnreviewed) ++out.earlierQuarantined;
        if (r.unresolvedConflict) ++out.conflicts;
        ++out.sourceStatuses[size_t(r.source.trust)];
        auto& source = sources[{r.source.sourceId,r.source.url}]; source.id = r.source.sourceId; source.url = r.source.url; ++source.records;
        if (std::find(source.lineages.begin(), source.lineages.end(), r.source.lineage) == source.lineages.end())
            source.lineages.push_back(r.source.lineage);
    }
    for (const auto& omission : omissions) {
        if (!validDate(omission.date) || omission.drawId.size() != 6 || omission.drawId[2] != '/' ||
            omission.drawId.substr(0, 2) != omission.date.substr(2, 2) ||
            omission.drawId.substr(0, 2).find_first_not_of("0123456789") != std::string::npos ||
            omission.drawId.substr(3).find_first_not_of("0123456789") != std::string::npos || omission.drawId.substr(3) == "000" ||
            omission.evidence.empty() || omission.evidence.size() > 2048 ||
            std::any_of(omission.evidence.begin(), omission.evidence.end(), [](unsigned char c) { return c < 32 || c == 127; }))
            throw std::invalid_argument("Known omission requires a valid identity and source evidence");
        const auto key = omission.date.substr(0, 4) + ":" + omission.drawId;
        if (present.contains(key) || !missing.insert(key).second) throw std::invalid_argument("Omission is present or duplicated");
    }
    out.knownOmissions = std::move(omissions);
    for (auto& [key, source] : sources) { (void)key; std::sort(source.lineages.begin(), source.lineages.end()); out.sources.push_back(std::move(source)); }
    out.complete = out.expectedInventoryVerified && out.knownOmissions.empty() && out.total > 0 &&
        out.eligibleCurrent49 == out.total;
    out.explanation = out.complete ? "Complete within the independently verified supplied inventory, not all historical time." :
        "Completeness unknown or incomplete. Date ranges and sequence continuity alone do not establish expected inventory. "
        "Earlier formats, unverified imports and unresolved conflicts are excluded from current-49 analysis.";
    return out;
}
std::vector<Result> browseEra(const Snapshot& snapshot, Era era) {
    if (era != Era::Current49 && era != Era::EarlierUnreviewed) throw std::invalid_argument("Unknown browsing era");
    std::vector<Result> out;
    for (const auto& r : snapshot.records()) if (r.era == era) out.push_back(r);
    return out;
}
}
