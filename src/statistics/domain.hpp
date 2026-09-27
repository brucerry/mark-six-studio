#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace marksix::statistics {
enum class Era { Current49, EarlierUnreviewed };
enum class Trust { Unverified, OfficialRetrieval, CorroboratedPublic };
struct Provenance {
    std::string sourceId, url, retrievedUtc, contentSha256, lineage;
    Trust trust = Trust::Unverified;
    // Evidence identifiers, not user-entered labels. The ingestion boundary owns trust.
    std::string verificationEvidence;
};
struct Result {
    std::string id, date; // Published YY/NNN, Hong Kong civil YYYY-MM-DD.
    std::array<int, 6> main{}; // Canonicalized unordered set, never extraction order.
    int extra = 0;
    Era era = Era::Current49;
    Provenance source;
    std::optional<std::array<int, 6>> extractionOrder;
    std::string orderEvidence;
    bool unresolvedConflict = false;
};
struct Filter {
    std::string from, through;
    size_t lastN = 0; // Zero means all eligible records, after date filtering.
};
bool validDate(std::string_view date);
Result canonicalize(Result result); // Throws on malformed rows; earlier eras stay quarantined.
std::string sha256(std::string_view bytes);
std::string revisionDigest(const Result& result);
bool eligible(const Result& result);

// Immutable, owned values: no mutable accessors or borrowed mutable data.
class Snapshot {
public:
    explicit Snapshot(std::vector<Result> records, bool inventoryVerified = false,
                      std::string inventoryEvidence = {});
    const std::vector<Result>& records() const { return records_; }
    const std::string& digest() const { return digest_; }
    bool inventoryVerified() const { return inventoryVerified_; }
    const std::string& inventoryEvidence() const { return inventoryEvidence_; }
    std::vector<Result> select(const Filter& filter = {}) const;
    std::string selectionDigest(const Filter& filter = {}) const;
private:
    std::vector<Result> records_;
    std::string digest_, inventoryEvidence_;
    bool inventoryVerified_ = false;
};
}
