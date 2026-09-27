#include "adaptive.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>
#include <stdexcept>

namespace marksix::statistics {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
std::array<double, 4> weights(const AdaptiveState& state) {
    std::array<double, 4> out{};
    const double largest = *std::max_element(state.logWeights.begin(), state.logWeights.end());
    double sum = 0;
    for (size_t j = 0; j < 4; ++j) sum += out[j] = std::exp(state.logWeights[j] - largest);
    for (auto& value : out) value /= sum;
    return out;
}
std::string prefix(const std::string& previous, const Result& r) {
    // No retrieval metadata: same accepted values must not be learned twice.
    std::string bytes = "adaptive-prefix-v1|" + previous + "|" + r.date + "|" + r.id;
    for (int n : r.main) bytes += "|" + std::to_string(n);
    return sha256(bytes + "|extra=" + std::to_string(r.extra));
}
void add(AdaptiveState& state, const Result& r) {
    const std::array<double, 3> decay{1.0, std::exp(-std::log(2.0) / 50.0), std::exp(-std::log(2.0) / 200.0)};
    for (size_t j = 0; j < 3; ++j) {
        auto& c = state.counts[j];
        c.n = decay[j] * c.n + 1;
        for (size_t i = 0; i < 49; ++i) { c.main[i] *= decay[j]; c.extra[i] *= decay[j]; }
        for (int n : r.main) c.main[size_t(n - 1)] += 1;
        c.extra[size_t(r.extra - 1)] += 1;
    }
    ++state.draws; state.lastDate = r.date; state.lastId = r.id;
    state.prefixDigest = prefix(state.prefixDigest, r);
}
Selection rank(const Probabilities& p) {
    std::array<int, 49> order{}; std::iota(order.begin(), order.end(), 1);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return p[size_t(a - 1)].main == p[size_t(b - 1)].main ? a < b : p[size_t(a - 1)].main > p[size_t(b - 1)].main;
    });
    Selection result; std::copy_n(order.begin(), 6, result.main.begin());
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return p[size_t(a - 1)].extra == p[size_t(b - 1)].extra ? a < b : p[size_t(a - 1)].extra > p[size_t(b - 1)].extra;
    });
    for (int n : order) if (std::find(result.main.begin(), result.main.end(), n) == result.main.end()) { result.extra = n; break; }
    return result;
}
}
std::string adaptiveProtocolDigest() { return sha256(AdaptiveProtocol); }
std::string adaptiveValuePrefix(const std::string& previous, const Result& result) {
    return prefix(previous, canonicalize(result));
}
void validateAdaptiveState(const AdaptiveState& state) {
    require(state.draws >= 1 && state.draws <= 100000, "Invalid adaptive draw count");
    require(validDate(state.lastDate) && state.lastDate >= "2002-07-04", "Invalid adaptive cutoff");
    require(state.lastId.size() == 6 && state.lastId[2] == '/' &&
        state.lastId.substr(0, 2) == state.lastDate.substr(2, 2) &&
        state.lastId.substr(3).find_first_not_of("0123456789") == std::string::npos &&
        state.lastId.substr(3) != "000", "Invalid adaptive last identity");
    require(state.prefixDigest.size() == 64 && std::all_of(state.prefixDigest.begin(), state.prefixDigest.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "Invalid adaptive prefix");
    double largest = -std::numeric_limits<double>::infinity();
    for (double a : state.logWeights) {
        require(std::isfinite(a) && a <= 1e-12 && a >= -1e8, "Invalid adaptive log weight");
        largest = std::max(largest, a);
    }
    require(std::abs(largest) <= 1e-12, "Adaptive weights not normalized");
    for (size_t j = 0; j < 3; ++j) {
        const auto& c = state.counts[j];
        const double rho = j == 0 ? 1 : std::exp(-std::log(2.0) / (j == 1 ? 50.0 : 200.0));
        const double expectedN = j == 0 ? double(state.draws) : -std::expm1(double(state.draws) * std::log(rho)) / (1 - rho);
        require(std::isfinite(c.n) && std::abs(c.n - expectedN) <= 1e-8 * expectedN, "Invalid adaptive effective count");
        double main = 0, extra = 0;
        for (size_t i = 0; i < 49; ++i) {
            require(std::isfinite(c.main[i]) && std::isfinite(c.extra[i]) && c.main[i] >= 0 && c.extra[i] >= 0 &&
                c.main[i] + c.extra[i] <= c.n + 1e-10 * c.n, "Invalid adaptive category count");
            main += c.main[i]; extra += c.extra[i];
        }
        require(std::abs(main - 6 * c.n) <= 1e-9 * c.n && std::abs(extra - c.n) <= 1e-9 * c.n,
            "Invalid adaptive count totals");
    }
}
AdaptiveState seedAdaptive(const Result& input) {
    const auto seed = canonicalize(input);
    require(eligible(seed) && seed.date == "2002-07-04" && seed.id == "02/053", "Verified adaptive seed unavailable");
    AdaptiveState state; state.prefixDigest = adaptiveProtocolDigest(); add(state, seed);
    validateAdaptiveState(state); return state;
}
AdaptivePrediction predictAdaptive(const AdaptiveState& state) {
    validateAdaptiveState(state);
    AdaptivePrediction out; out.weights = weights(state); out.trainingDraws = state.draws;
    out.warmup = state.draws < 200; out.cutoff = state.lastDate; out.prefixDigest = state.prefixDigest;
    for (size_t j = 0; j < 3; ++j) {
        const auto& c = state.counts[j];
        for (size_t i = 0; i < 49; ++i) out.experts[j + 1][i] = {
            (c.main[i] + 100.0 * 6 / 49) / (c.n + 100),
            (c.extra[i] + 100.0 / 49) / (c.n + 100),
            (c.n - c.main[i] - c.extra[i] + 100.0 * 42 / 49) / (c.n + 100)};
    }
    for (size_t i = 0; i < 49; ++i) {
        Categories q{0.5 * 6 / 49, 0.5 / 49, 0.5 * 42 / 49};
        for (size_t j = 0; j < 4; ++j) {
            const auto& p = out.experts[j][i]; const auto w = 0.5 * out.weights[j];
            q.main += w * p.main; q.extra += w * p.extra; q.absent += w * p.absent;
        }
        out.probabilities[i] = q;
    }
    out.selection = rank(out.probabilities); return out;
}
AdaptiveStep observeAdaptive(AdaptiveState& state, const Result& input) {
    validateAdaptiveState(state); const auto actual = canonicalize(input);
    require(eligible(actual), "Ineligible adaptive observation");
    // Same-date records lack source-supported extraction order; fail closed.
    require(actual.date > state.lastDate, "Ambiguous or non-increasing adaptive order");
    require(actual.id != state.lastId && state.draws < 100000, "Duplicate or excessive adaptive observation");
    AdaptiveStep out; out.actual = actual; out.before = state; out.prediction = predictAdaptive(state);
    out.adaptiveLoss = score(out.prediction.probabilities, actual); out.uniformLoss = score(Probabilities{}, actual);
    auto next = state;
    for (size_t j = 0; j < 4; ++j) {
        out.expertLogLoss[j] = score(out.prediction.experts[j], actual).log;
        next.logWeights[j] -= out.expertLogLoss[j];
    }
    const double largest = *std::max_element(next.logWeights.begin(), next.logWeights.end());
    for (auto& a : next.logWeights) a -= largest;
    add(next, actual); validateAdaptiveState(next);
    out.weightsAfter = weights(next); out.after = next; state = std::move(next); return out;
}
AdaptiveReplay replayAdaptive(const Snapshot& snapshot, std::stop_token stop) {
    AdaptiveReplay out; out.protocolDigest = adaptiveProtocolDigest(); out.snapshotDigest = snapshot.digest();
    out.completenessKnown = snapshot.inventoryVerified(); const auto rows = snapshot.select();
    out.excluded = snapshot.records().size() - rows.size();
    for (const auto& r : snapshot.records()) if (r.era == Era::Current49 && !eligible(r)) out.completenessKnown = false;
    const auto cancel = [&] { if (stop.stop_requested()) throw std::runtime_error("Adaptive calculation cancelled"); };
    cancel();
    if (rows.empty() || rows.front().date != "2002-07-04" || rows.front().id != "02/053") {
        out.unavailableReason = "Adaptive unavailable: verified seed 02/053 (2002-07-04) required."; return out;
    }
    out.state = seedAdaptive(rows.front()); out.steps.reserve(rows.size() - 1);
    for (size_t i = 1; i < rows.size(); ++i) { cancel(); out.steps.push_back(observeAdaptive(out.state, rows[i])); }
    cancel(); out.next = predictAdaptive(out.state); out.available = true; return out;
}
namespace {
void collect(AdaptiveDiagnostics& out, const Probabilities& p, const Loss& loss, const Result& actual) {
    ++out.draws;
    const auto selected = rank(p);
    for (int n : selected.main) if (std::find(actual.main.begin(), actual.main.end(), n) != actual.main.end()) ++out.mainHits;
    if (selected.extra == actual.extra) ++out.extraMatches;
    out.mean.brier += loss.brier; out.mean.log += loss.log; out.mean.mainBrier += loss.mainBrier;
    out.mean.extraBrier += loss.extraBrier; out.mean.anyBrier += loss.anyBrier;
    for (size_t i = 0; i < 49; ++i) {
        const bool main = std::find(actual.main.begin(), actual.main.end(), int(i + 1)) != actual.main.end();
        const bool extra = actual.extra == int(i + 1);
        const std::array<double, 3> values{p[i].main, p[i].extra, p[i].any()};
        const std::array<bool, 3> observed{main, extra, main || extra};
        for (size_t s = 0; s < 3; ++s) {
            auto& bin = out.calibration[s][std::min(size_t(values[s]*10), size_t(9))];
            ++bin.samples; bin.predictedSum += values[s]; if (observed[s]) ++bin.observed;
        }
    }
}
void finish(AdaptiveDiagnostics& out) {
    if (!out.draws) return;
    const double n = double(out.draws);
    out.mean.brier /= n; out.mean.log /= n; out.mean.mainBrier /= n;
    out.mean.extraBrier /= n; out.mean.anyBrier /= n;
}
void target(AdaptiveWindow& window, const Result& r) {
    if (window.firstId.empty()) { window.firstId = r.id; window.firstDate = r.date; }
    window.lastId = r.id; window.lastDate = r.date;
}
}
AdaptiveEvaluation evaluateAdaptive(const AdaptiveReplay& replay, const Backtest& fixed, std::stop_token stop) {
    require(replay.snapshotDigest == fixed.snapshotDigest, "Adaptive comparison snapshot mismatch");
    AdaptiveEvaluation out;
    if (stop.stop_requested()) throw std::runtime_error("Adaptive evaluation cancelled");
    if (!replay.available) return out;
    const size_t expectedFixed = replay.state.draws > MinimumTraining ? replay.state.draws - MinimumTraining : 0;
    require(fixed.draws.size() == expectedFixed, "Adaptive common target count mismatch");
    std::vector<double> allLog, allBrier, commonLog, commonBrier, fixedLog, fixedBrier;
    size_t at = 0;
    for (const auto& step : replay.steps) {
        if (stop.stop_requested()) throw std::runtime_error("Adaptive evaluation cancelled");
        target(out.all, step.actual);
        collect(out.all.adaptive, step.prediction.probabilities, step.adaptiveLoss, step.actual);
        collect(out.all.uniform, Probabilities{}, step.uniformLoss, step.actual);
        allLog.push_back(step.uniformLoss.log - step.adaptiveLoss.log);
        allBrier.push_back(step.uniformLoss.brier - step.adaptiveLoss.brier);
        if (step.prediction.trainingDraws < MinimumTraining) continue;
        const auto& f = fixed.draws.at(at++);
        require(f.id == step.actual.id && f.date == step.actual.date && f.trainingDraws == step.prediction.trainingDraws &&
            f.trainingThrough == step.prediction.cutoff, "Adaptive common target identity mismatch");
        target(out.common, step.actual);
        collect(out.common.adaptive, step.prediction.probabilities, step.adaptiveLoss, step.actual);
        collect(out.common.uniform, Probabilities{}, step.uniformLoss, step.actual);
        collect(out.common.fixed, f.prediction, f.model, step.actual);
        commonLog.push_back(step.uniformLoss.log - step.adaptiveLoss.log);
        commonBrier.push_back(step.uniformLoss.brier - step.adaptiveLoss.brier);
        fixedLog.push_back(f.model.log - step.adaptiveLoss.log);
        fixedBrier.push_back(f.model.brier - step.adaptiveLoss.brier);
    }
    for (auto* window : {&out.all, &out.common}) {
        finish(window->adaptive); finish(window->uniform); finish(window->fixed);
    }
    out.all.uniformMinusAdaptiveLog = bootstrap(allLog, stop);
    out.all.uniformMinusAdaptiveBrier = bootstrap(allBrier, stop);
    out.common.uniformMinusAdaptiveLog = bootstrap(commonLog, stop);
    out.common.uniformMinusAdaptiveBrier = bootstrap(commonBrier, stop);
    out.common.fixedMinusAdaptiveLog = bootstrap(fixedLog, stop);
    out.common.fixedMinusAdaptiveBrier = bootstrap(fixedBrier, stop);
    return out;
}
}
