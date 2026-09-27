#include "presentation.hpp"
#include "update_worker.hpp"
#include <algorithm>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace marksix::statistics {
namespace {
std::string real(double v, int precision = 6) { std::ostringstream out; out << std::fixed << std::setprecision(precision) << v; return out.str(); }
Cell text(std::string value) { return {std::move(value), {}}; }
Cell count(size_t value) { return {std::to_string(value), double(value)}; }
Cell rate(double value) { return {percent(value), value}; }
Cell optionalRate(std::optional<double> value) { return value ? rate(*value) : text("Unavailable"); }
std::string balls(const Result& r) { std::string out; for (int n : r.main) out += (out.empty() ? "" : " ") + std::to_string(n); return out; }
std::string trust(Trust v) { return v == Trust::OfficialRetrieval ? "Official retrieval" : v == Trust::CorroboratedPublic ? "Corroborated public" : "Unverified import"; }
std::string gap(GapStatus s) {
    switch (s) {
    case GapStatus::ExactWithinVerifiedCoverage: return "Within verified coverage";
    case GapStatus::IncompleteCoverage: return "Incomplete coverage";
    case GapStatus::LeftCensored: return "At least; left-censored";
    default: return "No usable data";
    }
}
std::string interval(const Comparison& c) {
    return c.interval ? "[" + real(c.interval->low, 9) + ", " + real(c.interval->high, 9) + "]" : "unavailable (minimum 200 test draws)";
}
void row(Presentation& out, std::vector<Cell> values, size_t identity) { out.rows.push_back(std::move(values)); out.identities.push_back(identity); }
std::string six(const std::array<int,6>& values){std::string out;for(int n:values)out+=(out.empty()?"":" ")+std::to_string(n);return out;}
}
std::string percent(double value) { return real(value * 100, 4) + "%"; }
Presentation present(const ViewData& data, Page page, int selectedNumber, Era era,int progressEvidence,int progressMode,[[maybe_unused]] size_t labOffset) {
    if (!data.snapshot || selectedNumber < 1 || selectedNumber > 49) throw std::invalid_argument("Invalid view input");
    Presentation out; const auto& a = data.analysis; const auto& c = data.coverage;
    const std::string scope = a.scope == Scope::Main ? "Six main" : a.scope == Scope::Extra ? "Extra only" : "All seven";
    out.summary = std::to_string(c.total) + " archived | " + std::to_string(c.eligibleCurrent49) + " eligible current-49 | " +
        std::to_string(c.earlierQuarantined) + " earlier/quarantined | " + std::to_string(c.conflicts) + " unresolved conflicts\r\n" +
        "Coverage: " + (c.earliest.empty() ? "none" : c.earliest + " to " + c.latest) +
        " | Completeness " + (c.complete ? "verified within inventory" : "unknown/incomplete") +
        " | Last retrieval: " + (c.latestRetrievalUtc.empty() ? "never" : c.latestRetrievalUtc) + "\r\n" +
        "Analysis: " + scope + "; N=" + std::to_string(a.draws) + "; " + (a.from.empty() ? "no eligible dates" : a.from + " to " + a.through) +
        ". Freshness after retrieval unknown. Not affiliated with HKJC; no betting or guaranteed prediction.";
    if(page==Page::Lab){
        out.summary="Forecast Lab | Six main | Experimental selection, not winning confidence";
        out.explanation="Frozen forecast-lab-v1 candidate grid and 400/200/100 chronological selector. Every row is predicted before its own outcome, then scored and learned once. Previously inspected archive scores are development evidence, not a fresh holdout or proof of future advantage.";
        if(!data.lab||!data.lab->result.available||!data.lab->result.replay.next){
            out.summary+="\r\nUnavailable: "+(data.lab?(data.lab->error.empty()?data.lab->result.error:data.lab->error):std::string("Not loaded"));
            return out;}
        const auto& lab=*data.lab;
        const auto& next=*lab.result.replay.next;
        const auto chosen=next.decision.candidate;
        out.summary+="\r\nNext six: "+six(next.selections[chosen].main)+" | "+labCandidates()[chosen].id+
            (next.decision.fallback?" (Adaptive fallback)":" (inner-selected)")+
            " | cutoff "+next.cutoff+" | "+std::to_string(next.completedDraws)+" observed";
        out.explanation+="\r\nSelection reason: "+next.decision.reason+". Validation targets "+
            (next.decision.validationTargets?std::to_string(next.decision.validationFirst)+"-"+
             std::to_string(next.decision.validationLast):"unavailable")+
            ". Protocol SHA-256 "+labProtocolDigest()+". Source snapshot "+lab.result.replay.snapshotDigest+".";
        const auto& evaluation=lab.evaluation;
        out.summary+="\r\nDevelopment outer targets "+std::to_string(evaluation.commonTargets)+
            " | selected mean hits "+(evaluation.commonTargets?real(evaluation.selector.meanHits(),4):"unavailable")+
            " | Adaptive "+(evaluation.commonTargets?real(evaluation.adaptive.meanHits(),4):"unavailable")+
            " | fair expectation "+real(evaluation.fairExpectedMainHits,4);
        if(evaluation.commonTargets){
            const auto ci=[](const std::optional<Interval>& value){return value?"["+real(value->low,6)+", "+real(value->high,6)+"]":std::string("unavailable (<200 common targets)");};
            out.explanation+="\r\nSelected minus Adaptive mean main hits "+real(evaluation.versusAdaptive.meanHitDifference,6)+
                "; descriptive paired 95% interval "+ci(evaluation.versusAdaptive.hitInterval)+
                ". Selected minus fixed "+real(evaluation.versusFixed.meanHitDifference,6)+
                "; selected minus realized uniform "+real(evaluation.versusUniform.meanHitDifference,6)+".";
            const auto& m=evaluation.selector;
            out.explanation+="\r\nMain-hit histogram 0..6:";
            for(size_t hit=0;hit<=6;++hit)out.explanation+=" "+std::to_string(hit)+"="+std::to_string(m.exactly(hit));
            out.explanation+=". 3+="+std::to_string(m.atLeast(3))+"/"+std::to_string(m.targets)+
                ", 4+="+std::to_string(m.atLeast(4))+"/"+std::to_string(m.targets)+
                ", exactly 5="+std::to_string(m.exactly(5))+"/"+std::to_string(m.targets)+
                ", exactly 6="+std::to_string(m.exactly(6))+"/"+std::to_string(m.targets)+".";
            out.explanation+="\r\nMean categorical log/Brier losses: selector "+real(m.meanLoss.log,6)+"/"+real(m.meanLoss.brier,6)+
                ", Adaptive "+real(evaluation.adaptive.meanLoss.log,6)+"/"+real(evaluation.adaptive.meanLoss.brier,6)+
                ", fixed "+real(evaluation.fixed.meanLoss.log,6)+"/"+real(evaluation.fixed.meanLoss.brier,6)+".";
            out.explanation+="\r\nIndividual candidate results are exploratory (same inspected targets):";
            for(size_t j=0;j<labCandidates().size();++j)
                out.explanation+="\r\n"+labCandidates()[j].id+": "+real(evaluation.candidates[j].meanHits(),4)+" mean hits";
        }
        if(data.labControls){
            out.explanation+="\r\nFair-history controls: "+std::to_string(data.labControls->completed)+"/"+
                std::to_string(data.labControls->requested)+"; Monte Carlo right-tail "+
                real(data.labControls->tailFraction,6)+" +/- simulation SE "+
                real(data.labControls->simulationStdError,6)+". Not a next-draw winning probability.";
        }else out.explanation+="\r\nFair-history controls are not run automatically. Use Run fair controls to execute the separate local experiment.";
        out.columns={"Target date","Draw ID","Chosen model","Forecast six","Actual six","Main hits","Adaptive hits","Evidence","Training cutoff"};
        double running=0;size_t scored=0;
        for(size_t i=0;i<lab.history.size();++i){const auto& record=lab.history[i];
            const auto choice=record.forecast.decision.candidate;
            std::string actual="Pending",hits="Pending",adaptiveHits="Pending";
            if(record.outcome){
                actual=balls(*record.outcome);
                const auto s=labScore(record.forecast,*record.outcome);
                hits=std::to_string(s.mainHits[choice]);adaptiveHits=std::to_string(s.mainHits[1]);
                if(record.forecast.completedDraws>=400){
                    running+=s.mainHits[choice]-s.mainHits[1];++scored;
                    if(out.chart.empty())out.chartFirst=std::to_string(record.forecast.targetOrdinal);
                    out.chartLast=std::to_string(record.forecast.targetOrdinal);
                    out.chart.push_back(running/double(scored));
                }
            }
            row(out,{text(record.outcome?record.outcome->date:"Upcoming"),text(record.outcome?record.outcome->id:"--"),
                text(labCandidates()[choice].id),text(six(record.forecast.selections[choice].main)),
                text(actual),text(hits),text(adaptiveHits),text(record.provenance),text(record.forecast.cutoff)},i);
        }
        out.progressChart=!out.chart.empty();out.chartZero=true;
        out.chartTitle="Page-local cumulative selected minus Adaptive main hits; dashed zero. Exact rows are in the table.";
        out.summary+="\r\nLearning rows "+(lab.history.empty()?"0":std::to_string(lab.historyOffset+1)+"-"+
            std::to_string(lab.historyOffset+lab.history.size()))+" | Historical replay unless explicitly labeled local pre-fetch";
    }else if(page==Page::Progress){
        if(progressEvidence<0||progressEvidence>1||progressMode<0||progressMode>3)throw std::invalid_argument("Invalid progress selection");
        out.summary="Learning progress | No demonstrated predictive advantage";
        out.explanation="Lower rolling error is better. Cumulative gain = mean(uniform error - adaptive error): positive favors adaptive, negative favors uniform. No guarantee of continuous improvement.\r\nRolling windows require 100 scored records of the selected evidence type, not 100 calendar draws. Pending outcomes are excluded; chart order remains chronological when the table is sorted.\r\nFinal 95% interval only: paired draw-level moving-block bootstrap, block 20, 2000 replicates, minimum 200 scores. Descriptive, not a simultaneous chart band or proof of future advantage.\r\nHistorical replay uses currently accepted corrected history. Saved before local fetch does NOT certify publication before the real-world draw. These evidence groups are never pooled.";
        if(!data.adaptive||!data.adaptive->error.empty()||!data.adaptive->result.current){out.summary+="\r\nProgress unavailable until a compatible current history is loaded.";return out;}
        const auto& progress=data.adaptive->progress;const auto& series=progress.evidence[size_t(progressEvidence)];
        const size_t metric=size_t(progressMode/2);const bool cumulative=progressMode%2!=0;const auto n=series.points.size();
        out.summary+="\r\n"+std::string(progressEvidence?"Saved before local fetch":"Historical replay")+" | history version "+std::to_string(progress.lineage)+" | scored "+std::to_string(n)+" | pending excluded "+std::to_string(progress.pending);
        if(!n){out.summary+="\r\nNo scored results in this evidence group; no curve or interval available.";return out;}
        const auto& comparison=series.finalComparison[metric];
        out.summary+="\r\nFinal mean gain "+real(comparison.meanDifference,9)+" | descriptive 95% interval "+interval(comparison);
        if(n<100)out.summary+="\r\nRolling curve needs 100 scores. Select cumulative view for the available prefix means.";
        out.columns={"Scored count","Result date","Result draw ID","Past draws used","Rolling adaptive log","Rolling uniform log","Mean log gain","Rolling adaptive Brier","Rolling uniform Brier","Mean Brier gain"};
        const auto numeric=[](double v){return Cell{real(v,9),v};};
        for(size_t i=0;i<n;++i){const auto& p=series.points[i];
            row(out,{count(p.count),text(p.date),text(p.id),count(p.trainingDraws),p.rollingAdaptive?numeric((*p.rollingAdaptive)[0]):text("Needs 100 scores"),p.rollingUniform?numeric((*p.rollingUniform)[0]):text("Needs 100 scores"),numeric(p.cumulativeGain[0]),p.rollingAdaptive?numeric((*p.rollingAdaptive)[1]):text("Needs 100 scores"),p.rollingUniform?numeric((*p.rollingUniform)[1]):text("Needs 100 scores"),numeric(p.cumulativeGain[1])},i);
            if(cumulative||p.rollingAdaptive){
                if(out.chart.empty())out.chartFirst=std::to_string(p.count);
                out.chartLast=std::to_string(p.count);out.chart.push_back(cumulative?p.cumulativeGain[metric]:(*p.rollingAdaptive)[metric]);
                if(!cumulative)out.chartOther.push_back((*p.rollingUniform)[metric]);
            }
        }
        out.progressChart=true;out.chartZero=cumulative;
        out.chartTitle=std::string(metric?"Brier":"Log")+(cumulative?" cumulative MEAN gain: solid = uniform minus adaptive; dashed = zero. Above zero favors adaptive.":" rolling 100-score mean error: solid = adaptive; dashed = uniform. Lower is better.");
        out.explanation+="\r\nSelected scores: "+series.points.front().date+" to "+series.points.back().date+". X-axis is selected scored count; dates are in the table. No forecast confidence percentage is derived from this chart.";
        out.explanation+=" Y-axis uses auto-scaled loss units, NOT percentages. Near-zero fluctuations can appear large when zoomed; read the axis and exact table values.";
    }else if(page==Page::Adaptive || page==Page::Learning){
        out.summary="Experimental adaptive estimates | No demonstrated predictive advantage";
        out.explanation=std::string(SelectionLabel)+"\r\nadaptive-mixture-v1: uniform / expanding / half-life 50 / half-life 200; prior 100, learning rate 1, uniform blend 50%. All eligible history; filters do not tune the model. Coverage/freshness remain unknown.\r\n";
        if(!data.adaptive){out.summary+="\r\nNot reconciled";return out;}
        const auto& workspace=*data.adaptive;
        if(!workspace.error.empty()){out.summary+="\r\nUnavailable: "+workspace.error;if(page==Page::Adaptive)return out;}
        if(!workspace.result.current){out.summary+="\r\n"+workspace.result.detail;if(page==Page::Adaptive)return out;}
        if(workspace.result.current){
        const auto prediction=predictAdaptive(workspace.result.current->state);
        out.summary+="\r\n"+std::string(prediction.warmup?"Warm-up | ":"Next-draw estimate | ")+"Training data through "+prediction.cutoff+" | "+std::to_string(prediction.trainingDraws)+" past draws used";
        out.explanation+="Protocol digest: "+adaptiveProtocolDigest()+"\r\nWeights (uniform, expanding, half50, half200): ";
        for(double w:prediction.weights)out.explanation+=real(w,8)+" ";
        out.explanation+="\r\nLearning scores are development replay unless explicitly marked locally pre-fetch; neither certifies a prediction before the real-world draw.";
        }
        if(page==Page::Adaptive){
            const auto prediction=predictAdaptive(workspace.result.current->state);
            out.summary+="\r\nTile % = estimated draw chance, not confidence. Fair main 12.2449%; Extra 2.0408%.";
            out.explanation+="\r\nEach main tile estimates inclusion among ANY of the six main numbers, not its extraction position. Extra estimates the Extra event. These are model marginals, not confidence in the recommendation or a probability of matching the whole set. Do not multiply the tile percentages.";
            out.columns={"Number","Adaptive main","Baseline main","Not main","Adaptive Extra","Baseline Extra","Not Extra","Any seven","Baseline any","Absent"};
            for(size_t n=0;n<49;++n){const auto& q=prediction.probabilities[n];row(out,{count(n+1),rate(q.main),rate(6.0/49),rate(1-q.main),rate(q.extra),rate(1.0/49),rate(1-q.extra),rate(q.any()),rate(7.0/49),rate(q.absent)},n);}
        }else{
            out.columns={"Past draws used","Training data through","Suggested main","Suggested Extra","Result draw ID","Result date","Actual main","Actual Extra","Log error (lower better)","Brier error (lower better)","Evidence type"};
            out.summary="Learning history | history version "+std::to_string(workspace.historyLineage)+" | rows "+std::to_string(workspace.historyOffset+1)+"-"+std::to_string(workspace.historyOffset+workspace.history.size())+"\r\nTraining data through is NOT the predicted result date. Each row predicts a later draw.\r\nResult not linked = no unambiguous actual result attached; not necessarily a future draw.";
            out.explanation+="\r\nPast draws used counts results known BEFORE this prediction. Result date is the later draw being scored. No score means no linked outcome, not zero error. Lower log/Brier error is better; changing weights alone does not prove improvement. Compare averages on the same scored draws against the uniform baseline, not individual lucky hits.";
            for(size_t i=0;i<workspace.history.size();++i){const auto& r=workspace.history[i];std::string selection;for(int n:r.prediction.selection.main)selection+=(selection.empty()?"":" ")+std::to_string(n);
                const auto evidence=r.provenance=="historical-replay"?"Historical replay":r.provenance=="locally-pre-fetch"?"Saved before local fetch":"Result not linked";
                row(out,{count(r.before.draws),text(r.before.lastDate),text(selection),count(size_t(r.prediction.selection.extra)),text(r.outcome?r.outcome->actual.id:"Result not linked"),text(r.outcome?r.outcome->actual.date:"Unknown"),text(r.outcome?balls(r.outcome->actual):"--"),r.outcome?count(size_t(r.outcome->actual.extra)):text("--"),r.outcome?Cell{real(r.outcome->adaptiveLoss.log,9),r.outcome->adaptiveLoss.log}:text("Not scored"),r.outcome?Cell{real(r.outcome->adaptiveLoss.brier,9),r.outcome->adaptiveLoss.brier}:text("Not scored"),text(evidence)},i);
            }
        }
    } else if (page == Page::Numbers) {
        out.columns = {"Number","Observed count","Historical rate","Expected count","Fair baseline","95% Wilson low","95% Wilson high","Observed draws since","Gap qualification"};
        out.explanation = std::string(WilsonLabel) + "\r\nCurrent-49 eligible draws only. Never-observed gaps are lower bounds, not overdue claims. Snapshot: " + data.snapshot->digest();
        for (size_t n = 0; n < 49; ++n) {
            const auto& v = a.numbers[n];
            row(out, {count(n+1),count(v.count),optionalRate(v.observedRate),{real(v.expectedCount,3),v.expectedCount},rate(baseline(a.scope).inclusion),
                v.interval ? rate(v.interval->low) : text("Unavailable"),v.interval ? rate(v.interval->high) : text("Unavailable"),
                v.observedDrawsSinceLast ? count(*v.observedDrawsSinceLast) : text("Unavailable"),text(gap(v.gapStatus))}, n);
            if (v.observedRate) out.chart.push_back(*v.observedRate);
        }
        out.reference = baseline(a.scope).inclusion; out.chartTitle = "Historical rate by number 1-49 (solid bars); fair baseline (dashed line). Table contains exact values.";
    } else if (page == Page::Probability) {
        out.columns = {"Number","Main 6/49","Not main 43/49","Extra 1/49","Not Extra 48/49","Any 7/49","Absent 42/49"};
        out.explanation = "Exact fair-independent model, unchanged by history or filters; not measured physical fairness.\r\nA specified six-main set: 1 / 13,983,816. Without-replacement pairs: main 30/2352; any 42/2352; Extra-only pairs unavailable. Do not multiply marginals into joint odds.";
        for (size_t n = 0; n < 49; ++n) row(out,{count(n+1),rate(6.0/49),rate(43.0/49),rate(1.0/49),rate(48.0/49),rate(7.0/49),rate(42.0/49)},n);
    } else if (page == Page::History) {
        out.columns = {"Draw ID","HK date","Six main (sorted, NOT extraction order)","Extra","Era","Source status","Conflict","Source URL","Retrieved UTC"};
        out.explanation = "History browsing includes unverified/conflicting records; analysis does not. Earlier rows have unreviewed pool boundaries and never enter current-49 statistics. Date controls filter this history table and descriptive analysis; last-N applies only to analysis.";
        for (size_t i = 0; i < data.snapshot->records().size(); ++i) {
            const auto& r = data.snapshot->records()[i];
            if (r.era != era || (!data.historyFrom.empty() && r.date < data.historyFrom) ||
                (!data.historyThrough.empty() && r.date > data.historyThrough)) {
                continue;
            }
            row(out, {
                text(r.id), text(r.date), text(balls(r)), count(size_t(r.extra)),
                text(r.era == Era::Current49 ? "Current 49" : "Earlier, unreviewed"),
                text(trust(r.source.trust)),
                text(r.unresolvedConflict ? "UNRESOLVED" : "No"),
                text(r.source.url), text(r.source.retrievedUtc)
            }, i);
        }
        out.summary += "\r\nHistory view: " + std::to_string(out.rows.size()) + " records";
        if (!data.historyFrom.empty() || !data.historyThrough.empty()) {
            out.summary += " within " + (data.historyFrom.empty() ? "earliest" : data.historyFrom) +
                " to " + (data.historyThrough.empty() ? "latest" : data.historyThrough);
        }
    } else if (page == Page::Sources) {
        out.columns={"Draw ID","Date","Revision","Main","Extra","Source status","Active","Conflict","Source URL","Retrieved UTC","Lineage","Evidence","Content digest","Decision history"};
        out.explanation=updateHistoryText(data.updateAttempts)+"\r\nSelect a revision to inspect all fields below. Resolve requires an unresolved draw, an explicit reason and source evidence; prior revisions are retained.\r\nUpdate history fetches public HKJC results for local use only (hkjc-public-local-v1), not a supported third-party API or HKJC endorsement. No startup download or bundled redistribution. Missing ranges and 14 recent days are checked; older corrections and complete inventory remain unknown. Cached data works offline. Matching/shared sources do not establish independent corroboration. Dataset: "+data.snapshot->digest();
        for(size_t i=0;i<data.revisions.size();++i){const auto& rev=data.revisions[i];const auto& r=rev.result;if(r.era!=era)continue;
            std::string decisions;
            for(const auto& decision:data.resolutions)if(decision.drawKey==drawKey(r)){
                if(!decisions.empty())decisions+=" | ";
                decisions+=decision.decidedUtc+" selected "+decision.selectedDigest+": "+decision.reason+
                    "; source evidence "+decision.evidence;
            }
            row(out,{text(r.id),text(r.date),text(rev.digest),text(balls(r)),count(size_t(r.extra)),text(trust(r.source.trust)),text(rev.active?"Yes":"No"),
                text(r.unresolvedConflict?"UNRESOLVED":"No"),text(r.source.url),text(r.source.retrievedUtc),text(r.source.lineage),text(r.source.verificationEvidence),text(r.source.contentSha256),text(decisions)},i);}
    } else if(page==Page::Pairs){
        out.columns={"First","Second","Observed pair count","Historical pair rate","Fair pair baseline"};
        out.explanation="Distinct unordered pairs, same eligible draws/window as Numbers; descriptive, not winning tips. Extra-only pair analysis is unavailable.";
        if(a.pairs)for(size_t x=0;x<49;++x)for(size_t y=x+1;y<49;++y){const auto n=(*a.pairs)[x][y];
            row(out,{count(x+1),count(y+1),count(n),a.draws?rate(double(n)/double(a.draws)):text("Unavailable"),rate(*baseline(a.scope).pair)},x*49+y);}
    } else if(page==Page::Rolling){
        out.columns={"Draw ID","Through date","Number","Window draws","Historical rolling rate","100-draw window"};
        out.explanation="Selected number "+std::to_string(selectedNumber)+"; "+scope+"; trailing 100 eligible draws within selected analysis coverage. Initial incomplete windows are marked. Missing coverage is not filled with invented draws.";
        for(size_t i=0;i<a.rolling.size();++i){const auto& p=a.rolling[i];row(out,{text(p.id),text(p.date),count(size_t(selectedNumber)),count(p.draws),rate(p.rates[selectedNumber-1]),text(p.fullWindow?"Full":"Incomplete initial window")},i);out.chart.push_back(p.rates[selectedNumber-1]);}
        out.reference=baseline(a.scope).inclusion;out.chartTitle="Rolling historical rate, chronological left to right (solid line); fair baseline (dashed). Exact points are in the table.";
    } else if(page==Page::Forecasts || page==Page::Calibration){
        const auto& f=data.forecast; const auto& b=data.backtest;
        std::string status=!f.modelAvailable?"Insufficient training: at least 200 eligible draws required. Uniform baseline only.":
            b.evidence==Evidence::InsufficientTestHistory?"Insufficient held-out evidence: fewer than 200 test draws.":
            b.evidence==Evidence::HistoricalHeldOutImprovement?"Historical held-out improvement only; no established future advantage.":"NO DEMONSTRATED PREDICTIVE ADVANTAGE.";
        out.summary="Experimental estimates - "+status+"\r\nfrequency-shrinkage-v1 | lambda=100 | training="+std::to_string(f.trainingDraws)+
            " | next draw after "+(f.cutoff.empty()?"no data":f.cutoff)+" (not a verified schedule).\r\nAll eligible current-49 history; descriptive window filters do not tune the model. Completeness/freshness unknown; no betting or HKJC affiliation.";
        out.explanation=std::string(SelectionLabel)+"\r\n";
        if(f.selection){out.explanation+="Suggested main: ";for(int n:f.selection->main)out.explanation+=std::to_string(n)+" ";out.explanation+="; distinct Extra: "+std::to_string(f.selection->extra)+". ";}
        out.explanation+="Test N="+std::to_string(b.draws.size())+(b.draws.empty()?"":", "+b.draws.front().date+" to "+b.draws.back().date)+"; retrospective corrected data, strictly earlier training.\r\n";
        if(!b.draws.empty())out.explanation+="Brier baseline/model: "+real(b.meanUniform.brier,9)+" / "+real(b.meanModel.brier,9)+"; log loss: "+real(b.meanUniform.log,9)+" / "+real(b.meanModel.log,9)+".\r\nBrier difference (positive=improvement): "+real(b.brierDifference.meanDifference,9)+"; 95% block interval "+interval(b.brierDifference)+". Log difference: "+real(b.logDifference.meanDifference,9)+"; interval "+interval(b.logDifference)+".\r\n";
        out.explanation+="Paired draw-level approximate moving-block bootstrap: length 20, 2000 replicates, fixed mt19937_64 seed 5575863618947872817. No 49-fold independent evidence or joint likelihood. Snapshot: "+data.snapshot->digest();
        if(page==Page::Forecasts){
            out.columns={"Number","Model main","Baseline main","Not main","Model Extra","Baseline Extra","Not Extra","Model any","Baseline any","Absent"};
            for(size_t n=0;n<49;++n){const auto& q=f.probabilities[n];row(out,{count(n+1),rate(q.main),rate(6.0/49),rate(1-q.main),rate(q.extra),rate(1.0/49),rate(1-q.extra),rate(q.any()),rate(7.0/49),rate(q.absent)},n);}
        }else{
            out.columns={"Method","Scope","Bin [low,high), last inclusive","Samples","Mean predicted","Observed rate"};
            for(size_t method=0;method<2;++method)for(size_t scopeIndex=0;scopeIndex<3;++scopeIndex)for(size_t bin=0;bin<10;++bin){
                const auto& v=(method?b.modelCalibration:b.uniformCalibration)[scopeIndex][bin];row(out,{text(method?"Model":"Uniform"),text(scopeIndex==0?"Main":scopeIndex==1?"Extra":"Any"),text(std::to_string(bin)+"/10 - "+std::to_string(bin+1)+"/10"),count(size_t(v.samples)),optionalRate(v.meanPrediction()),optionalRate(v.observedRate())},method*30+scopeIndex*10+bin);}
        }
    } else throw std::invalid_argument("Unknown statistics page");
    return out;
}
void sortRows(Presentation& p,size_t column,bool descending){
    if(column>=p.columns.size())return;
    std::vector<size_t> order(p.rows.size());std::iota(order.begin(),order.end(),0);
    std::stable_sort(order.begin(),order.end(),[&](size_t x,size_t y){const auto& a=p.rows[x][column];const auto& b=p.rows[y][column];
        if(a.numeric&&b.numeric)return descending?*a.numeric>*b.numeric:*a.numeric<*b.numeric;
        if(bool(a.numeric)!=bool(b.numeric))return bool(a.numeric);
        return descending?a.text>b.text:a.text<b.text;});
    auto rows=std::move(p.rows);auto identities=std::move(p.identities);p.rows.clear();p.identities.clear();
    for(size_t i:order){p.rows.push_back(std::move(rows[i]));p.identities.push_back(identities[i]);}
}
}
