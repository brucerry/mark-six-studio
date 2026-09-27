#include "forecast_lab_storage.hpp"
#include "adaptive_ledger.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace marksix::statistics {
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Writer {
    std::string data;
    void integer(uint64_t value) { for (unsigned i = 0; i < 8; ++i) data += char(value >> (8*i)); }
    void real(double value) { require(std::isfinite(value), "Nonfinite Lab value"); integer(std::bit_cast<uint64_t>(value)); }
    void text(std::string_view value) { require(value.size() <= 4'000'000, "Oversized Lab text"); integer(value.size()); data += value; }
};
struct Reader {
    std::string_view data; size_t at = 0;
    explicit Reader(std::string_view input) : data(input) { require(input.size() <= 4'000'000, "Oversized Lab blob"); }
    uint64_t integer() {
        require(at <= data.size() && data.size()-at >= 8, "Truncated Lab blob");
        uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) value |= uint64_t(uint8_t(data[at++])) << (8*i);
        return value;
    }
    double real() { const double value = std::bit_cast<double>(integer()); require(std::isfinite(value), "Nonfinite stored Lab value"); return value; }
    std::string text(size_t limit = 2048) {
        const auto n = integer(); require(n <= limit && n <= data.size()-at, "Invalid Lab string length");
        std::string out(data.substr(at, size_t(n))); at += size_t(n); return out;
    }
    void end() const { require(at == data.size(), "Trailing Lab bytes"); }
};
size_t size(Reader& r, size_t max) {
    const auto value = r.integer(); require(value <= max, "Invalid Lab count"); return size_t(value);
}
void counts(Writer& w, const LabCounts& c) {
    w.real(c.n);
    for (double v : c.main) w.real(v);
    for (double v : c.extra) w.real(v);
}
LabCounts counts(Reader& r) {
    LabCounts out; out.n = r.real();
    for (double& v : out.main) v = r.real();
    for (double& v : out.extra) v = r.real();
    return out;
}
void probabilities(Writer& w, const Probabilities& p) {
    for (const auto& v : p) { w.real(v.main); w.real(v.extra); w.real(v.absent); }
}
Probabilities probabilities(Reader& r) {
    Probabilities out;
    for (auto& v : out) { v.main = r.real(); v.extra = r.real(); v.absent = r.real(); }
    return out;
}
}
std::string encodeLabCheckpoint(const LabReplay& replay) {
    require(replay.available && replay.next && replay.protocolDigest == labProtocolDigest(),
            "Invalid Lab checkpoint source");
    Writer w; w.text("forecast-lab-checkpoint-v1"); w.text(replay.protocolDigest);
    w.text(encodeAdaptiveState(replay.adaptive)); counts(w, replay.fixed);
    for (const auto& mix : replay.mixtures) {
        w.text(mix.candidate.id); w.integer(mix.draws); w.text(mix.lastDate); w.text(mix.prefixDigest);
        for (const auto& c : mix.counts) counts(w, c);
        for (double value : mix.logWeights) w.real(value);
    }
    w.integer(replay.scoreHistory.size());
    for (const auto& row : replay.scoreHistory) {
        w.integer(row.ordinal);
        for (size_t j = 0; j < 15; ++j) {
            w.integer(row.available[j] ? 1 : 0);
            w.integer(uint64_t(row.mainHits[j]));
            w.real(row.logLoss[j]);
        }
    }
    return w.data;
}
LabReplay decodeLabCheckpoint(std::string_view bytes) {
    Reader r(bytes); require(r.text() == "forecast-lab-checkpoint-v1", "Unknown Lab checkpoint version");
    require(r.text() == labProtocolDigest(), "Lab checkpoint protocol mismatch");
    const auto adaptive = decodeAdaptiveState(r.text(32768));
    const auto fixed = counts(r);
    std::array<LabMixtureState, 12> mixtures{};
    for (size_t j = 0; j < mixtures.size(); ++j) {
        auto& mix = mixtures[j];
        require(r.text() == labCandidates()[j+3].id, "Lab checkpoint candidate mismatch");
        mix.candidate = labCandidates()[j+3];
        mix.draws = size(r, 100000);
        mix.lastDate = r.text(); mix.prefixDigest = r.text();
        for (auto& c : mix.counts) c = counts(r);
        for (double& value : mix.logWeights) value = r.real();
    }
    std::vector<LabScoreRow> history;
    const size_t count = size(r, 300); history.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        LabScoreRow row; row.ordinal = size(r, 100000);
        for (size_t j = 0; j < 15; ++j) {
            row.available[j] = size(r, 1) != 0;
            row.mainHits[j] = int(size(r, 6));
            row.logLoss[j] = r.real();
        }
        if (i) require(row.ordinal == history.back().ordinal + 1, "Lab checkpoint history gap");
        history.push_back(row);
    }
    r.end();
    if (!history.empty()) require(history.back().ordinal == adaptive.draws, "Lab checkpoint tail mismatch");
    return labResume(adaptive, fixed, mixtures, std::move(history));
}
std::string encodeLabForecast(const LabForecast& forecast) {
    Writer w; w.text("forecast-lab-forecast-v1");
    w.integer(forecast.targetOrdinal); w.integer(forecast.completedDraws);
    w.text(forecast.cutoff); w.text(forecast.prefixDigest);
    const auto& decision = forecast.decision;
    w.integer(decision.candidate); w.integer(decision.validationFirst);
    w.integer(decision.validationLast); w.integer(decision.validationTargets);
    w.integer(decision.fallback ? 1 : 0); w.text(decision.reason);
    for (size_t j = 0; j < 15; ++j) {
        w.integer(forecast.available[j] ? 1 : 0);
        if (!forecast.available[j]) continue;
        probabilities(w, forecast.probabilities[j]);
        for (int n : forecast.selections[j].main) w.integer(uint64_t(n));
        w.integer(uint64_t(forecast.selections[j].extra));
    }
    return w.data;
}
LabForecast decodeLabForecast(std::string_view bytes) {
    Reader r(bytes); require(r.text() == "forecast-lab-forecast-v1", "Unknown Lab forecast version");
    LabForecast out;
    out.targetOrdinal = size(r, 100001); out.completedDraws = size(r, 100000);
    require(out.targetOrdinal == out.completedDraws + 1, "Lab target count mismatch");
    out.cutoff = r.text(); out.prefixDigest = r.text();
    out.decision.candidate = size(r, 14);
    out.decision.validationFirst = size(r, 100000);
    out.decision.validationLast = size(r, 100000);
    out.decision.validationTargets = size(r, 200);
    out.decision.fallback = size(r, 1) != 0;
    out.decision.reason = r.text();
    for (size_t j = 0; j < 15; ++j) {
        out.available[j] = size(r, 1) != 0;
        if (!out.available[j]) continue;
        out.probabilities[j] = probabilities(r);
        for (int& n : out.selections[j].main) n = int(size(r, 49));
        out.selections[j].extra = int(size(r, 49));
    }
    r.end();
    require(out.available[out.decision.candidate], "Unavailable selected Lab candidate");
    return out;
}
std::string encodeLabResult(const Result& input) {
    const auto result = canonicalize(input);
    Writer w; w.text("forecast-lab-result-v1"); w.text(result.id); w.text(result.date);
    for (int number : result.main) w.integer(uint64_t(number));
    w.integer(uint64_t(result.extra)); w.integer(uint64_t(result.era));
    const auto& s = result.source;
    for (const auto* value : {&s.sourceId, &s.url, &s.retrievedUtc, &s.contentSha256,
                              &s.lineage, &s.verificationEvidence}) w.text(*value);
    w.integer(uint64_t(s.trust)); w.integer(result.unresolvedConflict ? 1 : 0);
    w.integer(result.extractionOrder ? 1 : 0);
    if (result.extractionOrder) for (int number : *result.extractionOrder) w.integer(uint64_t(number));
    w.text(result.orderEvidence); return w.data;
}
Result decodeLabResult(std::string_view bytes) {
    Reader r(bytes); require(r.text() == "forecast-lab-result-v1", "Unknown Lab result version");
    Result out; out.id = r.text(); out.date = r.text();
    for (int& number : out.main) number = int(size(r, 49));
    out.extra = int(size(r, 49)); out.era = Era(size(r, 1));
    auto& s = out.source;
    s.sourceId = r.text(); s.url = r.text(); s.retrievedUtc = r.text();
    s.contentSha256 = r.text(); s.lineage = r.text(); s.verificationEvidence = r.text();
    s.trust = Trust(size(r, 2)); out.unresolvedConflict = size(r, 1) != 0;
    if (size(r, 1)) {
        std::array<int, 6> order{};
        for (int& number : order) number = int(size(r, 49));
        out.extractionOrder = order;
    }
    out.orderEvidence = r.text(); r.end(); return canonicalize(out);
}
std::string encodeLabControl(const LabControlReport& report) {
    require(report.complete && report.completed == LabControlRuns &&
            report.historyHashes.size() == LabControlRuns &&
            report.nullStatistics.size() == LabControlRuns, "Incomplete Lab control cannot be cached");
    Writer w; w.text("forecast-lab-controls-v1"); w.text(report.protocolDigest);
    w.text(report.sourceDigest); w.integer(report.masterSeed); w.integer(report.historyLength);
    w.real(report.observedStatistic);
    for (size_t i = 0; i < LabControlRuns; ++i) {
        w.text(report.historyHashes[i]); w.real(report.nullStatistics[i]);
    }
    w.real(report.tailFraction); w.real(report.simulationStdError);
    return w.data;
}
LabControlReport decodeLabControl(std::string_view bytes) {
    Reader r(bytes); require(r.text() == "forecast-lab-controls-v1", "Unknown Lab control version");
    LabControlReport out; out.protocolDigest = r.text(); out.sourceDigest = r.text();
    out.masterSeed = r.integer(); out.historyLength = size(r, 100000);
    out.observedStatistic = r.real();
    for (size_t i = 0; i < LabControlRuns; ++i) {
        out.historyHashes.push_back(r.text()); out.nullStatistics.push_back(r.real());
    }
    out.tailFraction = r.real(); out.simulationStdError = r.real();
    out.completed = LabControlRuns; out.complete = true; r.end();
    require(out.protocolDigest == labProtocolDigest() && out.masterSeed == LabControlSeed,
            "Lab control protocol mismatch");
    return out;
}
}
