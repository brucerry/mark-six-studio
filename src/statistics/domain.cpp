#include "domain.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace marksix::statistics {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
bool digits(std::string_view s) {
    return !s.empty() && s.find_first_not_of("0123456789") == std::string_view::npos;
}
int decimal(std::string_view s) {
    int n = 0; for (char c : s) n = n * 10 + c - '0'; return n;
}
bool text(std::string_view s, size_t limit = 2048) {
    return s.size() <= limit && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 32 && c != 127; });
}
bool utc(std::string_view s) {
    if (s.size() < 20 || s.size() > 28 || !validDate(s.substr(0, 10)) || s[10] != 'T' ||
        s[13] != ':' || s[16] != ':' || s.back() != 'Z') return false;
    if (!digits(s.substr(11, 2)) || !digits(s.substr(14, 2)) || !digits(s.substr(17, 2))) return false;
    if (decimal(s.substr(11, 2)) > 23 || decimal(s.substr(14, 2)) > 59 || decimal(s.substr(17, 2)) > 59) return false;
    return s.size() == 20 || (s.size() >= 22 && s[19] == '.' && digits(s.substr(20, s.size() - 21)));
}
void field(std::string& out, std::string_view value) {
    out += std::to_string(value.size()); out += ':'; out += value;
}
void number(std::string& out, int n) { field(out, std::to_string(n)); }
std::string serialize(const Result& r) {
    std::string out = "marksix-result-v1:";
    field(out, r.id); field(out, r.date); number(out, int(r.era));
    for (int n : r.main) number(out, n); number(out, r.extra);
    for (const auto* s : {&r.source.sourceId, &r.source.url, &r.source.retrievedUtc,
                         &r.source.contentSha256, &r.source.lineage, &r.source.verificationEvidence}) field(out, *s);
    number(out, int(r.source.trust)); number(out, r.unresolvedConflict ? 1 : 0);
    number(out, r.extractionOrder ? 1 : 0);
    if (r.extractionOrder) for (int n : *r.extractionOrder) number(out, n);
    field(out, r.orderEvidence); return out;
}
}
bool validDate(std::string_view s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-' || !digits(s.substr(0, 4)) ||
        !digits(s.substr(5, 2)) || !digits(s.substr(8, 2))) return false;
    using namespace std::chrono;
    const int y = decimal(s.substr(0, 4));
    return y >= 1975 && y <= 9999 && year_month_day{year{y}, month{unsigned(decimal(s.substr(5, 2)))},
                                                    day{unsigned(decimal(s.substr(8, 2)))}}.ok();
}
std::string sha256(std::string_view bytes) {
    require(bytes.size() <= std::numeric_limits<ULONG>::max(), "Digest input too large");
    std::array<unsigned char, 32> hash{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                   reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), ULONG(bytes.size()),
                   hash.data(), ULONG(hash.size())) != 0) throw std::runtime_error("SHA-256 failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(64);
    for (unsigned char b : hash) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}
Result canonicalize(Result r) {
    require(validDate(r.date), "Invalid Hong Kong draw date");
    require(r.id.size() == 6 && r.id[2] == '/' && digits(std::string_view(r.id).substr(0, 2)) &&
            digits(std::string_view(r.id).substr(3)) && decimal(std::string_view(r.id).substr(3)) > 0 &&
            r.id.substr(0, 2) == r.date.substr(2, 2), "Invalid or contradictory published draw identifier");
    require(r.era == Era::Current49 || r.era == Era::EarlierUnreviewed, "Unknown era");
    require(r.era == Era::Current49 ? r.date >= "2002-07-04" : r.date < "2002-07-04", "Date contradicts era");
    // 1..49 is a structural ceiling only for quarantined earlier rows, not an asserted earlier pool.
    std::set<int> seen;
    for (int n : r.main) require(n >= 1 && n <= 49 && seen.insert(n).second, "Duplicate or out-of-range main number");
    require(r.extra >= 1 && r.extra <= 49 && seen.insert(r.extra).second, "Duplicate or out-of-range Extra number");
    std::sort(r.main.begin(), r.main.end());
    require(!r.source.sourceId.empty() && text(r.source.sourceId, 128) &&
            r.source.url.starts_with("https://") && r.source.url.size() > 8 && text(r.source.url) &&
            utc(r.source.retrievedUtc) && text(r.source.lineage) && text(r.source.verificationEvidence), "Invalid provenance");
    require(r.source.contentSha256.size() == 64 &&
            r.source.contentSha256.find_first_not_of("0123456789abcdef") == std::string::npos, "Invalid source content digest");
    require(r.source.trust == Trust::Unverified || r.source.trust == Trust::OfficialRetrieval ||
            r.source.trust == Trust::CorroboratedPublic, "Unknown verification status");
    require(r.source.trust == Trust::Unverified || !r.source.verificationEvidence.empty(), "Verified source requires evidence");
    require(text(r.orderEvidence), "Invalid extraction-order evidence");
    if (r.extractionOrder) {
        auto ordered = *r.extractionOrder; std::sort(ordered.begin(), ordered.end());
        require(ordered == r.main && !r.orderEvidence.empty(), "Extraction order needs matching numbers and evidence");
    } else require(r.orderEvidence.empty(), "Order evidence without order");
    return r;
}
std::string revisionDigest(const Result& r) { return sha256(serialize(canonicalize(r))); }
bool eligible(const Result& r) {
    return r.era == Era::Current49 && !r.unresolvedConflict && r.source.trust != Trust::Unverified;
}
Snapshot::Snapshot(std::vector<Result> records, bool inventoryVerified, std::string inventoryEvidence)
    : inventoryEvidence_(std::move(inventoryEvidence)), inventoryVerified_(inventoryVerified) {
    require(records.size() <= 100000, "Dataset too large");
    require(text(inventoryEvidence_) && (!inventoryVerified_ || !inventoryEvidence_.empty()), "Inventory claim requires evidence");
    std::set<std::string> identities;
    for (auto& r : records) {
        r = canonicalize(std::move(r));
        // This is an active snapshot, not a revision store. Quarantine/resolution occurs before construction.
        require(identities.insert(r.date.substr(0, 4) + ":" + r.id).second, "Duplicate/conflicting draw identity in snapshot");
    }
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        return std::tie(a.date, a.id) < std::tie(b.date, b.id);
    });
    records_ = std::move(records);
    std::string bytes = "marksix-snapshot-v1:official-or-corroborated/current49/";
    number(bytes, inventoryVerified_ ? 1 : 0); field(bytes, inventoryEvidence_);
    for (const auto& r : records_) field(bytes, revisionDigest(r));
    digest_ = sha256(bytes);
}
std::vector<Result> Snapshot::select(const Filter& f) const {
    require((f.from.empty() || validDate(f.from)) && (f.through.empty() || validDate(f.through)) &&
            (f.from.empty() || f.through.empty() || f.from <= f.through), "Invalid date filter");
    std::vector<Result> out;
    for (const auto& r : records_) if (eligible(r) && (f.from.empty() || r.date >= f.from) &&
                                      (f.through.empty() || r.date <= f.through)) out.push_back(r);
    if (f.lastN && out.size() > f.lastN) out.erase(out.begin(), out.end() - f.lastN);
    return out;
}
std::string Snapshot::selectionDigest(const Filter& f) const {
    std::string bytes = "marksix-selection-v1:"; field(bytes, digest_);
    field(bytes, f.from); field(bytes, f.through); field(bytes, std::to_string(f.lastN));
    for (const auto& r : select(f)) field(bytes, revisionDigest(r));
    return sha256(bytes);
}
}
