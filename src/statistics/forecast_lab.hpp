#pragma once
#include "adaptive.hpp"
#include <functional>

namespace marksix::statistics {
enum class LabKind { Uniform, Adaptive, Fixed, Mixture };
struct LabCandidate {
    std::string id;
    LabKind kind = LabKind::Uniform;
    int halfLife = 0;
    int prior = 0;
    double learningRate = 0;
};
const std::array<LabCandidate, 15>& labCandidates();
std::string labProtocolDigest();
Selection labRank(const Probabilities& probabilities);
struct LabCounts {
    double n = 0;
    std::array<double, 49> main{}, extra{};
};
struct LabMixtureState {
    LabCandidate candidate;
    size_t draws = 0;
    std::string lastDate, prefixDigest;
    std::array<LabCounts, 2> counts{}; // Expanding and candidate half-life.
    std::array<double, 3> logWeights{}; // Uniform, expanding, recent.
};
LabMixtureState labSeed(const LabCandidate& candidate, const Result& seed);
Probabilities labPredict(const LabMixtureState& state);
void labObserve(LabMixtureState& state, const Result& actual);
struct LabScoreRow {
    size_t ordinal = 0; // One-based eligible target position; seed is 1.
    std::array<int, 15> mainHits{};
    std::array<double, 15> logLoss{};
    std::array<Loss, 15> losses{};
    std::array<bool, 15> available{};
};
struct LabDecision {
    size_t candidate = 1; // Adaptive fallback.
    size_t validationFirst = 0, validationLast = 0, validationTargets = 0;
    bool fallback = true;
    std::string reason = "Insufficient chronological validation";
};
LabDecision labChoose(const std::vector<LabScoreRow>& history, size_t completedDraws);
struct LabForecast {
    size_t targetOrdinal = 0, completedDraws = 0;
    std::string cutoff, prefixDigest;
    LabDecision decision;
    std::array<Probabilities, 15> probabilities{};
    std::array<Selection, 15> selections{};
    std::array<bool, 15> available{};
};
struct LabStep {
    Result actual;
    LabForecast frozen;
    LabScoreRow scores;
};
struct LabReplay {
    bool available = false;
    std::string unavailableReason, protocolDigest, snapshotDigest;
    AdaptiveState adaptive;
    LabCounts fixed;
    std::array<LabMixtureState, 12> mixtures{};
    std::vector<LabScoreRow> scoreHistory;
    std::vector<LabStep> steps;
    std::optional<LabForecast> next;
};
LabReplay labStart(const Result& seed);
LabReplay labResume(AdaptiveState adaptive, LabCounts fixed,
                    std::array<LabMixtureState, 12> mixtures,
                    std::vector<LabScoreRow> scoreHistory);
LabScoreRow labScore(const LabForecast& frozen, const Result& actual);
void labAppend(LabReplay& replay, const Result& actual);
LabReplay replayLab(const Snapshot& snapshot, std::stop_token stop = {});
struct LabMetric {
    size_t targets = 0;
    int64_t hitSum = 0;
    std::array<size_t, 7> hitHistogram{};
    Loss meanLoss{};
    Calibration calibration{};
    double meanHits() const { return targets ? double(hitSum) / double(targets) : 0; }
    size_t exactly(size_t hits) const { return hits < hitHistogram.size() ? hitHistogram[hits] : 0; }
    size_t atLeast(size_t hits) const {
        size_t count = 0;
        for (; hits < hitHistogram.size(); ++hits) count += hitHistogram[hits];
        return count;
    }
};
struct LabPair {
    size_t targets = 0;
    double meanHitDifference = 0, meanLogDifference = 0, meanBrierDifference = 0;
    std::optional<Interval> hitInterval, logInterval, brierInterval;
};
struct LabEvaluation {
    size_t commonTargets = 0;
    LabMetric selector, adaptive, fixed, uniform;
    std::array<LabMetric, 15> candidates{}; // Exploratory, never selector confirmation.
    LabPair versusAdaptive, versusFixed, versusUniform;
    double fairExpectedMainHits = 36.0 / 49.0;
    std::string evidence = "Inspected development replay; no demonstrated future advantage.";
};
LabEvaluation evaluateLab(const LabReplay& replay);
inline constexpr uint64_t LabControlSeed = 0x466f726563617374ULL;
inline constexpr size_t LabControlRuns = 199;
struct LabControlReport {
    std::string protocolDigest, sourceDigest;
    uint64_t masterSeed = LabControlSeed;
    size_t historyLength = 0, requested = LabControlRuns, completed = 0;
    std::vector<std::string> historyHashes;
    std::vector<double> nullStatistics;
    double observedStatistic = 0, tailFraction = 0, simulationStdError = 0;
    bool complete = false, cancelled = false;
};
LabControlReport labControls(size_t historyLength, double observedStatistic,
                             std::string sourceDigest, size_t maxRuns = LabControlRuns,
                             std::stop_token stop = {},
                             std::function<void(size_t, size_t, const std::string&, double)> progress = {});
}
