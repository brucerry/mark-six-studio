#include "learning_progress.hpp"
#include <cmath>

namespace marksix::statistics {
namespace {void cancelled(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Learning progress cancelled");}}
LearningProgress calculateLearningProgress(const std::vector<LearningScore>& scores,std::stop_token stop){
    cancelled(stop);LearningProgress out;
    if(scores.size()>100000)throw std::invalid_argument("Learning progress limit exceeded");
    for(size_t group=0;group<2;++group){
        auto& series=out.evidence[group];std::array<std::vector<double>,2> differences;
        std::array<double,2> total{},rollingA{},rollingU{};
        std::vector<std::array<double,2>> adaptive,uniform;
        for(const auto& s:scores){
            cancelled(stop);if(size_t(s.prefetch)!=group)continue;
            if(!series.points.empty()&&(s.date<=series.points.back().date||s.trainingDraws<=series.points.back().trainingDraws))
                throw std::invalid_argument("Progress scores are not chronological");
            const std::array<double,2> a{s.adaptive.log,s.adaptive.brier},u{s.uniform.log,s.uniform.brier};
            adaptive.push_back(a);uniform.push_back(u);const auto n=adaptive.size();
            LearningPoint point{s.date,s.id,s.trainingDraws,n};
            for(size_t m=0;m<2;++m){
                if(!std::isfinite(a[m])||!std::isfinite(u[m])||a[m]<0||u[m]<0)throw std::invalid_argument("Invalid progress score");
                const double d=u[m]-a[m];differences[m].push_back(d);total[m]+=d;
                point.cumulativeGain[m]=total[m]/double(n);rollingA[m]+=a[m];rollingU[m]+=u[m];
                if(n>100){rollingA[m]-=adaptive[n-101][m];rollingU[m]-=uniform[n-101][m];}
            }
            if(n>=100){point.rollingAdaptive=rollingA;point.rollingUniform=rollingU;
                for(size_t m=0;m<2;++m){(*point.rollingAdaptive)[m]/=100;(*point.rollingUniform)[m]/=100;}}
            series.points.push_back(point);
        }
        for(size_t m=0;m<2;++m)series.finalComparison[m]=bootstrap(differences[m],stop);
    }
    return out;
}
LearningProgress readLearningProgress(AdaptiveLedger& ledger,int64_t lineage,std::stop_token stop){
    std::vector<LearningScore> scores;size_t pending=0;
    for(size_t offset=0;;offset+=200){
        cancelled(stop);const auto page=ledger.predictions(lineage,offset,200);
        for(const auto& r:page){cancelled(stop);if(!r.outcome){++pending;continue;}
            if(r.provenance!="historical-replay"&&r.provenance!="locally-pre-fetch")throw std::runtime_error("Unknown learning evidence type");
            const auto& s=*r.outcome;scores.push_back({s.actual.date,s.actual.id,r.before.draws,r.provenance=="locally-pre-fetch",s.adaptiveLoss,s.uniformLoss});}
        if(page.size()<200)break;
        if(offset>=100000)throw std::runtime_error("Learning progress ledger limit exceeded");
    }
    auto out=calculateLearningProgress(scores,stop);out.lineage=lineage;out.pending=pending;return out;
}
}
