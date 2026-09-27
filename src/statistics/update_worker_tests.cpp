#include "update_worker.hpp"
#include "presentation.hpp"
#include <sqlite3.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <stdexcept>

using namespace marksix::statistics;
namespace {
size_t checks = 0;
void check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F fn, const char* message) { bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}check(rejected,message); }
Result row(int n) {
    Result r; r.id="03/00"+std::to_string(n);r.date="2003-01-0"+std::to_string(n);r.main={1,2,3,4,5,6};r.extra=7;
    r.source={"synthetic-worker","https://example.invalid/fixture","2026-09-25T00:00:00Z",sha256("fixture response"),"synthetic-only",Trust::OfficialRetrieval,"injected fixture; not real data"};return r;
}
struct Gate {
    std::mutex mutex;std::condition_variable_any cv;bool entered=false,released=false;
    void block(std::stop_token stop) { std::unique_lock lock(mutex);entered=true;cv.notify_all();cv.wait(lock,stop,[&]{return released;});if(stop.stop_requested())throw ProviderFailure(UpdateFailure::Cancelled); }
    void wait() { std::unique_lock lock(mutex);check(cv.wait_for(lock,std::chrono::seconds(5),[&]{return entered;}),"Worker reached controlled gate"); }
    void release() { std::lock_guard lock(mutex);released=true;cv.notify_all(); }
};
struct FixtureProvider final : UpdateProvider {
    bool allowed=true,empty=false,malformed=false,omitLatest=false,wrongRange=false;
    UpdateFailure latestFailure=UpdateFailure::None,fetchFailure=UpdateFailure::None;
    size_t latestCalls=0,fetchCalls=0,blockFetch=0;
    std::shared_ptr<Gate> latestGate,fetchGate;
    std::vector<DateRange> requested;
    bool enabled()const override{return allowed;}
    UpdatePolicy policy()const override{return {"synthetic-worker","fixture-v1",{"2003-01-01","2003-01-03"},1,1};}
    std::optional<Result> latest(std::stop_token stop)override{
        ++latestCalls;if(latestGate)latestGate->block(stop);
        if(latestFailure!=UpdateFailure::None)throw ProviderFailure(latestFailure);
        return empty?std::optional<Result>{}:row(3);
    }
    FetchedWindow fetch(const DateRange& range,std::stop_token stop)override{
        ++fetchCalls;requested.push_back(range);if(fetchGate&&fetchCalls==blockFetch)fetchGate->block(stop);
        if(fetchFailure!=UpdateFailure::None)throw ProviderFailure(fetchFailure);
        FetchedWindow out{range,sha256("fixture response"),{}};
        if(!empty&&!(omitLatest&&range.from=="2003-01-03")){auto r=row(std::stoi(range.from.substr(8)));if(malformed)r.extra=1;out.rows.push_back(r);}
        if(wrongRange)out.range.through="2003-01-09";return out;
    }
};
void waitDone(UpdateWorker& worker) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(worker.busy()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    check(!worker.busy(),"Update worker finished within fixture deadline");
}
void saveFailure(const std::filesystem::path& root) {
    { Archive archive(root); }
    sqlite3* db=nullptr;const auto name=(root/"statistics/results.sqlite3").u8string();
    check(sqlite3_open(reinterpret_cast<const char*>(name.c_str()),&db)==SQLITE_OK,"Open isolated failure fixture");
    const auto rc=sqlite3_exec(db,"CREATE TRIGGER fail_update_save BEFORE INSERT ON fetched_windows BEGIN SELECT RAISE(ABORT,'fixture save failure'); END",nullptr,nullptr,nullptr);sqlite3_close(db);
    check(rc==SQLITE_OK,"Install test-only transaction failure");
    FixtureProvider provider;const auto result=executeUpdate(root,&provider);Archive archive(root);
    check(result.failure==UpdateFailure::Storage&&!result.committed&&result.attemptStatusSaved,"Save error classified separately from source error");
    check(archive.updateAttempts().back().status=="failed"&&archive.revisions().empty()&&archive.committedWindows().empty(),"Save failure records failed attempt after result/receipt rollback");
}
void lifecycle(const std::filesystem::path& root) {
    Archive archive(root);const auto policy=FixtureProvider{}.policy();
    const auto id=archive.startUpdate(policy.sourceId,policy.version);
    check(archive.updateAttempts().back().status=="pending"&&archive.committedWindows().empty(),"Pre-connectivity attempt durable without inferred coverage");
    rejects([&]{archive.commitUpdate(id,{{policy.available,sha256("empty"),{}}});},"Unplanned attempt cannot commit");
    auto plan=planUpdate(policy,archive.snapshot(),{});auto wrong=plan;wrong.policyVersion="different";
    rejects([&]{archive.setUpdatePlan(id,wrong);},"Cannot swap policy after connection check");
    wrong=plan;wrong.sourceId="other";
    rejects([&]{archive.setUpdatePlan(id,wrong);},"Cannot swap source after connection check");
    archive.setUpdatePlan(id,plan);
    rejects([&]{archive.setUpdatePlan(id,plan);},"Cannot replace attached request plan");
    archive.finishUpdate(id,false,"fixture incomplete download");
    const auto stale=archive.startUpdate(policy.sourceId,policy.version);archive.ingest({row(1)});
    rejects([&]{archive.setUpdatePlan(stale,plan);},"Concurrent import invalidates pre-connectivity snapshot");
    const auto older=archive.startUpdate(policy.sourceId,policy.version);
    const auto newer=archive.startUpdate(policy.sourceId,policy.version);
    plan=planUpdate(policy,archive.snapshot(),{});
    rejects([&]{archive.setUpdatePlan(older,plan);},"Newer attempt supersedes unplanned older worker");
    std::stop_source stop;stop.request_stop();
    rejects([&]{archive.setUpdatePlan(newer,plan,stop.get_token());},"Plan attachment is cancellable");
    check(archive.committedWindows().empty(),"No planning path creates receipts");
}
void pipeline(const std::filesystem::path& root) {
    FixtureProvider provider;provider.allowed=false;
    const auto disabled=executeUpdate(root/"disabled",&provider);
    check(disabled.stage==UpdateStage::Disabled&&!std::filesystem::exists(root/"disabled")&&provider.latestCalls==0,"Disabled provider makes no DB or network call");
    check(executeUpdate(root/"null",nullptr).stage==UpdateStage::Disabled&&!std::filesystem::exists(root/"null"),"Absent production provider gated before storage");
    provider.allowed=true;std::vector<UpdateStage> stages;
    auto result=executeUpdate(root/"success",&provider,[&](const UpdateProgress& p){stages.push_back(p.stage);});
    check(result.committed&&result.committed->comparison.added==3&&provider.latestCalls==1&&provider.fetchCalls==3,"One latest check plus initial bounded backfill");
    check(stages==std::vector<UpdateStage>{UpdateStage::Checking,UpdateStage::Comparing,UpdateStage::Downloading,UpdateStage::Downloading,UpdateStage::Downloading,UpdateStage::Downloading,UpdateStage::Validating,UpdateStage::Saving,UpdateStage::Succeeded},"Ordered progress from connection check to durable success");
    Archive archive(root/"success");const auto digest=archive.snapshot().digest();const auto version=archive.version();
    check(archive.updateAttempts().back().status=="succeeded"&&archive.committedWindows().size()==3,"Worker pipeline publishes receipt and success metadata");
    result=executeUpdate(root/"success",&provider);
    check(result.committed&&result.committed->comparison.unchanged==1&&provider.fetchCalls==4&&provider.requested.back()==DateRange{"2003-01-03","2003-01-03"},"Repeated check downloads recent overlap only");
    check(archive.version()==version&&archive.snapshot().digest()==digest,"No-change worker run preserves dataset version and digest");
    check(updateResultText(result).find("No new records in checked ranges")!=std::string::npos,"No-change text makes no complete-inventory claim");
    const auto successText=updateHistoryText(archive.updateAttempts());
    check(successText.find("Last successful check:")!=std::string::npos&&successText.find("unchanged 1")!=std::string::npos,"Successful check timestamp and counts visible");
    for(const auto category:{UpdateFailure::Connection,UpdateFailure::Tls,UpdateFailure::Denied,UpdateFailure::RateLimited,UpdateFailure::Schema}){
        provider.latestFailure=category;const auto before=archive.committedWindows().size();result=executeUpdate(root/"success",&provider);
        check(result.stage==UpdateStage::Failed&&result.failure==category&&!result.committed,"Source failure categories remain distinct");
        check(archive.updateAttempts().back().status=="failed"&&archive.committedWindows().size()==before&&archive.snapshot().digest()==digest,"Pre-planning failure recorded without changing accepted state");
        check(!archive.updateAttempts().back().startedUtc.empty()&&!archive.updateAttempts().back().finishedUtc.empty(),"Failed source check retains attempt and completion time");
    }
    auto history=updateHistoryText(archive.updateAttempts());
    check(history.find("(failed)")!=std::string::npos&&history.find("Last success counts: new 0, unchanged 1")!=std::string::npos,"Last attempted failure does not replace last successful check");
    provider.latestFailure=UpdateFailure::None;
    for(int kind=0;kind<4;++kind){
        FixtureProvider invalid;invalid.malformed=kind==0;invalid.omitLatest=kind==1;invalid.wrongRange=kind==2;
        if(kind==3)invalid.fetchFailure=UpdateFailure::RateLimited;
        const auto path=root/("bad-"+std::to_string(kind));const auto failed=executeUpdate(path,&invalid);Archive rejected(path);
        check(!failed.committed&&failed.stage==UpdateStage::Failed,"Malformed, missing latest, wrong range or source error rejected");
        check(rejected.revisions().empty()&&rejected.committedWindows().empty()&&rejected.updateAttempts().back().status=="failed","No partial results or receipts after invalid response");
    }
    FixtureProvider empty;empty.empty=true;result=executeUpdate(root/"empty",&empty);
    Archive emptyArchive(root/"empty");check(result.committed&&emptyArchive.snapshot().records().empty()&&emptyArchive.committedWindows().size()==3,"Schema-validated empty windows are successful checks, not null errors");
    for(const auto target:{UpdateStage::Checking,UpdateStage::Comparing,UpdateStage::Downloading,UpdateStage::Validating,UpdateStage::Saving}){
        FixtureProvider fixture;std::stop_source stop;const auto path=root/("cancel-"+std::to_string(int(target)));
        result=executeUpdate(path,&fixture,[&](const UpdateProgress& p){if(p.stage==target)stop.request_stop();},stop.get_token());
        Archive cancelled(path);check(result.stage==UpdateStage::Cancelled&&!result.committed,"Cancellation before commit reported truthfully");
        check(cancelled.revisions().empty()&&cancelled.committedWindows().empty()&&cancelled.updateAttempts().back().status=="cancelled","Cancellation at every pipeline stage preserves archive");
        check(executeUpdate(path,&fixture).committed.has_value(),"Cancelled windows remain retryable");
    }
    FixtureProvider fixture;std::stop_source late;
    result=executeUpdate(root/"late",&fixture,[&](const UpdateProgress& p){if(p.stage==UpdateStage::Succeeded)late.request_stop();},late.get_token());
    check(late.stop_requested()&&result.committed&&result.stage==UpdateStage::Succeeded,"Post-commit cancellation cannot erase successful pipeline result");
    check(executeUpdate(root/"callback",&fixture,[](const UpdateProgress&){throw std::runtime_error("presentation failed");}).committed.has_value(),"Progress callback failure cannot reinterpret a committed transaction");
    ViewData view;auto state=archive.readState();view.snapshot=std::make_shared<const Snapshot>(state.snapshot);view.coverage=coverage(*view.snapshot);view.revisions=state.revisions;view.updateAttempts=state.attempts;
    const auto sources=present(view,Page::Sources);
    check(sources.explanation.find("Last successful check:")!=std::string::npos&&sources.explanation.find("(failed)")!=std::string::npos,"Sources presentation contains durable freshness and failure status");
    check(updateHistoryText({}).find("never attempted")!=std::string::npos,"Legacy/import-only cache has no invented successful check");
    auto unfinished=state.attempts;unfinished.back().status="pending";
    check(updateHistoryText(unfinished).find("unfinished; success not established")!=std::string::npos,"Interrupted attempt not mistaken for ongoing or successful update");
}
void workers(const std::filesystem::path& root) {
    UpdateWorker unavailable;check(!unavailable.available()&&!unavailable.start(root/"disabled"),"Default worker cannot enable an unaudited provider");
    for(int phase=0;phase<2;++phase){
        auto provider=std::make_shared<FixtureProvider>();auto gate=std::make_shared<Gate>();
        if(phase==0)provider->latestGate=gate;else{provider->fetchGate=gate;provider->blockFetch=2;}
        UpdateWorker worker(provider);const auto path=root/("switch-"+std::to_string(phase));
        check(worker.start(path),"User-initiated update starts");gate->wait();
        check(!worker.start(path),"Double-click cannot queue another fetch");
        { Archive reader(path);check(reader.snapshot().records().empty()&&reader.updateAttempts().back().status=="pending","Cached archive remains readable during blocked provider work"); }
        const auto generation=worker.progress().generation;worker.cancel();waitDone(worker);auto result=worker.takeResult();
        check(result&&result->generation==generation&&result->stage==UpdateStage::Cancelled,"Mode-switch-equivalent cancellation retains terminal outcome");
        { Archive reader(path);check(reader.committedWindows().empty()&&reader.revisions().empty()&&reader.updateAttempts().back().status=="cancelled","Cancelled in-flight worker publishes no partial history"); }
        gate->release();check(worker.start(path),"New explicit request can retry after cancellation");waitDone(worker);worker.cancel();result=worker.takeResult();
        check(result&&result->committed&&result->generation>generation,"Late UI cancellation does not discard committed newer result");
        check(!worker.takeResult(),"Terminal result delivered once");worker.close();check(!worker.start(path),"Closed worker rejects new work");
    }
    auto provider=std::make_shared<FixtureProvider>();provider->latestGate=std::make_shared<Gate>();
    { UpdateWorker worker(provider);check(worker.start(root/"close"),"Close fixture starts");provider->latestGate->wait();worker.close();check(!worker.busy(),"Close cancels and joins provider worker before view destruction"); }
    Archive afterClose(root/"close");check(afterClose.updateAttempts().back().status=="cancelled"&&afterClose.committedWindows().empty(),"Close persists cancellation without touching accepted coverage");
    auto staleProvider=std::make_shared<FixtureProvider>();staleProvider->latestGate=std::make_shared<Gate>();UpdateWorker stale(staleProvider);
    check(stale.start(root/"stale"),"Concurrent writer fixture starts");staleProvider->latestGate->wait();
    { Archive writer(root/"stale");writer.ingest({row(1)}); }
    staleProvider->latestGate->release();waitDone(stale);const auto result=stale.takeResult();
    check(result&&result->failure==UpdateFailure::Storage&&!result->committed,"Stale pre-connection snapshot is not mislabeled source schema failure");
    Archive unchanged(root/"stale");check(unchanged.snapshot().records().size()==1&&unchanged.committedWindows().empty(),"Stale worker preserves concurrent writer result without fetched coverage");
}
}
size_t testUpdateWorker(){
    const auto root=std::filesystem::temp_directory_path()/("marksix-update-worker-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(root),"Fresh isolated worker fixtures");lifecycle(root/"lifecycle");pipeline(root/"pipeline");workers(root/"workers");saveFailure(root/"save-failure");
    std::cout<<"PASS update pipeline/progress/attempt freshness/cancellation/lifetime; fixtures="<<root.string()<<" network=none physics=none\n";return checks;
}
