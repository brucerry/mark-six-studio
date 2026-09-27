#include "update_plan.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace marksix::statistics;
namespace {
size_t checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F f,const char* message){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,message);}
CommittedWindow receipt(std::string from,std::string through){return {"test-source","fixture-v1",{from,through},sha256("synthetic response")};}
Result result(std::string date="2024-02-20",std::string id="24/020"){
    return {id,date,{1,2,3,4,5,6},7,Era::Current49,
        {"test-source","https://example.invalid/results","2026-09-25T00:00:00Z",sha256("synthetic"),"synthetic",Trust::OfficialRetrieval,"fixture only"},{},"",false};
}
std::string day(int offset){
    using namespace std::chrono;const year_month_day d{sys_days{2024y/January/1}+days{offset}};
    char out[16]{};std::snprintf(out,sizeof(out),"%04d-%02u-%02u",int(d.year()),unsigned(d.month()),unsigned(d.day()));return out;
}
}
size_t testUpdatePlan(){
    const Snapshot empty({});const UpdatePolicy policy{"test-source","fixture-v1",{"2024-01-01","2024-03-31"},31,7};
    auto p=planUpdate(policy,empty,{});
    check(p.requests==std::vector<DateRange>{{"2024-01-01","2024-01-31"},{"2024-02-01","2024-03-02"},{"2024-03-03","2024-03-31"}},"Initial backfill includes leap day and respects window bounds");
    check(p.snapshotDigest==empty.digest()&&p.policyVersion==policy.version,"Plan identifies snapshot and policy");
    p=planUpdate(policy,empty,{receipt("2024-01-01","2024-03-31")});
    check(p.requests==std::vector<DateRange>{{"2024-03-25","2024-03-31"}},"No new draw still checks bounded correction overlap");
    p=planUpdate(policy,empty,{receipt("2024-01-01","2024-03-20")});
    check(p.requests==std::vector<DateRange>{{"2024-03-21","2024-03-31"}},"Incremental missing tail coalesces with overlap");
    const std::vector<CommittedWindow> interrupted{receipt("2024-01-01","2024-01-31"),receipt("2024-03-01","2024-03-31")};
    p=planUpdate(policy,Snapshot({result("2024-03-31","24/040")}),interrupted);
    check(p.requests==std::vector<DateRange>{{"2024-02-01","2024-02-29"},{"2024-03-25","2024-03-31"}},"Newer row cannot mask interrupted backfill hole");
    check(planUpdate(policy,empty,interrupted).requests==p.requests,"Records do not manufacture range coverage");
    auto unverified=result();unverified.source.trust=Trust::Unverified;
    check(planUpdate(policy,Snapshot({unverified}),{}).requests==planUpdate(policy,empty,{}).requests,"Unverified and legacy cache cannot skip backfill");
    auto foreign=receipt("2024-01-01","2024-03-31");foreign.sourceId="other-source";
    check(planUpdate(policy,empty,{foreign}).requests==planUpdate(policy,empty,{}).requests,"Other source receipt ignored");
    foreign.sourceId=policy.sourceId;foreign.policyVersion="old-policy";
    check(planUpdate(policy,empty,{foreign}).requests==planUpdate(policy,empty,{}).requests,"Other policy receipt ignored");
    const auto latest=result();const Snapshot one({latest});const std::vector<CommittedWindow> full{receipt("2024-01-01","2024-03-31")};
    check(!planUpdate(policy,one,full,latest).latestNeedsReconciliation,"Verified matching latest identity recognized");
    p=planUpdate(policy,empty,full,latest);
    check(p.latestNeedsReconciliation&&p.requests.front()==DateRange{"2024-02-20","2024-02-20"},"Latest identity absent despite receipt is fetched");
    auto correction=latest;correction.extra=8;
    check(planUpdate(policy,one,full,correction).latestNeedsReconciliation,"Latest correction not hidden by receipt");
    auto refreshed=latest;refreshed.source.retrievedUtc="2026-09-25T01:00:00Z";refreshed.source.contentSha256=sha256("another response");std::reverse(refreshed.main.begin(),refreshed.main.end());
    check(compareUpdate(one,{refreshed}).unchanged==1,"Retrieval timestamp/hash and set order do not invent new outcomes");
    check(compareUpdate(one,{correction}).conflicts==1,"Corrected Extra is a conflict not overwrite");
    auto redated=latest;redated.date="2024-02-21";
    check(compareUpdate(one,{redated}).conflicts==1,"Same published identity with changed date is a conflict");
    check(compareUpdate(Snapshot({unverified}),{latest}).needsVerification==1,"Unverified matching values still require verified ingestion");
    auto conflicted=latest;conflicted.unresolvedConflict=true;
    check(compareUpdate(Snapshot({conflicted}),{latest}).conflicts==1,"Matching a conflicted active row does not resolve it");
    auto order=latest;order.extractionOrder=order.main;order.orderEvidence="fixture order";auto reversed=order;
    std::reverse(reversed.extractionOrder->begin(),reversed.extractionOrder->end());
    check(compareUpdate(Snapshot({order}),{reversed}).conflicts==1,"Contradictory evidenced extraction order retained as conflict");
    check(compareUpdate(empty,{}).rows.empty(),"Valid empty response yields no fabricated result");
    check(compareUpdate(empty,{latest}).added==1,"New identity counted");
    auto next=latest;next.date="2024-02-22";next.id="24/021";
    const auto mixed=compareUpdate(one,{refreshed,next});check(mixed.unchanged==1&&mixed.added==1&&mixed.rows.size()==2,"Per-row disposition and counts agree");
    rejects([&]{compareUpdate(one,{latest,refreshed});},"Duplicate fetched identities rejected");
    rejects([&]{compareUpdate(one,{unverified});},"Unverified source response rejected");
    rejects([&]{compareUpdate(one,{conflicted});},"Conflicted source response rejected");
    auto invalid=latest;invalid.main[0]=7;rejects([&]{compareUpdate(one,{invalid});},"Malformed number rejects update comparison");
    auto wrong=latest;wrong.source.sourceId="foreign";rejects([&]{planUpdate(policy,one,full,wrong);},"Latest source mismatch rejected");
    wrong=latest;wrong.date="2024-04-01";rejects([&]{planUpdate(policy,one,full,wrong);},"Latest outside requested range rejected");
    for(const auto dates:std::vector<DateRange>{{"2024-02-30","2024-03-31"},{"2024-03-31","2024-01-01"},{"1975-01-01","9999-12-31"}}){auto bad=policy;bad.available=dates;rejects([&]{planUpdate(bad,empty,{});},"Invalid/reversed/oversized date span rejected");}
    for(size_t bound:{size_t(0),size_t(367)}){auto bad=policy;bad.maximumWindowDays=bound;rejects([&]{planUpdate(bad,empty,{});},"Invalid request bound rejected");bad=policy;bad.correctionOverlapDays=bound;rejects([&]{planUpdate(bad,empty,{});},"Invalid overlap bound rejected");}
    auto broken=receipt("2024-01-01","2024-03-31");broken.responseSha256="bad";
    rejects([&]{planUpdate(policy,empty,{broken});},"Malformed committed receipt rejected");
    std::stop_source stop;stop.request_stop();rejects([&]{planUpdate(policy,one,full,latest,stop.get_token());},"Plan cancellation");rejects([&]{compareUpdate(one,{latest},stop.get_token());},"Comparison cancellation");
    check(planUpdate(policy,one,full,latest).requests==planUpdate(policy,one,full,latest).requests,"Cancelled planning cannot change next plan");
    // Independent per-day set oracle: duplicates, clipping, overlap, holes and bounds.
    for(int trial=0;trial<80;++trial){
        std::vector<CommittedWindow> receipts;std::set<std::string> covered,expected,actual;
        for(int j=0;j<7;++j){int first=(trial*13+j*19)%120-15,last=first+(trial+j)%23;
            receipts.push_back(receipt(day(first),day(last)));if(j%2==0)receipts.push_back(receipts.back());
            for(int k=first;k<=last;++k)if(k>=0&&k<=90)covered.insert(day(k));}
        auto config=policy;config.maximumWindowDays=size_t(trial%17+1);config.correctionOverlapDays=size_t(trial%15+1);
        for(int k=0;k<=90;++k)if(!covered.contains(day(k))||k>=91-int(config.correctionOverlapDays))expected.insert(day(k));
        const auto plan=planUpdate(config,empty,receipts);std::string previous;
        for(const auto& request:plan.requests){
            check(previous.empty()||previous<request.from,"Requests ordered and disjoint");previous=request.through;size_t days=0;
            for(int k=0;k<=90;++k)if(day(k)>=request.from&&day(k)<=request.through){check(actual.insert(day(k)).second,"No date requested twice");++days;}
            check(days>=1&&days<=config.maximumWindowDays,"Request size bound maintained");}
        check(actual==expected,"Planner matches independent per-day union/subtraction oracle");
    }
    std::cout<<"PASS incremental planner: missing ranges, overlap, source/version isolation, identity reconciliation, cancellation, independent date-set oracle; network=none\n";
    return checks;
}
