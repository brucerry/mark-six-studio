#pragma once
#include "analysis.hpp"
#include "archive.hpp"
#include "coverage.hpp"
#include "forecast.hpp"
#include "adaptive_coordinator.hpp"
#include <algorithm>

namespace marksix::statistics {
enum class Page { Numbers, History, Probability, Forecasts, Sources, Pairs, Rolling, Calibration, Adaptive, Learning, Progress, Lab };
inline constexpr std::array<Page,12> PageOrder{Page::Lab,Page::Adaptive,Page::Learning,Page::Progress,Page::Numbers,Page::History,Page::Probability,Page::Forecasts,Page::Sources,Page::Pairs,Page::Rolling,Page::Calibration};
struct ViewData {
    std::shared_ptr<const Snapshot> snapshot;
    std::string historyFrom;
    std::string historyThrough;
    Coverage coverage;
    Analysis analysis;
    Forecast forecast;
    Backtest backtest;
    std::vector<StoredRevision> revisions;
    std::vector<UpdateAttempt> updateAttempts;
    std::vector<Resolution> resolutions;
    uint64_t archiveVersion = 0;
    std::shared_ptr<const AdaptiveWorkspace> adaptive;
    std::shared_ptr<const LabWorkspace> lab;
    std::optional<LabControlReport> labControls;
};
struct Cell { std::string text; std::optional<double> numeric; };
struct Presentation {
    std::string summary, explanation;
    std::vector<std::string> columns;
    std::vector<std::vector<Cell>> rows;
    std::vector<size_t> identities; // Stable domain record/revision index, not a sorted display index.
    std::vector<double> chart;
    double reference = 0;
    std::string chartTitle;
    bool progressChart=false,chartZero=false;
    std::vector<double> chartOther;
    std::string chartFirst,chartLast;
};
Presentation present(const ViewData& data, Page page, int selectedNumber = 1, Era era = Era::Current49,int progressEvidence=0,int progressMode=0,size_t labOffset=0);
void sortRows(Presentation& presentation, size_t column, bool descending);
std::string percent(double value);
}
