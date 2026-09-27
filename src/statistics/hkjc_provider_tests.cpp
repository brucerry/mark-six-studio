#include "hkjc_provider.hpp"
#include "hkjc_query_fixture.hpp"
#include "import.hpp"
#include <chrono>
#include <iostream>
#include <set>

using namespace marksix::statistics;
namespace {
size_t checks = 0;
void check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action, UpdateFailure expected = UpdateFailure::Schema) {
    bool rejected = false;
    try { action(); } catch (const ProviderFailure& failure) { rejected = failure.category == expected; }
    check(rejected, "HKJC failure must have expected category");
}
std::string jsonQuoted(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else { if (c == '\\' || c == '"') out += '\\'; out += c; }
    }
    return out + '"';
}
std::string script() {
    const std::string query = HkjcQueryFixture;
    const auto tail = query.find("\n        query marksixResult");
    return "maxSelectMonths:3,minDate:new Date(\"1993/1/1\");" + jsonQuoted(query.substr(9, tail - 9)) + ";" + jsonQuoted(query.substr(tail));
}
constexpr const char* page = "<script src=\"/static/js/main.abc123.js\"></script>";
constexpr const char* row = R"({"id":"synthetic-id","year":"2026","no":1,"status":"Result","drawDate":"2026-09-22+08:00","drawResult":{"drawnNo":[6,5,4,3,2,1],"xDrawnNo":7}})";
std::string response() { return std::string("{\"data\":{\"lotteryDraws\":[") + row + "]}}"; }
std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from); check(at != std::string::npos, "Fixture replacement anchor"); text.replace(at, from.size(), to); return text;
}
constexpr const char* retrieved = "2026-09-25T00:00:00Z";
const DateRange range{"2026-09-01", "2026-09-25"};
}
size_t testHkjcProvider() {
    check(hkjcAssetPath(page) == "/static/js/main.abc123.js", "Pinned-host relative asset extraction");
    check(hkjcQuery(script()) == HkjcQueryFixture, "Saved public query string extraction/hash");
    rejects([] { hkjcAssetPath("<script src=\"https://other.invalid/main.js\">"); });
    rejects([] { hkjcAssetPath(std::string(page) + page); });
    rejects([] { hkjcAssetPath("src=\"/static/js/main.../escape.js\""); });
    rejects([] { hkjcQuery(replaced(script(), "maxSelectMonths:3", "maxSelectMonths:4")); });
    rejects([] { hkjcQuery(replaced(script(), "drawnNo", "otherNo")); });
    rejects([] { hkjcQuery(script() + script()); });
    const auto rows = parseHkjcResults(response(), range, retrieved);
    check(rows.size() == 1 && rows[0].id == "26/001" && rows[0].main[0] == 1 && rows[0].extra == 7, "Strict result fields and unordered canonical set");
    check(rows[0].source.trust == Trust::OfficialRetrieval && rows[0].source.contentSha256 == sha256(response()) && !rows[0].extractionOrder, "Provider provenance and no invented extraction order");
    check(parseHkjcResults("{\"data\":{\"lotteryDraws\":[]}}", range, retrieved).empty(), "Valid empty window distinct from null");
    for (const auto bad : {"{}", "{\"data\":null}", "{\"data\":{\"lotteryDraws\":null}}", "{\"data\":{\"lotteryDraws\":{}}}", "<html>denied</html>",
                           "{\"data\":{\"lotteryDraws\":[]},\"errors\":[{\"message\":\"denied\"}]}",
                           "{\"data\":{\"lotteryDraws\":[],\"lotteryDraws\":[]}}"})
        rejects([&] { parseHkjcResults(bad, range, retrieved); });
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
        {"[6,5,4,3,2,1]", "[6,5,4,3,2,2]"}, {"[6,5,4,3,2,1]", "[6,5,4,3,2]"}, {"[6,5,4,3,2,1]", "[6,5,4,3,2,1,8]"},
        {"[6,5,4,3,2,1]", "[6,5,4,3,2,1.0]"}, {"[6,5,4,3,2,1]", "[6,5,4,3,2,\"1\"]"},
        {"\"xDrawnNo\":7", "\"xDrawnNo\":6"}, {"\"xDrawnNo\":7", "\"xDrawnNo\":50"},
        {"Result", "Selling"}, {"2026-09-22+08:00", "2026-09-22+00:00"}, {"2026-09-22+08:00", "2026-02-30+08:00"},
        {"\"year\":\"2026\"", "\"year\":\"2025\""}, {"\"no\":1", "\"no\":0"}, {"synthetic-id", ""}})
        rejects([&] { parseHkjcResults(replaced(response(), from, to), range, retrieved); });
    rejects([] { parseHkjcResults(response(), {"2026-09-23", "2026-09-25"}, retrieved); });
    rejects([] { parseHkjcResults(std::string("{\"data\":{\"lotteryDraws\":[") + row + "," + row + "]}}", range, retrieved); });
    rejects([] { parseHkjcResults(std::string(2 * 1024 * 1024 + 1, ' '), range, retrieved); });
    rejects([] { parseHkjcResults(response() + '\0', range, retrieved); });
    std::stop_source stopped; stopped.request_stop();
    rejects([&] { parseHkjcResults(response(), range, retrieved, stopped.get_token()); }, UpdateFailure::Cancelled);
    checkHkjcHttpStatus(200);
    rejects([] { checkHkjcHttpStatus(429); }, UpdateFailure::RateLimited);
    for (unsigned status : {301u, 302u, 401u, 403u}) rejects([&] { checkHkjcHttpStatus(status); }, UpdateFailure::Denied);
    rejects([] { checkHkjcHttpStatus(503); }, UpdateFailure::Connection);
    rejects([] { hkjcHttps({"example.invalid", "/", {}, 10}, {}); });
    rejects([&] { hkjcHttps({"bet.hkjc.com", "/en/marksix/results", {}, 10}, stopped.get_token()); }, UpdateFailure::Cancelled);
    size_t calls = 0; std::vector<std::string> bodies;
    auto provider = makeHkjcProviderForTest([&](const HkjcRequest& request, std::stop_token) {
        ++calls;
        if (request.path == "/en/marksix/results") return std::string(page);
        if (request.path.starts_with("/static/js/")) return script();
        bodies.push_back(request.body);
        if (request.body.find("\"lastNDraw\":1") != std::string::npos) return response();
        const auto pos = request.body.find("\"startDate\":\"");
        check(pos != std::string::npos, "Date query has start bound");
        const auto from = request.body.substr(pos + 13, 8);
        const auto end = request.body.find("\"endDate\":\"");
        const auto through = request.body.substr(end + 11, 8);
        return from <= "20260922" && through >= "20260922" ? response() : std::string("{\"data\":{\"lotteryDraws\":[]}}");
    }, "2026-09-25");
    check(calls == 0 && provider->enabled(), "Provider construction performs no request");
    check(provider->policy().maximumWindowDays == 89 && provider->policy().correctionOverlapDays == 14, "Versioned bounded policy");
    const auto root = std::filesystem::temp_directory_path() / ("marksix-hkjc-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto first = executeUpdate(root, provider.get());
    check(first.stage == UpdateStage::Succeeded && first.committed->comparison.added == 1, "HKJC provider fixture initial atomic backfill");
    Archive archive(root); const auto receipts = archive.committedWindows().size(), version = archive.version();
    check(receipts > 100 && archive.snapshot().records().size() == 1, "Initial backfill covers reviewed range including validated empty windows");
    bodies.clear(); const auto second = executeUpdate(root, provider.get());
    check(second.stage == UpdateStage::Succeeded && second.committed->comparison.added == 0 && second.committed->comparison.unchanged == 1, "HKJC repeated update is idempotent");
    check(bodies.size() == 2 && bodies.back().find("20260912") != std::string::npos && archive.version() == version, "Second update only latest plus 14-day overlap, no archive redownload");
    for (const auto category : {UpdateFailure::Connection, UpdateFailure::Tls, UpdateFailure::Denied, UpdateFailure::RateLimited, UpdateFailure::Schema}) {
        auto failing = makeHkjcProviderForTest([&](const HkjcRequest&, std::stop_token) -> std::string { throw ProviderFailure(category); }, "2026-09-25");
        const auto failed = executeUpdate(root, failing.get());
        check(failed.failure == category && !failed.committed && archive.version() == version, "Transport failure keeps cache and distinct category");
    }
    auto noRequest = makeHkjcProviderForTest([&](const HkjcRequest&, std::stop_token) { ++calls; return response(); }, "2026-09-25");
    const auto before = calls;
    rejects([&] { noRequest->latest(stopped.get_token()); }, UpdateFailure::Cancelled);
    check(calls == before, "Pre-cancelled fetch never reaches transport");
    std::cout << "PASS HKJC provider fixtures checks=" << checks << " network=none\n"; return checks;
}
void verifySavedHkjc(const std::filesystem::path& source, const std::filesystem::path& cache) {
    size_t windows = 0, rows = 0; std::set<std::string> identities; std::vector<Result> saved;
    for (const auto& file : std::filesystem::directory_iterator(source / "raw")) {
        const auto name = file.path().filename().string();
        if (name.size() != 22 || !name.ends_with(".json")) continue;
        const auto date = [&](size_t offset) { return name.substr(offset, 4) + "-" + name.substr(offset + 4, 2) + "-" + name.substr(offset + 6, 2); };
        auto parsed = parseHkjcResults(readBounded(file.path(), 2 * 1024 * 1024), {date(0), date(9)}, retrieved);
        for (const auto& result : parsed) check(identities.insert(result.id).second, "Saved official windows have unique identities");
        rows += parsed.size(); ++windows;
        saved.insert(saved.end(), parsed.begin(), parsed.end());
    }
    check(windows == 135 && rows == 4391, "All saved official windows parsed using desktop provider parser");
    std::cout << "HKJC_SAVED PASS windows=" << windows << " rows=" << rows << " network=none\n";
    if (!cache.empty()) {
        check(std::filesystem::is_regular_file(cache / "statistics/results.sqlite3"), "Existing real cache required");
        Archive archive(cache); const auto snapshot = archive.snapshot(); const auto comparison = compareUpdate(snapshot, saved);
        check(snapshot.records().size() == rows && comparison.unchanged == rows && !comparison.added && !comparison.conflicts, "Live button results match every independently verified saved result");
        std::cout << "HKJC_CACHE PASS matching=" << comparison.unchanged << " eligible=" << snapshot.select().size() << " network=none\n";
    }
}
