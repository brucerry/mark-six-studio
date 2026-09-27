#include "forecast_lab.hpp"
#include <algorithm>
#include <stdexcept>

namespace marksix::statistics {
namespace {
void fixedAdd(LabCounts& counts, const Result& result) {
    ++counts.n;
    for (int number : result.main) ++counts.main[size_t(number - 1)];
    ++counts.extra[size_t(result.extra - 1)];
}
Probabilities fixedPredict(const LabCounts& counts) {
    Probabilities out{};
    for (size_t i = 0; i < 49; ++i) {
        const double n = counts.n, prior = PriorDraws;
        out[i] = {(counts.main[i] + prior * 6 / 49) / (n + prior),
                  (counts.extra[i] + prior / 49) / (n + prior),
                  (n - counts.main[i] - counts.extra[i] + prior * 42 / 49) / (n + prior)};
    }
    return out;
}
int hits(const Selection& selection, const Result& actual) {
    int count = 0;
    for (int number : selection.main)
        if (std::find(actual.main.begin(), actual.main.end(), number) != actual.main.end()) ++count;
    return count;
}
LabForecast predict(const LabReplay& replay) {
    LabForecast out;
    out.completedDraws = replay.adaptive.draws;
    out.targetOrdinal = out.completedDraws + 1;
    out.cutoff = replay.adaptive.lastDate;
    out.prefixDigest = replay.adaptive.prefixDigest;
    out.decision = labChoose(replay.scoreHistory, out.completedDraws);
    out.probabilities[0] = Probabilities{};
    out.probabilities[1] = predictAdaptive(replay.adaptive).probabilities;
    out.available[0] = out.available[1] = true;
    if (out.completedDraws >= MinimumTraining) {
        out.probabilities[2] = fixedPredict(replay.fixed);
        out.available[2] = true;
    }
    for (size_t j = 0; j < replay.mixtures.size(); ++j) {
        out.probabilities[j + 3] = labPredict(replay.mixtures[j]);
        out.available[j + 3] = true;
    }
    for (size_t j = 0; j < out.available.size(); ++j)
        if (out.available[j]) out.selections[j] = labRank(out.probabilities[j]);
    return out;
}
}
LabReplay labStart(const Result& input) {
    const auto seed = canonicalize(input);
    LabReplay out;
    out.protocolDigest = labProtocolDigest();
    out.adaptive = seedAdaptive(seed);
    fixedAdd(out.fixed, seed);
    const auto& candidates = labCandidates();
    for (size_t j = 0; j < out.mixtures.size(); ++j)
        out.mixtures[j] = labSeed(candidates[j + 3], seed);
    out.available = true;
    out.next = predict(out);
    return out;
}
LabReplay labResume(AdaptiveState adaptive, LabCounts fixed,
                    std::array<LabMixtureState, 12> mixtures,
                    std::vector<LabScoreRow> scoreHistory) {
    validateAdaptiveState(adaptive);
    if (adaptive.draws != size_t(fixed.n) || adaptive.draws == 0 ||
        scoreHistory.size() > adaptive.draws - 1 || scoreHistory.size() > 300)
        throw std::invalid_argument("Inconsistent Forecast Lab checkpoint");
    const auto& candidates = labCandidates();
    for (size_t j = 0; j < mixtures.size(); ++j)
        if (mixtures[j].candidate.id != candidates[j + 3].id ||
            mixtures[j].draws != adaptive.draws || mixtures[j].lastDate != adaptive.lastDate)
            throw std::invalid_argument("Mismatched Forecast Lab mixture checkpoint");
    LabReplay out;
    out.protocolDigest = labProtocolDigest();
    out.adaptive = std::move(adaptive);
    out.fixed = std::move(fixed);
    out.mixtures = std::move(mixtures);
    out.scoreHistory = std::move(scoreHistory);
    out.available = true;
    out.next = predict(out);
    return out;
}
LabScoreRow labScore(const LabForecast& frozen, const Result& input) {
    const auto actual = canonicalize(input);
    LabScoreRow out;
    out.ordinal = frozen.targetOrdinal;
    for (size_t j = 0; j < out.available.size(); ++j) {
        if (!frozen.available[j]) continue;
        out.available[j] = true;
        out.mainHits[j] = hits(frozen.selections[j], actual);
        out.losses[j] = score(frozen.probabilities[j], actual);
        out.logLoss[j] = out.losses[j].log;
    }
    return out;
}
void labAppend(LabReplay& replay, const Result& input) {
    if (!replay.available || !replay.next) throw std::invalid_argument("Forecast Lab is not initialized");
    const auto actual = canonicalize(input);
    if (!eligible(actual) || actual.date <= replay.adaptive.lastDate)
        throw std::invalid_argument("Forecast Lab target is ineligible or out of order");
    LabStep step;
    step.actual = actual;
    step.frozen = *replay.next;
    step.scores = labScore(step.frozen, actual);
    // Prepare the complete next state before publishing the frozen row.
    auto nextAdaptive = replay.adaptive;
    auto nextMixtures = replay.mixtures;
    auto nextFixed = replay.fixed;
    observeAdaptive(nextAdaptive, actual);
    for (auto& mixture : nextMixtures) labObserve(mixture, actual);
    fixedAdd(nextFixed, actual);
    replay.adaptive = std::move(nextAdaptive);
    replay.mixtures = std::move(nextMixtures);
    replay.fixed = std::move(nextFixed);
    replay.scoreHistory.push_back(step.scores);
    // A held 100-target choice still needs its earlier 200-target validation window.
    if (replay.scoreHistory.size() > 300) replay.scoreHistory.erase(replay.scoreHistory.begin());
    replay.steps.push_back(std::move(step));
    replay.next = predict(replay);
}
LabReplay replayLab(const Snapshot& snapshot, std::stop_token stop) {
    LabReplay out;
    out.protocolDigest = labProtocolDigest();
    out.snapshotDigest = snapshot.digest();
    const auto rows = snapshot.select();
    if (rows.empty() || rows.front().date != "2002-07-04" || rows.front().id != "02/053") {
        out.unavailableReason = "Forecast Lab unavailable: verified seed 02/053 (2002-07-04) required.";
        return out;
    }
    if (stop.stop_requested()) throw std::runtime_error("Forecast Lab replay cancelled");
    out = labStart(rows.front());
    out.snapshotDigest = snapshot.digest();
    out.steps.reserve(rows.size() - 1);
    out.scoreHistory.reserve(rows.size() - 1);
    for (size_t i = 1; i < rows.size(); ++i) {
        if (stop.stop_requested()) throw std::runtime_error("Forecast Lab replay cancelled");
        labAppend(out, rows[i]);
    }
    return out;
}
}
