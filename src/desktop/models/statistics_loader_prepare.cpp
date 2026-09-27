#include "statistics_loader.hpp"

#include "analysis.hpp"
#include "archive.hpp"
#include "coverage.hpp"
#include "forecast.hpp"
#include "forecast_lab_ledger.hpp"
#include "learning_progress.hpp"

#include <algorithm>
#include <stdexcept>

using namespace marksix::statistics;

PreparedStatistics StatisticsLoader::prepare(const std::filesystem::path& root,
                                             AdaptiveCoordinator& coordinator,
                                             std::stop_token stop, ViewOptions options) {
    Archive archive(root);
    auto state = archive.readState();
    if (stop.stop_requested()) throw std::runtime_error("Statistics load cancelled");
    ViewData data;
    data.snapshot = std::make_shared<const Snapshot>(std::move(state.snapshot));
    data.historyFrom = options.from;
    data.historyThrough = options.through;
    data.coverage = coverage(*data.snapshot);
    data.revisions = std::move(state.revisions);
    data.updateAttempts = std::move(state.attempts);
    data.resolutions = std::move(state.resolutions);
    data.archiveVersion = state.version;
    const Scope scope = options.scope == 1 ? Scope::Extra : options.scope == 2 ? Scope::Any : Scope::Main;
    data.analysis = analyze(*data.snapshot, scope,
        Filter{options.from, options.through, options.lastN}, 100, stop);
    data.forecast = forecast(*data.snapshot);
    data.backtest = backtest(*data.snapshot, stop);
    auto adaptive = coordinator.reconcile(root, stop).get();
    if (adaptive.result.snapshotDigest != data.snapshot->digest()) {
        adaptive.result.current.reset();
        if (adaptive.error.empty()) adaptive.error = "Source changed; reload before using adaptive output.";
    }
    if (!adaptive.lineages.empty()) {
        const auto selected = std::find_if(adaptive.lineages.begin(), adaptive.lineages.end(),
            [&](const auto& lineage) { return lineage.id == options.learningLineage; });
        const auto& lineage = selected == adaptive.lineages.end() ? adaptive.lineages.front() : *selected;
        adaptive.historyLineage = lineage.id;
        adaptive.historyOffset = std::min(options.learningOffset,
            lineage.predictionCount > 100 ? lineage.predictionCount - 100 : size_t(0));
        adaptive.history = coordinator.history(root, lineage.id, adaptive.historyOffset, stop).get();
        if (adaptive.result.current && lineage.id != adaptive.result.current->lineage.id) {
            AdaptiveLedger ledger(root);
            adaptive.progress = readLearningProgress(ledger, lineage.id, stop);
        }
    }
    data.adaptive = std::make_shared<const AdaptiveWorkspace>(std::move(adaptive));
    auto lab = coordinator.lab(root, stop).get();
    if (lab.result.available && lab.result.replay.snapshotDigest != data.snapshot->digest()) {
        lab.result.available = false;
        lab.error = "Source changed before Forecast Lab publication; reload.";
    }
    data.lab = std::make_shared<const LabWorkspace>(std::move(lab));
    if (data.lab->result.available) {
        LabLedger ledger(root);
        data.labControls = ledger.control(data.snapshot->digest(), data.snapshot->select().size());
    }
    if (stop.stop_requested()) throw std::runtime_error("Statistics load cancelled");
    PreparedStatistics result;
    for (const auto& record : data.snapshot->records()) {
        const size_t eraIndex = record.era == Era::Current49 ? 0 : 1;
        auto& first = result.eraFirstDates[eraIndex];
        auto& last = result.eraLastDates[eraIndex];
        if (first.empty() || record.date < first) {
            first = record.date;
        }
        if (last.empty() || record.date > last) {
            last = record.date;
        }
    }
    const Era era = options.era == 1 ? Era::EarlierUnreviewed : Era::Current49;
    result.archive = present(data, Page::History, options.selectedNumber, era);
    result.sources = present(data, Page::Sources, options.selectedNumber, era);
    result.sourceVersion = data.archiveVersion;
    result.analysis = present(data, Page::Numbers);
    result.pairs = present(data, Page::Pairs);
    result.rolling = present(data, Page::Rolling, options.selectedNumber);
    result.forecastLab = present(data, Page::Lab);
    result.learning = present(data, Page::Learning);
    result.progress = present(data, Page::Progress, 1, Era::Current49,
        options.progressEvidence, options.progressMode);
    result.adaptive = present(data, Page::Adaptive);
    result.fixed = present(data, Page::Forecasts);
    result.exact = present(data, Page::Probability);
    result.calibration = present(data, Page::Calibration);
    result.labOffset = data.lab ? data.lab->historyOffset : 0;
    result.learningOffset = data.adaptive ? data.adaptive->historyOffset : 0;
    if (data.adaptive) {
        for (size_t index = 0; index < data.adaptive->lineages.size(); ++index) {
            const auto& lineage = data.adaptive->lineages[index];
            result.learningLineages.push_back("Version " + std::to_string(lineage.id) + " · " +
                lineage.createdUtc + " · " + lineage.reason);
            result.learningLineageIds.push_back(lineage.id);
            if (lineage.id == data.adaptive->historyLineage) {
                result.learningLineageIndex = int(index);
                result.learningCount = lineage.predictionCount;
            }
        }
        result.learningOffset = data.adaptive->historyOffset;
    }
    result.sourceStatus = std::to_string(data.coverage.total) + " archived | " +
        std::to_string(data.coverage.eligibleCurrent49) + " eligible current-49 | " +
        (data.coverage.latestRetrievalUtc.empty() ? "never retrieved" : "last retrieval " + data.coverage.latestRetrievalUtc) +
        " | completeness " + (data.coverage.complete ? "verified within inventory" : "unknown/incomplete");
    result.evidence = "No demonstrated predictive advantage. Experimental selection, not winning confidence.";
    if (data.adaptive && data.adaptive->result.current) {
        const auto prediction = predictAdaptive(data.adaptive->result.current->state);
        result.adaptiveMain = prediction.selection.main;
        result.adaptiveExtra = prediction.selection.extra;
        result.adaptiveCutoff = prediction.cutoff;
    }
    if (data.forecast.selection) {
        result.fixedMain = data.forecast.selection->main;
        result.fixedExtra = data.forecast.selection->extra;
        result.fixedCutoff = data.forecast.cutoff;
    }
    result.comparisonSummary = "Forecast Lab comparison unavailable until compatible history is loaded.";
    result.progressSummary = "Learning progress unavailable until compatible scored history is loaded.";
    if (data.adaptive && !data.adaptive->progress.evidence[0].points.empty()) {
        const auto& points = data.adaptive->progress.evidence[0].points;
        const auto begin = points.size() > 100 ? points.size() - 100 : 0;
        for (size_t index = begin; index < points.size(); ++index)
            result.progressPoints.push_back(points[index].cumulativeGain[0]);
        result.progressSummary = "Historical replay · " + std::to_string(points.size()) +
            " scored · cumulative mean log gain (uniform minus adaptive): " +
            std::to_string(points.back().cumulativeGain[0]) +
            ". Positive favors adaptive; negative favors uniform. Descriptive, not next-draw confidence.";
    }
    if (data.lab && data.lab->result.available && data.lab->result.replay.next) {
        const auto& next = *data.lab->result.replay.next;
        const auto choice = next.decision.candidate;
        result.suggestedMain = next.selections[choice].main;
        result.selectionAvailable = true;
        result.selectedModel = labCandidates()[choice].id +
            (next.decision.fallback ? " (Adaptive fallback)" : " (inner-selected)");
        result.cutoff = next.cutoff;
        result.selectionState = next.completedDraws < 400 ? "Warm-up / limited validation" : "Retrospective development replay";
        const auto& evaluation = data.lab->evaluation;
        result.comparisonSummary = "Inspected development targets " + std::to_string(evaluation.commonTargets) +
            " | selected mean main hits " + std::to_string(evaluation.selector.meanHits()) +
            " | Adaptive " + std::to_string(evaluation.adaptive.meanHits()) +
            " | fixed " + std::to_string(evaluation.fixed.meanHits()) +
            " | fair expectation " + std::to_string(evaluation.fairExpectedMainHits) +
            ". Same inspected history, not a fresh holdout or winning confidence.";
        result.labProbabilities.columns = {"Number", "Selected main", "Fair main", "Not main", "Selected Extra", "Fair Extra", "Any seven", "Absent"};
        const auto& probabilities = next.probabilities[choice];
        for (size_t index = 0; index < 49; ++index) {
            const auto& q = probabilities[index];
            const auto cell = [](double value) { return Cell{percent(value), value}; };
            result.labProbabilities.rows.push_back({Cell{std::to_string(index + 1), double(index + 1)},
                cell(q.main), cell(6.0 / 49), cell(1 - q.main), cell(q.extra),
                cell(1.0 / 49), cell(q.any()), cell(q.absent)});
            result.labProbabilities.identities.push_back(index);
        }
        result.labProbabilities.summary = "Forecast Lab selected candidate marginals; six-main inclusion, not extraction position or set confidence.";
        result.controlEligible = evaluation.commonTargets > 0;
        result.controlDigest = data.snapshot->digest();
        result.controlHistoryLength = data.snapshot->select().size();
        result.controlObservedStatistic = evaluation.selector.meanHits() - 36.0 / 49.0;
        if (data.labControls) {
            result.controlSummary = "Saved fair-history controls " + std::to_string(data.labControls->completed) +
                "/" + std::to_string(data.labControls->requested) +
                "; Monte Carlo right-tail " + std::to_string(data.labControls->tailFraction) +
                " ± simulation SE " + std::to_string(data.labControls->simulationStdError) +
                ". Not next-draw confidence.";
        }
    } else if (data.lab) {
        result.selectionState = data.lab->error.empty() ? data.lab->result.error : data.lab->error;
        if (result.selectionState.empty()) result.selectionState = "No compatible Forecast Lab result";
    }
    return result;
}
