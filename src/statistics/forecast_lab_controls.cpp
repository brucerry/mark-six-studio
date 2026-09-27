#include "forecast_lab.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace marksix::statistics {
namespace {
uint64_t bounded(std::mt19937_64& generator, uint64_t bound) {
    const uint64_t rejectBelow = (uint64_t(0) - bound) % bound;
    uint64_t value;
    do { value = generator(); } while (value < rejectBelow);
    return value % bound;
}
std::string dateAt(size_t index) {
    using namespace std::chrono;
    const auto date = year_month_day{sys_days{2002y/July/4} + days{int(index)}};
    std::ostringstream out;
    out << int(date.year()) << '-' << std::setw(2) << std::setfill('0') << unsigned(date.month())
        << '-' << std::setw(2) << unsigned(date.day());
    return out.str();
}
Result fairDraw(std::mt19937_64& generator, size_t index) {
    std::array<int, 49> urn{};
    for (int i = 0; i < 49; ++i) urn[size_t(i)] = i + 1;
    for (size_t i = 0; i < 7; ++i) {
        const size_t j = i + size_t(bounded(generator, 49 - i));
        std::swap(urn[i], urn[j]);
    }
    Result out;
    out.date = dateAt(index);
    std::ostringstream id;
    id << out.date.substr(2, 2) << '/' << std::setw(3) << std::setfill('0')
       << (index == 0 ? 53 : 100 + index % 800);
    out.id = id.str();
    std::copy_n(urn.begin(), 6, out.main.begin());
    out.extra = urn[6];
    out.source = {"forecast-lab-synthetic-fair-control", "https://example.invalid/synthetic-control",
                  "2026-09-27T00:00:00Z", sha256("forecast-lab-synthetic-fair-control"),
                  "local-synthetic-only", Trust::CorroboratedPublic, "synthetic fair-control fixture, not a public result"};
    return canonicalize(out);
}
}
LabControlReport labControls(size_t historyLength, double observedStatistic,
                             std::string sourceDigest, size_t maxRuns, std::stop_token stop,
                             std::function<void(size_t, size_t, const std::string&, double)> progress) {
    if (historyLength < 401 || historyLength > 100000 || !std::isfinite(observedStatistic) ||
        sourceDigest.empty() || maxRuns > LabControlRuns)
        throw std::invalid_argument("Invalid Forecast Lab control request");
    LabControlReport out;
    out.protocolDigest = labProtocolDigest();
    out.sourceDigest = std::move(sourceDigest);
    out.historyLength = historyLength;
    out.observedStatistic = observedStatistic;
    out.historyHashes.reserve(maxRuns);
    out.nullStatistics.reserve(maxRuns);
    for (size_t run = 0; run < maxRuns; ++run) {
        if (stop.stop_requested()) { out.cancelled = true; break; }
        std::mt19937_64 generator(LabControlSeed + uint64_t(run));
        std::vector<Result> history;
        history.reserve(historyLength);
        std::string digestBytes = "forecast-lab-synthetic-fair-history-v1";
        for (size_t index = 0; index < historyLength; ++index) {
            if (stop.stop_requested()) { out.cancelled = true; break; }
            auto result = fairDraw(generator, index);
            digestBytes += '|' + result.date + '|' + result.id;
            for (int number : result.main) digestBytes += '|' + std::to_string(number);
            digestBytes += '|' + std::to_string(result.extra);
            history.push_back(std::move(result));
        }
        if (out.cancelled) break;
        LabReplay replay;
        try { replay = replayLab(Snapshot(std::move(history)), stop); }
        catch (const std::runtime_error&) {
            if (!stop.stop_requested()) throw;
            out.cancelled = true;
            break;
        }
        const auto evaluation = evaluateLab(replay);
        if (!evaluation.commonTargets) throw std::runtime_error("Control has no outer targets");
        out.historyHashes.push_back(sha256(digestBytes));
        out.nullStatistics.push_back(evaluation.selector.meanHits() - 36.0 / 49.0);
        ++out.completed;
        if (progress) progress(out.completed, LabControlRuns,
                               out.historyHashes.back(), out.nullStatistics.back());
    }
    out.complete = out.completed == LabControlRuns;
    const size_t upper = std::count_if(out.nullStatistics.begin(), out.nullStatistics.end(),
        [&](double value) { return value >= observedStatistic; });
    out.tailFraction = double(1 + upper) / double(1 + out.completed);
    out.simulationStdError = std::sqrt(out.tailFraction * (1 - out.tailFraction) /
                                       double(1 + out.completed));
    return out;
}
}
