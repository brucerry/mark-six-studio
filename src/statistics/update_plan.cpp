#include "update_plan.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

namespace marksix::statistics {
namespace {
using namespace std::chrono;
void require(bool condition, const char* message) { if (!condition) throw std::invalid_argument(message); }
void cancelled(std::stop_token stop) { if (stop.stop_requested()) throw std::runtime_error("Update planning cancelled"); }
bool label(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c < 127; });
}
sys_days date(const std::string& text) {
    require(validDate(text), "Invalid update date");
    return sys_days{year{std::stoi(text.substr(0,4))}/unsigned(std::stoi(text.substr(5,2)))/unsigned(std::stoi(text.substr(8,2)))};
}
std::string format(sys_days value) {
    const year_month_day d{value}; char out[16]{};
    std::snprintf(out,sizeof(out),"%04d-%02u-%02u",int(d.year()),unsigned(d.month()),unsigned(d.day())); return out;
}
std::pair<sys_days,sys_days> range(const DateRange& value) {
    const auto from=date(value.from), through=date(value.through);
    require(from<=through,"Reversed update range"); return {from,through};
}
std::string identity(const Result& r) { return r.date.substr(0,4)+":"+r.id; }
bool sameValues(const Result& a,const Result& b) {
    return a.id==b.id && a.date==b.date && a.era==b.era && a.main==b.main && a.extra==b.extra &&
        (!a.extractionOrder || !b.extractionOrder || a.extractionOrder==b.extractionOrder);
}
}
UpdateComparison compareUpdate(const Snapshot& local,const std::vector<Result>& fetched,std::stop_token stop) {
    cancelled(stop); require(fetched.size()<=100000,"Update contains too many results");
    std::map<std::string,const Result*> existing;
    for(const auto& row:local.records()) { cancelled(stop); existing.emplace(identity(row),&row); }
    UpdateComparison out; std::set<std::string> seen;
    for(const auto& raw:fetched) {
        cancelled(stop); const auto row=canonicalize(raw);
        require(row.source.trust!=Trust::Unverified && !row.unresolvedConflict,"Update comparison requires verified source results");
        const auto key=identity(row); require(seen.insert(key).second,"Duplicate source identity in update");
        auto disposition=UpdateDisposition::New; const auto found=existing.find(key);
        if(found!=existing.end()) {
            const auto& old=*found->second;
            if(old.unresolvedConflict || !sameValues(old,row)) disposition=UpdateDisposition::Conflict;
            else if(old.source.trust==Trust::Unverified) disposition=UpdateDisposition::NeedsVerification;
            else disposition=UpdateDisposition::Unchanged;
        }
        out.rows.push_back(disposition);
        switch(disposition) {
        case UpdateDisposition::New: ++out.added; break;
        case UpdateDisposition::Unchanged: ++out.unchanged; break;
        case UpdateDisposition::NeedsVerification: ++out.needsVerification; break;
        case UpdateDisposition::Conflict: ++out.conflicts; break;
        }
    }
    return out;
}
UpdatePlan planUpdate(const UpdatePolicy& p,const Snapshot& local,const std::vector<CommittedWindow>& receipts,
                      const std::optional<Result>& latest,std::stop_token stop) {
    cancelled(stop);
    require(label(p.sourceId)&&label(p.version),"Update policy needs source and version");
    require(p.maximumWindowDays>=1&&p.maximumWindowDays<=366 && p.correctionOverlapDays>=1&&p.correctionOverlapDays<=366,
            "Update request/overlap bounds must be between 1 and 366 days");
    require(receipts.size()<=100000,"Too many update receipts");
    const auto [first,rangeEnd]=range(p.available); const auto span=(rangeEnd-first).count()+1;
    require(span<=36600,"Update range exceeds the 100-year safety bound");
    const size_t count=size_t(span);
    // Difference array makes even heavily overlapping receipts O(receipts + days).
    std::vector<int> coverageDelta(count+1,0);
    for(const auto& receipt:receipts) {
        cancelled(stop);
        if(receipt.sourceId!=p.sourceId || receipt.policyVersion!=p.version) continue;
        const auto [begin,end]=range(receipt.range);
        require(receipt.responseSha256.size()==64 && receipt.responseSha256.find_first_not_of("0123456789abcdef")==std::string::npos,
                "Invalid committed response digest");
        if(end<first || begin>rangeEnd) continue;
        ++coverageDelta[size_t((std::max(begin,first)-first).count())];
        --coverageDelta[size_t((std::min(end,rangeEnd)-first).count())+1];
    }
    const auto overlapStart=std::max(first,rangeEnd-days{int(p.correctionOverlapDays-1)});
    UpdatePlan out{p.sourceId,p.version,local.digest(),{}, {format(overlapStart),format(rangeEnd)},false};
    std::vector<bool> needed(count); int covered=0;
    for(size_t i=0;i<count;++i) {
        cancelled(stop); covered+=coverageDelta[i];
        needed[i]=covered==0 || first+days{int(i)}>=overlapStart;
    }
    if(latest) {
        const auto row=canonicalize(*latest); const auto day=date(row.date);
        require(row.source.sourceId==p.sourceId && day>=first && day<=rangeEnd,"Latest result outside source/range");
        const auto comparison=compareUpdate(local,{row},stop);
        out.latestNeedsReconciliation=comparison.unchanged==0;
        // A missing/corrected latest identity cannot be hidden by an old receipt.
        if(out.latestNeedsReconciliation) needed[size_t((day-first).count())]=true;
    }
    for(size_t start=0;start<count;) {
        cancelled(stop); if(!needed[start]) {++start;continue;}
        size_t end=start;
        while(end+1<count && needed[end+1] && end-start+1<p.maximumWindowDays) ++end;
        out.requests.push_back({format(first+days{int(start)}),format(first+days{int(end)})}); start=end+1;
    }
    return out;
}
}
