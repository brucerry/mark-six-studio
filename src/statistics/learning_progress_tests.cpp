#include "learning_progress.hpp"
#include "presentation.hpp"
#include <chrono>
#include <cstdio>
#include <cmath>
#include <limits>
using namespace marksix::statistics;
size_t testLearningProgress(){
    size_t checks=0;const auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Learning progress fixture failed");};
    const auto near=[&](double a,double b){check(std::abs(a-b)<1e-10);};
    std::vector<LearningScore> rows;
    for(int i=0;i<201;++i){
        const auto date=std::chrono::year_month_day{std::chrono::sys_days{std::chrono::year{2002}/7/6}+std::chrono::days{i}};
        char b[32]{};std::snprintf(b,sizeof(b),"%04d-%02u-%02u",int(date.year()),unsigned(date.month()),unsigned(date.day()));
        LearningScore s;s.date=b;s.id=std::to_string(i);s.trainingDraws=size_t(i+1);
        s.adaptive.log=double(i+1)/1000;s.uniform.log=.5;s.adaptive.brier=.3;s.uniform.brier=.2;rows.push_back(s);
    }
    const auto all=calculateLearningProgress(rows);const auto& points=all.evidence[0].points;
    check(points.size()==201&&all.evidence[1].points.empty());check(!points[98].rollingAdaptive&&points[99].rollingAdaptive);
    near((*points[99].rollingAdaptive)[0],.0505);near((*points[100].rollingAdaptive)[0],.0515);
    near(points.back().cumulativeGain[0],.399);near(points.back().cumulativeGain[1],-.1);
    check(all.evidence[0].finalComparison[0].interval.has_value());near(all.evidence[0].finalComparison[1].interval->low,-.1);
    ViewData data;data.snapshot=std::make_shared<const Snapshot>(std::vector<Result>{});
    auto workspace=std::make_shared<AdaptiveWorkspace>();workspace->progress=all;workspace->result.current=AdaptiveHead{};data.adaptive=workspace;
    auto rolling=present(data,Page::Progress);check(rolling.chart.size()==102&&rolling.chartOther.size()==102&&rolling.chartFirst=="100");
    near(rolling.chart.front(),.0505);const auto chart=rolling.chart;sortRows(rolling,0,true);check(rolling.chart==chart&&rolling.identities.front()==200);
    const auto gain=present(data,Page::Progress,1,Era::Current49,0,3);check(gain.chartZero&&gain.chartOther.empty()&&gain.chart.size()==201);near(gain.chart.back(),-.1);
    const auto empty=present(data,Page::Progress,1,Era::Current49,1,0);check(empty.chart.empty()&&empty.rows.empty()&&empty.summary.find("No scored results")!=std::string::npos);
    auto prefix=rows;prefix.resize(99);const auto early=calculateLearningProgress(prefix);
    check(!early.evidence[0].finalComparison[0].interval);near(early.evidence[0].points.back().cumulativeGain[0],points[98].cumulativeGain[0]);
    prefix=rows;prefix.resize(199);check(!calculateLearningProgress(prefix).evidence[0].finalComparison[0].interval);
    prefix.push_back(rows[199]);check(calculateLearningProgress(prefix).evidence[0].finalComparison[0].interval.has_value());
    for(size_t i=0;i<rows.size();++i)rows[i].prefetch=i%2!=0;
    const auto split=calculateLearningProgress(rows);check(split.evidence[0].points.size()==101&&split.evidence[1].points.size()==100);
    check(!split.evidence[0].finalComparison[0].interval&&!split.evidence[1].finalComparison[0].interval);
    near((*split.evidence[0].points[99].rollingAdaptive)[0],.100);near((*split.evidence[1].points[99].rollingAdaptive)[0],.101);
    check(calculateLearningProgress({}).evidence[0].points.empty());
    std::stop_source stop;stop.request_stop();bool rejected=false;try{calculateLearningProgress(rows,stop.get_token());}catch(...){rejected=true;}check(rejected);
    rows[0].adaptive.log=std::numeric_limits<double>::quiet_NaN();rejected=false;try{calculateLearningProgress(rows);}catch(...){rejected=true;}check(rejected);
    return checks;
}
