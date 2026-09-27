#include "hkjc_provider.hpp"
#include <Windows.h>
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <set>

namespace marksix::statistics {
namespace {
void require(bool ok) { if (!ok) throw ProviderFailure(UpdateFailure::Schema); }
void cancelled(std::stop_token stop) { if (stop.stop_requested()) throw ProviderFailure(UpdateFailure::Cancelled); }
struct Db {
    sqlite3* value = nullptr;
    Db() { if (sqlite3_open(":memory:", &value) != SQLITE_OK) { sqlite3_close(value); throw ProviderFailure(UpdateFailure::Schema); } }
    ~Db() { sqlite3_close(value); }
};
struct Statement {
    sqlite3_stmt* value = nullptr;
    Statement(sqlite3* db, const char* sql, std::string_view bytes, std::string_view path = {}) {
        require(sqlite3_prepare_v2(db, sql, -1, &value, nullptr) == SQLITE_OK);
        sqlite3_bind_text(value, 1, bytes.data(), int(bytes.size()), SQLITE_TRANSIENT);
        if (!path.empty()) sqlite3_bind_text(value, 2, path.data(), int(path.size()), SQLITE_TRANSIENT);
    }
    ~Statement() { sqlite3_finalize(value); }
    std::string text(int col) const {
        const auto p = sqlite3_column_text(value, col);
        return p ? std::string(reinterpret_cast<const char*>(p), size_t(sqlite3_column_bytes(value, col))) : "";
    }
};
struct Json {
    Db db;
    std::string_view bytes;
    explicit Json(std::string_view b) : bytes(b) {
        require(!b.empty() && b.size() <= 2 * 1024 * 1024 && b.find('\0') == std::string_view::npos);
        require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b.data(), int(b.size()), nullptr, 0) > 0);
        Statement valid(db.value, "SELECT json_valid(?1)", b);
        require(sqlite3_step(valid.value) == SQLITE_ROW && sqlite3_column_int(valid.value, 0) == 1);
        Statement duplicates(db.value, "SELECT 1 FROM json_tree(?1) WHERE typeof(key)='text' GROUP BY parent,key HAVING count(*)>1 LIMIT 1", b);
        require(sqlite3_step(duplicates.value) == SQLITE_DONE);
    }
    std::pair<std::string, std::string> field(std::string_view path) {
        Statement s(db.value, "SELECT json_type(?1,?2),json_extract(?1,?2)", bytes, path);
        require(sqlite3_step(s.value) == SQLITE_ROW); return {s.text(0), s.text(1)};
    }
    std::string get(std::string_view path, std::string_view type) {
        auto f = field(path); require(f.first == type); return f.second;
    }
    std::vector<std::string> array(std::string_view path, size_t max) {
        get(path, "array"); Statement s(db.value, "SELECT value FROM json_each(?1,?2)", bytes, path);
        std::vector<std::string> values; int code;
        while ((code = sqlite3_step(s.value)) == SQLITE_ROW) { require(values.size() < max); values.push_back(s.text(0)); }
        require(code == SQLITE_DONE); return values;
    }
};
int decimal(std::string_view value, int low, int high) {
    require(!value.empty() && value.size() <= 4 && value.find_first_not_of("0123456789") == std::string_view::npos);
    int result = 0; for (const char c : value) result = result * 10 + c - '0';
    require(result >= low && result <= high); return result;
}
std::string quote(std::string_view value) {
    Db db; Statement s(db.value, "SELECT json_quote(?1)", value);
    require(sqlite3_step(s.value) == SQLITE_ROW); return s.text(0);
}
std::string literal(std::string_view script, std::string_view anchor) {
    const auto start = script.find(anchor); require(start != std::string_view::npos && script.find(anchor, start + 1) == std::string_view::npos);
    size_t end = start + 1;
    for (; end < script.size(); ++end) {
        if (script[end] == '\\') { ++end; continue; }
        if (script[end] == '"') break;
    }
    require(end < script.size() && end - start < 16384);
    Json json(script.substr(start, end - start + 1)); return json.get("$", "text");
}
std::string utcNow() {
    SYSTEMTIME t{}; GetSystemTime(&t); char value[32]{};
    std::snprintf(value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); return value;
}
std::string hongKongToday() {
    using namespace std::chrono;
    const year_month_day date{floor<days>(system_clock::now() + hours(8))}; char value[16]{};
    std::snprintf(value, sizeof(value), "%04d-%02u-%02u", int(date.year()), unsigned(date.month()), unsigned(date.day())); return value;
}
std::string compact(std::string date) { date.erase(std::remove(date.begin(), date.end(), '-'), date.end()); return date; }
class HkjcProvider final : public UpdateProvider {
    HkjcTransport transport_;
    std::string fixedToday_, query_;
    bool testing_;
    std::chrono::steady_clock::time_point next_{};
    std::string request(HkjcRequest req, std::stop_token stop) {
        cancelled(stop);
        if (!testing_) {
            std::mutex mutex; std::unique_lock lock(mutex); std::condition_variable_any wait;
            wait.wait_until(lock, stop, next_, [] { return false; }); cancelled(stop);
        }
        // Keep spacing even across failed attempts. No automatic retry.
        next_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto body = transport_(req, stop); cancelled(stop);
        require(body.size() <= req.maximumBytes); return body;
    }
    std::string results(const std::string& variables, std::stop_token stop) {
        require(!query_.empty());
        return request({"info.cld.hkjc.com", "/graphql/base/",
            "{\"operationName\":\"marksixResult\",\"query\":" + quote(query_) + ",\"variables\":" + variables + "}", 2 * 1024 * 1024}, stop);
    }
public:
    HkjcProvider(HkjcTransport transport, std::string today, bool testing)
        : transport_(std::move(transport)), fixedToday_(std::move(today)), testing_(testing) {}
    bool enabled() const override { return true; }
    UpdatePolicy policy() const override {
        return {HkjcSource, HkjcPolicy, {"1993-01-01", fixedToday_.empty() ? hongKongToday() : fixedToday_}, 89, 14};
    }
    std::optional<Result> latest(std::stop_token stop) override {
        query_.clear();
        const auto page = request({"bet.hkjc.com", "/en/marksix/results", {}, 2 * 1024 * 1024}, stop);
        const auto script = request({"bet.hkjc.com", hkjcAssetPath(page), {}, 16 * 1024 * 1024}, stop);
        query_ = hkjcQuery(script);
        const auto body = results("{\"lastNDraw\":1,\"startDate\":null,\"endDate\":null,\"drawType\":\"All\"}", stop);
        auto rows = parseHkjcResults(body, policy().available, utcNow(), stop);
        require(rows.size() == 1); // Known populated source; empty/latest-null is not evidence of coverage.
        return rows.front();
    }
    FetchedWindow fetch(const DateRange& range, std::stop_token stop) override {
        const auto available = policy().available;
        require(validDate(range.from) && validDate(range.through) && range.from <= range.through && range.from >= available.from && range.through <= available.through);
        // Reuse the planner's own range-bound validation rather than trust callers.
        const auto probe = planUpdate({HkjcSource, HkjcPolicy, range, 89, 1}, Snapshot({}), {});
        require(probe.requests.size() == 1);
        const auto body = results("{\"lastNDraw\":null,\"startDate\":\"" + compact(range.from) + "\",\"endDate\":\"" + compact(range.through) + "\",\"drawType\":\"All\"}", stop);
        return {range, sha256(body), parseHkjcResults(body, range, utcNow(), stop)};
    }
};
}
std::string hkjcAssetPath(std::string_view page) {
    constexpr std::string_view anchor = "src=\"/static/js/main.";
    const auto start = page.find(anchor); require(start != std::string_view::npos && page.find(anchor, start + 1) == std::string_view::npos);
    const auto end = page.find('"', start + anchor.size()); require(end != std::string_view::npos);
    const auto path = page.substr(start + 5, end - start - 5);
    require(path.size() < 100 && path.ends_with(".js"));
    const auto hash = path.substr(16, path.size() - 19);
    require(!hash.empty() && hash.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == std::string_view::npos);
    return std::string(path);
}
std::string hkjcQuery(std::string_view script) {
    require(script.find("maxSelectMonths:3,minDate:new Date(\"1993/1/1\")") != std::string_view::npos);
    const auto query = "\n        " + literal(script, "\"fragment lotteryDrawsFragment") + literal(script, "\"\\n        query marksixResult");
    // Reviewed exact public query, not arbitrary downloaded GraphQL/code execution.
    require(sha256(query) == "156c024a7a77406c6bf2127dcb5b029ba83b692f501aa3635911f7bdc029724d"); return query;
}
std::vector<Result> parseHkjcResults(std::string_view bytes, const DateRange& range, std::string_view retrievedUtc, std::stop_token stop) {
    cancelled(stop); Json payload(bytes); payload.get("$", "object");
    const auto errors = payload.field("$.errors");
    require(errors.first.empty() || (errors.first == "array" && payload.array("$.errors", 1).empty()));
    const auto rows = payload.array("$.data.lotteryDraws", 1000); const auto hash = sha256(bytes);
    std::vector<Result> out; std::set<std::string> ids, providerIds;
    for (const auto& bytesRow : rows) {
        cancelled(stop); Json row(bytesRow); row.get("$", "object"); require(row.get("$.status", "text") == "Result");
        const auto date = row.get("$.drawDate", "text"); require(date.size() == 16 && date.ends_with("+08:00"));
        Result r; r.date = date.substr(0, 10);
        require(validDate(r.date) && r.date >= range.from && r.date <= range.through);
        const auto year = row.field("$.year"); require(year.first == "text" || year.first == "integer");
        require(decimal(year.second, 1993, 9999) == decimal(std::string_view(r.date).substr(0, 4), 1993, 9999));
        const int no = decimal(row.get("$.no", "integer"), 1, 999); char id[16]{};
        std::snprintf(id, sizeof(id), "%s/%03d", r.date.substr(2, 2).c_str(), no); r.id = id;
        const auto provider = row.get("$.id", "text"); require(!provider.empty() && provider.size() <= 100 && providerIds.insert(provider).second && ids.insert(r.id).second);
        require(row.array("$.drawResult.drawnNo", 6).size() == 6);
        for (size_t i = 0; i < 6; ++i) r.main[i] = decimal(row.get("$.drawResult.drawnNo[" + std::to_string(i) + "]", "integer"), 1, 49);
        r.extra = decimal(row.get("$.drawResult.xDrawnNo", "integer"), 1, 49);
        r.era = r.date >= "2002-07-04" ? Era::Current49 : Era::EarlierUnreviewed;
        r.source = {HkjcSource, "https://bet.hkjc.com/en/marksix/results", std::string(retrievedUtc), hash,
                    "HKJC public results query; provider=" + provider, Trust::OfficialRetrieval,
                    std::string(HkjcPolicy) + "; validated HTTPS retrieval; response-sha256=" + hash};
        try { out.push_back(canonicalize(std::move(r))); } catch (...) { throw ProviderFailure(UpdateFailure::Schema); }
    }
    return out;
}
std::shared_ptr<UpdateProvider> makeHkjcProvider() { return std::make_shared<HkjcProvider>(hkjcHttps, "", false); }
std::shared_ptr<UpdateProvider> makeHkjcProviderForTest(HkjcTransport transport, std::string today) {
    require(validDate(today)); return std::make_shared<HkjcProvider>(std::move(transport), std::move(today), true);
}
}
