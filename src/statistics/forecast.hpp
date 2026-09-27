#pragma once
#include "analysis.hpp"
#include <stop_token>

namespace marksix::statistics {
inline constexpr std::string_view ModelVersion = "frequency-shrinkage-v1";
inline constexpr double PriorDraws = 100;
inline constexpr size_t MinimumTraining = 200, MinimumTestDraws = 200;
inline constexpr size_t BootstrapBlock = 20, BootstrapReplicates = 2000;
inline constexpr uint64_t BootstrapSeed = 0x4d61726b53697831ULL;
inline constexpr std::string_view SelectionLabel =
    "Experimental marginal-ranking heuristic, not a most-likely joint draw or improved winning odds. "
    "Ties use the smaller number; Extra excludes the six suggestions, not the displayed marginals.";
struct Categories {
    double main = 6.0 / 49, extra = 1.0 / 49, absent = 42.0 / 49;
    double any() const { return main + extra; }
};
using Probabilities = std::array<Categories, 49>;
struct Selection { std::array<int, 6> main{}; int extra = 0; };
struct Forecast {
    std::string snapshotDigest, selectionDigest, cutoff;
    size_t trainingDraws = 0;
    bool modelAvailable = false;
    Probabilities probabilities{}; // Uniform only when history is insufficient.
    std::optional<Selection> selection;
};
Forecast forecast(const Snapshot& snapshot);
struct Loss {
    double brier = 0, log = 0, mainBrier = 0, extraBrier = 0, anyBrier = 0;
};
Loss score(const Probabilities& probabilities, const Result& actual);
struct CalibrationBin {
    uint64_t samples = 0, observed = 0;
    double predictedSum = 0;
    std::optional<double> meanPrediction() const;
    std::optional<double> observedRate() const;
};
using Calibration = std::array<std::array<CalibrationBin, 10>, 3>; // Main / Extra / any.
struct ScoredDraw {
    std::string date, id, trainingThrough;
    size_t trainingDraws = 0;
    Probabilities prediction{};
    Loss model, uniform;
};
enum class Evidence { InsufficientTestHistory, NoDemonstratedAdvantage, HistoricalHeldOutImprovement };
struct Comparison {
    double meanDifference = 0; // Uniform minus model: positive is historical improvement.
    std::optional<Interval> interval;
};
struct Backtest {
    std::string snapshotDigest, selectionDigest;
    size_t eligibleDraws = 0, excluded = 0;
    bool completenessKnown = false;
    std::vector<ScoredDraw> draws;
    Loss meanModel, meanUniform;
    Calibration modelCalibration{}, uniformCalibration{};
    Comparison brierDifference, logDifference;
    Evidence evidence = Evidence::InsufficientTestHistory;
};
// Moving blocks: uniformly sampled non-wrapping starts, concatenate 20-draw blocks,
// truncate the final block to T. mt19937_64 + rejection-bounded integers, seed above.
// Percentile endpoints linearly interpolate sorted ranks p*(2000-1), p=.025/.975.
Comparison bootstrap(const std::vector<double>& differences, std::stop_token stop = {});
Backtest backtest(const Snapshot& snapshot, std::stop_token stop = {});
}
