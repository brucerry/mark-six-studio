#include "forecast_lab.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace marksix::statistics {
namespace {
constexpr double FairMain = 6.0 / 49, FairExtra = 1.0 / 49, FairAbsent = 42.0 / 49;
const std::array<LabCandidate, 15> candidates = [] {
    std::array<LabCandidate, 15> out{};
    out[0] = {"uniform", LabKind::Uniform};
    out[1] = {"adaptive-mixture-v1", LabKind::Adaptive};
    out[2] = {"frequency-shrinkage-v1", LabKind::Fixed};
    size_t next = 3;
    for (int half : {25, 100, 400}) for (int prior : {100, 400}) for (double eta : {0.25, 1.0}) {
        out[next++] = {"mix-h" + std::to_string(half) + "-p" + std::to_string(prior) +
            (eta == 0.25 ? "-e025" : "-e100"), LabKind::Mixture, half, prior, eta};
    }
    return out;
}();
double decay(const LabMixtureState& state) {
    return std::exp(-std::log(2.0) / double(state.candidate.halfLife));
}
void requireMixture(const LabCandidate& candidate) {
    if (candidate.kind != LabKind::Mixture ||
        std::none_of(candidates.begin(), candidates.end(), [&](const auto& c) {
            return c.kind == LabKind::Mixture && c.id == candidate.id &&
                c.halfLife == candidate.halfLife && c.prior == candidate.prior &&
                c.learningRate == candidate.learningRate;
        })) throw std::invalid_argument("Unknown Forecast Lab mixture candidate");
}
void add(LabMixtureState& state, const Result& result) {
    const std::array<double, 2> rho{1.0, decay(state)};
    for (size_t j = 0; j < 2; ++j) {
        auto& c = state.counts[j];
        c.n = rho[j] * c.n + 1.0;
        for (size_t i = 0; i < 49; ++i) { c.main[i] *= rho[j]; c.extra[i] *= rho[j]; }
        for (int n : result.main) c.main[size_t(n - 1)] += 1.0;
        c.extra[size_t(result.extra - 1)] += 1.0;
    }
    ++state.draws;
    state.lastDate = result.date;
    std::string value = "forecast-lab-v1|" + state.prefixDigest + "|" + result.date + "|" + result.id;
    for (int number : result.main) value += "|" + std::to_string(number);
    state.prefixDigest = sha256(value + "|extra=" + std::to_string(result.extra));
}
Probabilities expert(const LabCounts& count, int prior) {
    Probabilities out{};
    for (size_t i = 0; i < 49; ++i) {
        const double denominator = count.n + double(prior);
        out[i] = {(count.main[i] + double(prior) * FairMain) / denominator,
                  (count.extra[i] + double(prior) * FairExtra) / denominator,
                  (count.n - count.main[i] - count.extra[i] + double(prior) * FairAbsent) / denominator};
    }
    return out;
}
std::array<Probabilities, 3> experts(const LabMixtureState& state) {
    return {Probabilities{}, expert(state.counts[0], state.candidate.prior),
            expert(state.counts[1], state.candidate.prior)};
}
std::array<double, 3> weights(const LabMixtureState& state) {
    std::array<double, 3> out{};
    const double high = *std::max_element(state.logWeights.begin(), state.logWeights.end());
    double sum = 0;
    for (size_t j = 0; j < 3; ++j) sum += out[j] = std::exp(state.logWeights[j] - high);
    for (double& value : out) value /= sum;
    return out;
}
}
const std::array<LabCandidate, 15>& labCandidates() { return candidates; }
std::string labProtocolDigest() {
    std::string bytes = "forecast-lab-v1|seed=02/053|experts=uniform,expanding,half-life|"
        "grid=half:25,100,400;prior:100,400;eta:0.25,1|uniform-share=0.5|"
        "loss=mean49-category-log|order=predict,score,weights,counts|ties=main-desc,number-asc|"
        "selection=warmup400,inner200,block100,mean-hits/log/id,adaptive-fallback|"
        "bootstrap=mt19937_64-seed-4d61726b53697831,block20,replicates2000|"
        "controls=199-iid-without-replacement,mt19937_64,rejection-bounded,seed-466f726563617374|"
        "prospective=200";
    for (const auto& candidate : candidates) bytes += "|" + candidate.id;
    return sha256(bytes);
}
Selection labRank(const Probabilities& probabilities) {
    std::array<int, 49> order{};
    std::iota(order.begin(), order.end(), 1);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const double x = probabilities[size_t(a - 1)].main, y = probabilities[size_t(b - 1)].main;
        return x == y ? a < b : x > y;
    });
    Selection out;
    std::copy_n(order.begin(), 6, out.main.begin());
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const double x = probabilities[size_t(a - 1)].extra, y = probabilities[size_t(b - 1)].extra;
        return x == y ? a < b : x > y;
    });
    for (int n : order) if (std::find(out.main.begin(), out.main.end(), n) == out.main.end()) {
        out.extra = n;
        break;
    }
    return out;
}
LabMixtureState labSeed(const LabCandidate& candidate, const Result& input) {
    requireMixture(candidate);
    const auto seed = canonicalize(input);
    if (!eligible(seed) || seed.id != "02/053" || seed.date != "2002-07-04")
        throw std::invalid_argument("Verified Forecast Lab seed required");
    LabMixtureState out;
    out.candidate = candidate;
    out.prefixDigest = labProtocolDigest();
    add(out, seed);
    return out;
}
Probabilities labPredict(const LabMixtureState& state) {
    requireMixture(state.candidate);
    if (!state.draws) throw std::invalid_argument("Empty Forecast Lab state");
    const auto q = experts(state);
    const auto w = weights(state);
    Probabilities out{};
    for (size_t i = 0; i < 49; ++i) {
        out[i] = {0.5 * FairMain, 0.5 * FairExtra, 0.5 * FairAbsent};
        for (size_t j = 0; j < 3; ++j) {
            out[i].main += 0.5 * w[j] * q[j][i].main;
            out[i].extra += 0.5 * w[j] * q[j][i].extra;
            out[i].absent += 0.5 * w[j] * q[j][i].absent;
        }
    }
    return out;
}
void labObserve(LabMixtureState& state, const Result& input) {
    requireMixture(state.candidate);
    const auto actual = canonicalize(input);
    if (!eligible(actual) || actual.date <= state.lastDate)
        throw std::invalid_argument("Forecast Lab observation is ineligible or out of order");
    const auto q = experts(state);
    auto next = state;
    for (size_t j = 0; j < 3; ++j)
        next.logWeights[j] -= state.candidate.learningRate * score(q[j], actual).log;
    const double high = *std::max_element(next.logWeights.begin(), next.logWeights.end());
    for (double& value : next.logWeights) value -= high;
    add(next, actual);
    state = std::move(next);
}
LabDecision labChoose(const std::vector<LabScoreRow>& history, size_t completedDraws) {
    LabDecision out;
    if (completedDraws < 400) return out;
    const size_t boundary = 400 + 100 * ((completedDraws - 400) / 100);
    out.validationFirst = boundary - 199;
    out.validationLast = boundary;
    std::array<int64_t, 15> hits{};
    std::array<double, 15> logs{};
    std::array<size_t, 15> eligibleRows{};
    size_t found = 0;
    for (const auto& row : history) {
        if (row.ordinal < out.validationFirst || row.ordinal > boundary) continue;
        if (row.ordinal != out.validationFirst + found)
            throw std::invalid_argument("Forecast Lab validation history gap or duplicate");
        ++found;
        for (size_t j = 0; j < candidates.size(); ++j) if (row.available[j]) {
            if (row.mainHits[j] < 0 || row.mainHits[j] > 6 || !std::isfinite(row.logLoss[j]))
                throw std::invalid_argument("Invalid Forecast Lab validation score");
            hits[j] += row.mainHits[j];
            logs[j] += row.logLoss[j];
            ++eligibleRows[j];
        }
    }
    if (found != 200 || eligibleRows[1] != 200) return out;
    out.validationTargets = found;
    size_t best = 1;
    for (size_t j = 0; j < candidates.size(); ++j) {
        if (eligibleRows[j] != 200) continue;
        if (hits[j] > hits[best] ||
            (hits[j] == hits[best] && (logs[j] < logs[best] ||
                (logs[j] == logs[best] && candidates[j].id < candidates[best].id)))) best = j;
    }
    if (best == 1 || hits[best] <= hits[1] ||
        double(hits[best]) / 200.0 <= 36.0 / 49.0) {
        out.reason = "Adaptive fallback: no challenger clears the validation rule";
        return out;
    }
    out.candidate = best;
    out.fallback = false;
    out.reason = "Selected by earlier 200-target main hits, then log loss and stable ID";
    return out;
}
}
