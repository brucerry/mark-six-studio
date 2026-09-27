#pragma once
#include "adaptive_ledger.hpp"

namespace marksix::statistics {
struct LearningScore {
    std::string date,id;
    size_t trainingDraws=0;
    bool prefetch=false;
    Loss adaptive,uniform;
};
struct LearningPoint {
    std::string date,id;
    size_t trainingDraws=0,count=0;
    // Index 0 is log, index 1 is category Brier. No missing rolling value is zero.
    std::array<double,2> cumulativeGain{};
    std::optional<std::array<double,2>> rollingAdaptive,rollingUniform;
};
struct LearningSeries {
    std::vector<LearningPoint> points;
    std::array<Comparison,2> finalComparison{};
};
struct LearningProgress {
    int64_t lineage=0;
    size_t pending=0;
    std::array<LearningSeries,2> evidence; // replay, locally pre-fetch (never pooled)
};
LearningProgress calculateLearningProgress(const std::vector<LearningScore>& scores,std::stop_token stop={});
LearningProgress readLearningProgress(AdaptiveLedger& ledger,int64_t lineage,std::stop_token stop={});
}
