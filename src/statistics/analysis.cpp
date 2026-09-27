#include "analysis.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace marksix::statistics {
Baseline baseline(Scope scope) {
    int k = 0;
    switch (scope) {
    case Scope::Main: k = 6; break;
    case Scope::Extra: k = 1; break;
    case Scope::Any: k = 7; break;
    default: throw std::invalid_argument("Unknown result scope");
    }
    return {double(k) / 49, double(49 - k) / 49,
            k == 1 ? std::nullopt : std::optional<double>(double(k * (k - 1)) / (49 * 48))};
}
double sixSetProbability() {
    uint64_t choose = 1;
    for (uint64_t i = 1; i <= 6; ++i) choose = choose * (49 - i + 1) / i;
    return 1.0 / double(choose);
}
std::optional<Interval> wilson(size_t c, size_t n) {
    if (c > n) throw std::invalid_argument("Count exceeds denominator");
    if (!n) return std::nullopt;
    constexpr double z = 1.959963984540054;
    const double p = double(c) / double(n), a = z * z / double(n), denominator = 1 + a;
    const double center = (p + a / 2) / denominator;
    const double half = z * std::sqrt(p * (1 - p) / double(n) + z * z / (4 * double(n) * double(n))) / denominator;
    return Interval{std::max(0.0, center - half), std::min(1.0, center + half)};
}
Analysis analyze(const Snapshot& snapshot, Scope scope, const Filter& filter, size_t rollingWindow, std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Statistics analysis cancelled");
    if (!rollingWindow || rollingWindow > 100000) throw std::invalid_argument("Invalid rolling window");
    const auto reference = baseline(scope);
    Analysis out; out.snapshotDigest = snapshot.digest(); out.selectionDigest = snapshot.selectionDigest(filter);
    out.filter = filter; out.scope = scope; out.rollingWindow = rollingWindow;
    out.completenessKnown = snapshot.inventoryVerified();
    // A rejected/quarantined row breaks coverage even if the source inventory is known.
    for (const auto& r : snapshot.records()) if (!eligible(r)) {
        ++out.excluded;
        if (r.era == Era::Current49 && (filter.from.empty() || r.date >= filter.from) &&
            (filter.through.empty() || r.date <= filter.through)) out.completenessKnown = false;
    }
    const auto rows = snapshot.select(filter); out.draws = rows.size();
    if (!rows.empty()) { out.from = rows.front().date; out.through = rows.back().date; }
    if (scope != Scope::Extra) out.pairs.emplace();
    std::array<std::optional<size_t>, 49> last{};
    std::array<size_t, 49> rollingCounts{};
    std::vector<std::array<bool, 49>> events;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (stop.stop_requested()) throw std::runtime_error("Statistics analysis cancelled");
        std::array<bool, 49> event{};
        if (scope != Scope::Extra) for (int n : rows[i].main) event[n - 1] = true;
        if (scope != Scope::Main) event[rows[i].extra - 1] = true;
        for (size_t n = 0; n < 49; ++n) {
            if (event[n]) { ++out.numbers[n].count; last[n] = i; ++rollingCounts[n]; }
            if (i >= rollingWindow && events[i - rollingWindow][n]) --rollingCounts[n];
            if (out.pairs && event[n]) for (size_t b = n + 1; b < 49; ++b)
                if (event[b]) ++(*out.pairs)[n][b];
        }
        events.push_back(event);
        RollingPoint point; point.date = rows[i].date; point.id = rows[i].id;
        point.draws = std::min(i + 1, rollingWindow); point.fullWindow = i + 1 >= rollingWindow;
        for (size_t n = 0; n < 49; ++n) point.rates[n] = double(rollingCounts[n]) / double(point.draws);
        out.rolling.push_back(std::move(point));
    }
    for (size_t n = 0; n < 49; ++n) {
        auto& value = out.numbers[n]; value.expectedCount = double(out.draws) * reference.inclusion;
        value.interval = wilson(value.count, out.draws);
        if (out.draws) {
            value.observedRate = double(value.count) / double(out.draws);
            value.observedDrawsSinceLast = last[n] ? out.draws - 1 - *last[n] : out.draws;
            value.gapStatus = !last[n] ? GapStatus::LeftCensored :
                out.completenessKnown ? GapStatus::ExactWithinVerifiedCoverage : GapStatus::IncompleteCoverage;
        }
    }
    return out;
}
}
