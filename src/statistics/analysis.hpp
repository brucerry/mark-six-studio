#pragma once
#include "domain.hpp"
#include <stop_token>

namespace marksix::statistics {
enum class Scope { Main, Extra, Any };
struct Baseline { double inclusion, complement; std::optional<double> pair; };
Baseline baseline(Scope scope);
constexpr uint64_t SixSetCount = 13983816;
double sixSetProbability();
struct Interval { double low, high; };
inline constexpr std::string_view WilsonLabel =
    "Pointwise 95% Wilson interval; stationary independent-draw sampling assumption. "
    "Not next-draw odds, a simultaneous 49-number test or a fairness certification.";
std::optional<Interval> wilson(size_t count, size_t draws);
enum class GapStatus { ExactWithinVerifiedCoverage, IncompleteCoverage, LeftCensored, NoData };
struct NumberSummary {
    size_t count = 0;
    std::optional<double> observedRate;
    double expectedCount = 0;
    std::optional<Interval> interval;
    std::optional<size_t> observedDrawsSinceLast;
    GapStatus gapStatus = GapStatus::NoData;
};
struct RollingPoint {
    std::string date, id;
    size_t draws = 0;
    bool fullWindow = false;
    std::array<double, 49> rates{};
};
struct Analysis {
    std::string snapshotDigest, selectionDigest, from, through;
    Filter filter;
    Scope scope = Scope::Main;
    size_t draws = 0, excluded = 0;
    size_t rollingWindow = 100;
    bool completenessKnown = false;
    std::array<NumberSummary, 49> numbers{};
    // Upper triangle only: [a-1][b-1], a < b. Extra scope is unavailable.
    std::optional<std::array<std::array<size_t, 49>, 49>> pairs;
    std::vector<RollingPoint> rolling;
};
Analysis analyze(const Snapshot& snapshot, Scope scope = Scope::Main,
                 const Filter& filter = {}, size_t rollingWindow = 100, std::stop_token stop = {});
}
