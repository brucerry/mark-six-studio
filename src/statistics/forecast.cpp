#include "forecast.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <stdexcept>

namespace marksix::statistics {
namespace {
void cancelled(std::stop_token token) {
    if (token.stop_requested()) throw std::runtime_error("Statistics calculation cancelled");
}
struct Counts {
    size_t n = 0;
    std::array<size_t, 49> main{}, extra{};
    void add(const Result& r) { ++n; for (int v : r.main) ++main[v - 1]; ++extra[r.extra - 1]; }
    Probabilities predict() const {
        Probabilities out{};
        if (n < MinimumTraining) return out;
        for (size_t i = 0; i < 49; ++i) out[i] = {
            (double(main[i]) + PriorDraws * 6 / 49) / (double(n) + PriorDraws),
            (double(extra[i]) + PriorDraws / 49) / (double(n) + PriorDraws),
            (double(n - main[i] - extra[i]) + PriorDraws * 42 / 49) / (double(n) + PriorDraws)};
        return out;
    }
};
Selection rank(const Probabilities& probabilities) {
    std::array<int, 49> order{}; std::iota(order.begin(), order.end(), 1);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return probabilities[a - 1].main == probabilities[b - 1].main ? a < b :
            probabilities[a - 1].main > probabilities[b - 1].main;
    });
    Selection out; std::copy_n(order.begin(), 6, out.main.begin());
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return probabilities[a - 1].extra == probabilities[b - 1].extra ? a < b :
            probabilities[a - 1].extra > probabilities[b - 1].extra;
    });
    for (int n : order) if (std::find(out.main.begin(), out.main.end(), n) == out.main.end()) { out.extra = n; break; }
    return out;
}
std::array<int, 49> outcomes(const Result& r) {
    std::array<int, 49> values{}; // 0=absent, 1=main, 2=Extra.
    for (int n : r.main) values[n - 1] = 1; values[r.extra - 1] = 2; return values;
}
void calibrate(Calibration& bins, const Probabilities& p, const Result& r) {
    const auto actual = outcomes(r);
    for (size_t i = 0; i < 49; ++i) {
        const std::array<double, 3> values{p[i].main, p[i].extra, p[i].any()};
        const std::array<bool, 3> observed{actual[i] == 1, actual[i] == 2, actual[i] != 0};
        for (size_t s = 0; s < 3; ++s) {
            auto& bin = bins[s][std::min(size_t(values[s] * 10), size_t(9))];
            ++bin.samples; bin.predictedSum += values[s]; if (observed[s]) ++bin.observed;
        }
    }
}
void accumulate(Loss& a, const Loss& b) {
    a.brier += b.brier; a.log += b.log; a.mainBrier += b.mainBrier;
    a.extraBrier += b.extraBrier; a.anyBrier += b.anyBrier;
}
void divide(Loss& a, size_t n) {
    if (!n) return;
    a.brier /= double(n); a.log /= double(n); a.mainBrier /= double(n);
    a.extraBrier /= double(n); a.anyBrier /= double(n);
}
uint64_t bounded(std::mt19937_64& generator, uint64_t bound) {
    const uint64_t rejectBelow = (uint64_t(0) - bound) % bound;
    uint64_t value; do { value = generator(); } while (value < rejectBelow);
    return value % bound;
}
double quantile(const std::vector<double>& sorted, double p) {
    const double at = p * double(sorted.size() - 1); const auto low = size_t(at);
    return sorted[low] + (at - double(low)) * (sorted[std::min(low + 1, sorted.size() - 1)] - sorted[low]);
}
}
Forecast forecast(const Snapshot& snapshot) {
    Forecast out; out.snapshotDigest = snapshot.digest(); out.selectionDigest = snapshot.selectionDigest();
    const auto rows = snapshot.select(); Counts counts;
    for (const auto& r : rows) counts.add(r);
    out.trainingDraws = rows.size(); if (!rows.empty()) out.cutoff = rows.back().date;
    out.modelAvailable = rows.size() >= MinimumTraining; out.probabilities = counts.predict();
    if (out.modelAvailable) out.selection = rank(out.probabilities);
    return out;
}
Loss score(const Probabilities& p, const Result& actual) {
    const auto r = canonicalize(actual); const auto y = outcomes(r); Loss loss;
    const auto squared = [](double value) { return value * value; };
    for (size_t i = 0; i < 49; ++i) {
        const auto& q = p[i];
        for (double v : {q.main, q.extra, q.absent}) if (!std::isfinite(v) || v <= 0 || v > 1)
            throw std::invalid_argument("Scoring requires strictly positive finite category probabilities");
        if (std::abs(q.main + q.extra + q.absent - 1) > 1e-12) throw std::invalid_argument("Categories do not sum to one");
        const double m = y[i] == 1 ? 1.0 : 0.0, e = y[i] == 2 ? 1.0 : 0.0;
        const double bm = squared(q.main - m), be = squared(q.extra - e);
        loss.brier += bm + be + squared(q.absent - (1 - m - e));
        loss.log -= std::log(y[i] == 1 ? q.main : y[i] == 2 ? q.extra : q.absent);
        loss.mainBrier += bm; loss.extraBrier += be; loss.anyBrier += squared(q.any() - m - e);
    }
    divide(loss, 49); return loss;
}
std::optional<double> CalibrationBin::meanPrediction() const {
    return samples ? std::optional<double>(predictedSum / double(samples)) : std::nullopt;
}
std::optional<double> CalibrationBin::observedRate() const {
    return samples ? std::optional<double>(double(observed) / double(samples)) : std::nullopt;
}
Comparison bootstrap(const std::vector<double>& differences, std::stop_token stop) {
    cancelled(stop); Comparison out;
    for (double d : differences) if (!std::isfinite(d)) throw std::invalid_argument("Nonfinite bootstrap difference");
    if (differences.empty()) return out;
    out.meanDifference = std::accumulate(differences.begin(), differences.end(), 0.0) / double(differences.size());
    if (differences.size() < MinimumTestDraws) return out;
    // Prefix sums preserve paired draw-level differences; no 49-fold pseudo-replication.
    std::vector<double> prefix(differences.size() + 1, 0);
    std::partial_sum(differences.begin(), differences.end(), prefix.begin() + 1);
    std::mt19937_64 generator(BootstrapSeed); std::vector<double> means; means.reserve(BootstrapReplicates);
    for (size_t b = 0; b < BootstrapReplicates; ++b) {
        cancelled(stop); double sum = 0;
        for (size_t n = 0; n < differences.size(); n += BootstrapBlock) {
            const size_t start = size_t(bounded(generator, differences.size() - BootstrapBlock + 1));
            const size_t length = std::min(BootstrapBlock, differences.size() - n);
            sum += prefix[start + length] - prefix[start];
        }
        means.push_back(sum / double(differences.size()));
    }
    std::sort(means.begin(), means.end()); out.interval = Interval{quantile(means, 0.025), quantile(means, 0.975)};
    return out;
}
Backtest backtest(const Snapshot& snapshot, std::stop_token stop) {
    cancelled(stop); Backtest out; out.snapshotDigest = snapshot.digest(); out.selectionDigest = snapshot.selectionDigest();
    const auto rows = snapshot.select(); out.eligibleDraws = rows.size(); out.excluded = snapshot.records().size() - rows.size();
    out.completenessKnown = snapshot.inventoryVerified();
    for (const auto& r : snapshot.records()) if (r.era == Era::Current49 && !eligible(r)) out.completenessKnown = false;
    Counts counts; const Probabilities uniform{}; std::vector<double> brier, logs;
    for (size_t i = 0; i < rows.size(); ++i) {
        cancelled(stop);
        if (i >= MinimumTraining) {
            ScoredDraw draw; draw.date = rows[i].date; draw.id = rows[i].id;
            draw.trainingThrough = rows[i - 1].date; draw.trainingDraws = i; draw.prediction = counts.predict();
            draw.model = score(draw.prediction, rows[i]); draw.uniform = score(uniform, rows[i]);
            calibrate(out.modelCalibration, draw.prediction, rows[i]); calibrate(out.uniformCalibration, uniform, rows[i]);
            accumulate(out.meanModel, draw.model); accumulate(out.meanUniform, draw.uniform);
            brier.push_back(draw.uniform.brier - draw.model.brier); logs.push_back(draw.uniform.log - draw.model.log);
            out.draws.push_back(std::move(draw));
        }
        counts.add(rows[i]); // Deliberately only after prediction/scoring.
    }
    divide(out.meanModel, out.draws.size()); divide(out.meanUniform, out.draws.size());
    out.brierDifference = bootstrap(brier, stop); out.logDifference = bootstrap(logs, stop);
    if (out.brierDifference.interval) out.evidence = out.brierDifference.interval->low > 0 ?
        Evidence::HistoricalHeldOutImprovement : Evidence::NoDemonstratedAdvantage;
    return out;
}
}
