#pragma once
#include "forecast.hpp"

namespace marksix::statistics {
inline constexpr std::string_view AdaptiveVersion = "adaptive-mixture-v1";
// Canonical protocol bytes: changing any learning decision requires a new version.
inline constexpr std::string_view AdaptiveProtocol =
    "adaptive-mixture-v1|seed=2002-07-04:02/053|experts=uniform,expanding,half50,half200"
    "|prior=100|eta=1|uniform-share=0.5|initial-log-weights=0,0,0,0"
    "|loss=mean49-category-log|order=predict,score,weights,counts"
    "|decay=exp(-ln(2)/halfLife)|warmup=200|ranking=main-desc,number-asc,distinct-extra";
std::string adaptiveProtocolDigest();
// Canonical value identity only; does not train or score a draw.
std::string adaptiveValuePrefix(const std::string& previous, const Result& result);
struct AdaptiveCounts {
    double n = 0;
    std::array<double, 49> main{}, extra{};
};
struct AdaptiveState {
    size_t draws = 0;
    std::string lastDate, lastId, prefixDigest;
    std::array<AdaptiveCounts, 3> counts{};
    std::array<double, 4> logWeights{};
};
struct AdaptivePrediction {
    Probabilities probabilities{};
    std::array<Probabilities, 4> experts{};
    std::array<double, 4> weights{};
    Selection selection{};
    size_t trainingDraws = 0;
    bool warmup = true;
    std::string cutoff, prefixDigest;
};
struct AdaptiveStep {
    Result actual;
    AdaptivePrediction prediction;
    Loss adaptiveLoss, uniformLoss;
    std::array<double, 4> expertLogLoss{}, weightsAfter{};
    AdaptiveState before, after;
};
struct AdaptiveReplay {
    bool available = false;
    std::string unavailableReason, protocolDigest, snapshotDigest;
    size_t excluded = 0;
    bool completenessKnown = false;
    AdaptiveState state;
    std::optional<AdaptivePrediction> next;
    std::vector<AdaptiveStep> steps;
};
void validateAdaptiveState(const AdaptiveState& state);
AdaptiveState seedAdaptive(const Result& seed);
AdaptivePrediction predictAdaptive(const AdaptiveState& state);
// Strong exception guarantee: score the saved prior prediction, then publish state.
AdaptiveStep observeAdaptive(AdaptiveState& state, const Result& actual);
AdaptiveReplay replayAdaptive(const Snapshot& snapshot, std::stop_token stop = {});
struct AdaptiveDiagnostics {
    size_t draws = 0, mainHits = 0, extraMatches = 0;
    Loss mean;
    Calibration calibration{};
};
struct AdaptiveWindow {
    std::string firstDate, firstId, lastDate, lastId;
    AdaptiveDiagnostics adaptive, uniform, fixed;
    Comparison uniformMinusAdaptiveLog, uniformMinusAdaptiveBrier;
    Comparison fixedMinusAdaptiveLog, fixedMinusAdaptiveBrier;
};
struct AdaptiveEvaluation {
    AdaptiveWindow all, common;
    // Replay scores never promote an adaptive policy to demonstrated future advantage.
    std::string evidence = "No demonstrated predictive advantage; development replay, descriptive intervals only.";
};
// Consumes the unchanged fixed-v1 backtest; requires exactly matching snapshot/targets.
AdaptiveEvaluation evaluateAdaptive(const AdaptiveReplay& replay, const Backtest& fixed, std::stop_token stop = {});
}
