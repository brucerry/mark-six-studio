#include "forecast_lab.hpp"
#include <algorithm>
#include <stdexcept>

namespace marksix::statistics {
namespace {
void addLoss(Loss& sum, const Loss& value) {
    sum.brier += value.brier; sum.log += value.log;
    sum.mainBrier += value.mainBrier; sum.extraBrier += value.extraBrier;
    sum.anyBrier += value.anyBrier;
}
void divideLoss(Loss& value, size_t n) {
    if (!n) return;
    value.brier /= n; value.log /= n; value.mainBrier /= n;
    value.extraBrier /= n; value.anyBrier /= n;
}
void calibrate(Calibration& bins, const Probabilities& probabilities, const Result& actual) {
    for (size_t i = 0; i < 49; ++i) {
        const int number = int(i + 1);
        const bool main = std::find(actual.main.begin(), actual.main.end(), number) != actual.main.end();
        const bool extra = actual.extra == number;
        const auto& p = probabilities[i];
        const std::array<double, 3> q{p.main, p.extra, p.any()};
        const std::array<bool, 3> y{main, extra, main || extra};
        for (size_t category = 0; category < 3; ++category) {
            auto& bin = bins[category][std::min(size_t(q[category] * 10), size_t(9))];
            ++bin.samples;
            bin.predictedSum += q[category];
            if (y[category]) ++bin.observed;
        }
    }
}
void add(LabMetric& metric, int hits, const Loss& loss, const Probabilities& p, const Result& actual) {
    if (hits < 0 || hits > 6) throw std::invalid_argument("Invalid Forecast Lab hit count");
    ++metric.targets;
    metric.hitSum += hits;
    ++metric.hitHistogram[size_t(hits)];
    addLoss(metric.meanLoss, loss);
    calibrate(metric.calibration, p, actual);
}
void pair(LabPair& result, const LabMetric& selector, const LabMetric& reference,
          const std::vector<std::array<double, 3>>& differences) {
    if (selector.targets != reference.targets) throw std::invalid_argument("Unpaired Forecast Lab targets");
    result.targets = selector.targets;
    if (!result.targets) return;
    result.meanHitDifference = selector.meanHits() - reference.meanHits();
    result.meanLogDifference = reference.meanLoss.log - selector.meanLoss.log;
    result.meanBrierDifference = reference.meanLoss.brier - selector.meanLoss.brier;
    if (differences.size() != result.targets) throw std::invalid_argument("Missing Forecast Lab pair differences");
    if (result.targets < MinimumTestDraws) return;
    for (size_t category = 0; category < 3; ++category) {
        std::vector<double> series;
        series.reserve(differences.size());
        for (const auto& row : differences) series.push_back(row[category]);
        const auto interval = bootstrap(series).interval;
        if (category == 0) result.hitInterval = interval;
        else if (category == 1) result.logInterval = interval;
        else result.brierInterval = interval;
    }
}
}
LabEvaluation evaluateLab(const LabReplay& replay) {
    LabEvaluation out;
    if (!replay.available) return out;
    std::array<std::vector<std::array<double, 3>>, 3> paired;
    for (const auto& step : replay.steps) {
        if (step.frozen.completedDraws < 400) continue;
        const size_t selected = step.frozen.decision.candidate;
        if (selected >= 15 || !step.frozen.available[selected] || !step.frozen.available[2])
            throw std::invalid_argument("Incomplete Forecast Lab outer target");
        const auto& scores = step.scores;
        const auto& frozen = step.frozen;
        const auto record = [&](LabMetric& metric, size_t j) {
            if (!scores.available[j]) throw std::invalid_argument("Missing paired candidate score");
            add(metric, scores.mainHits[j], scores.losses[j], frozen.probabilities[j], step.actual);
        };
        record(out.selector, selected);
        record(out.adaptive, 1);
        record(out.fixed, 2);
        record(out.uniform, 0);
        for (size_t j = 0; j < 15; ++j) record(out.candidates[j], j);
        for (size_t comparison = 0; comparison < 3; ++comparison) {
            const size_t reference = comparison == 0 ? 1 : comparison == 1 ? 2 : 0;
            paired[comparison].push_back({double(scores.mainHits[selected] - scores.mainHits[reference]),
                scores.losses[reference].log - scores.losses[selected].log,
                scores.losses[reference].brier - scores.losses[selected].brier});
        }
    }
    out.commonTargets = out.selector.targets;
    divideLoss(out.selector.meanLoss, out.commonTargets);
    divideLoss(out.adaptive.meanLoss, out.commonTargets);
    divideLoss(out.fixed.meanLoss, out.commonTargets);
    divideLoss(out.uniform.meanLoss, out.commonTargets);
    for (auto& candidate : out.candidates) divideLoss(candidate.meanLoss, out.commonTargets);
    pair(out.versusAdaptive, out.selector, out.adaptive, paired[0]);
    pair(out.versusFixed, out.selector, out.fixed, paired[1]);
    pair(out.versusUniform, out.selector, out.uniform, paired[2]);
    return out;
}
}
