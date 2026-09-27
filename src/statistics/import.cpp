#include "import.hpp"
#include <Windows.h>
#include <sqlite3.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <stdexcept>

namespace marksix::statistics {
namespace {
void ensure(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
void utf8(std::string_view bytes) {
    ensure(bytes.size() <= MaxImportBytes && bytes.find('\0') == std::string_view::npos, "Oversized input or NUL byte");
    ensure(bytes.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), nullptr, 0) > 0, "Input is not valid UTF-8");
}
int integer(const std::string& s) {
    int value = 0; const auto parsed = std::from_chars(s.data(), s.data() + s.size(), value);
    ensure(!s.empty() && parsed.ec == std::errc{} && parsed.ptr == s.data() + s.size(), "Malformed CSV integer"); return value;
}
}
std::string readBounded(const std::filesystem::path& file, size_t maximum) {
    ensure(maximum <= MaxImportBytes, "Invalid file bound");
    std::ifstream input(file, std::ios::binary | std::ios::ate); ensure(bool(input), "Cannot open import file");
    const auto size = input.tellg(); ensure(size >= 0 && uint64_t(size) <= maximum, "Import file exceeds size limit");
    std::string bytes(size_t(size), '\0'); input.seekg(0);
    ensure(bytes.empty() || bool(input.read(bytes.data(), std::streamsize(bytes.size()))), "Truncated import file");
    ensure(input.peek() == std::char_traits<char>::eof(), "Import file changed while reading"); return bytes;
}
std::vector<std::vector<std::string>> parseCsv(std::string_view bytes) {
    utf8(bytes); if (bytes.starts_with("\xef\xbb\xbf")) bytes.remove_prefix(3);
    std::vector<std::vector<std::string>> rows; std::vector<std::string> row; std::string field;
    bool quoted = false, closed = false, began = false;
    auto endField = [&] {
        ensure(row.size() < 32, "Too many CSV columns"); row.push_back(field); field.clear(); began = false; closed = false;
    };
    auto endRow = [&] { endField(); ensure(rows.size() <= MaxImportRows, "Too many CSV rows"); rows.push_back(row); row.clear(); };
    for (size_t at = 0; at < bytes.size(); ++at) {
        const char c = bytes[at];
        if (quoted) {
            if (c == '"') {
                if (at + 1 < bytes.size() && bytes[at + 1] == '"') { field += '"'; ++at; }
                else { quoted = false; closed = true; }
            } else field += c;
        } else if (c == ',') endField();
        else if (c == '\n' || c == '\r') {
            if (c == '\r') { ensure(at + 1 < bytes.size() && bytes[at + 1] == '\n', "Bare CR outside CSV quote"); ++at; }
            endRow();
        } else if (c == '"' && !began && field.empty() && !closed) { quoted = true; began = true; }
        else { ensure(!closed && c != '"', "Malformed CSV quoting"); field += c; began = true; }
        ensure(field.size() <= 2048, "CSV field exceeds limit");
    }
    ensure(!quoted, "Unterminated CSV quote");
    if (began || closed || !field.empty() || !row.empty()) endRow();
    return rows;
}
std::map<std::string, std::string> parseManifest(std::string_view json) {
    ensure(json.size() <= 65536, "Manifest exceeds size limit"); utf8(json);
    sqlite3* db = nullptr; ensure(sqlite3_open(":memory:", &db) == SQLITE_OK, "Cannot initialize JSON parser");
    sqlite3_stmt* statement = nullptr;
    try {
        ensure(sqlite3_prepare_v2(db, "SELECT json_valid(?1), CASE WHEN json_valid(?1) THEN json_type(?1) ELSE '' END", -1, &statement, nullptr) == SQLITE_OK, "Cannot prepare manifest validation");
        sqlite3_bind_text(statement, 1, json.data(), int(json.size()), SQLITE_TRANSIENT);
        ensure(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1 &&
               std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 1))) == "object", "Manifest must be a strict JSON object");
        sqlite3_finalize(statement); statement = nullptr;
        ensure(sqlite3_prepare_v2(db, "SELECT key,type,atom FROM json_each(?1)", -1, &statement, nullptr) == SQLITE_OK, "Cannot parse manifest");
        sqlite3_bind_text(statement, 1, json.data(), int(json.size()), SQLITE_TRANSIENT);
        std::map<std::string, std::string> out; int rc;
        const auto text = [&](int col) { return std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, col)), size_t(sqlite3_column_bytes(statement, col))); };
        while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
            ensure(text(1) == "text", "Manifest fields must be strings");
            ensure(out.size() < 16 && out.emplace(text(0), text(2)).second, "Duplicate/excess manifest fields");
        }
        ensure(rc == SQLITE_DONE, "Invalid manifest JSON"); sqlite3_finalize(statement); sqlite3_close(db); return out;
    } catch (...) { sqlite3_finalize(statement); sqlite3_close(db); throw; }
}
std::vector<Result> parseImport(std::string_view csv, std::string_view manifest) {
    const auto meta = parseManifest(manifest);
    const std::set<std::string> keys{"schema", "csvSha256", "sourceId", "url", "retrievedUtc", "lineage", "status", "extractionOrder"};
    ensure(meta.size() == keys.size(), "Unexpected/missing manifest fields");
    for (const auto& [key, value] : meta) { (void)value; ensure(keys.contains(key), "Unknown manifest field"); }
    ensure(meta.at("schema") == "marksix-csv-v1", "Unknown import schema");
    ensure(meta.at("csvSha256") == sha256(csv), "CSV digest does not match manifest");
    ensure(meta.at("status") == "unverified", "An imported manifest cannot assert official/corroborated source status");
    ensure(meta.at("extractionOrder") == "unknown", "CSV main columns do not establish extraction order");
    const auto table = parseCsv(csv);
    const std::vector<std::string> header{"draw_id","date","n1","n2","n3","n4","n5","n6","extra","era","provider_id","source_window"};
    ensure(!table.empty() && table.front() == header, "Unsupported CSV header");
    std::vector<Result> rows;
    for (size_t line = 1; line < table.size(); ++line) {
        try {
            const auto& fields = table[line]; ensure(fields.size() == header.size(), "Wrong CSV field count");
            Result r; r.id = fields[0]; r.date = fields[1];
            for (size_t n = 0; n < 6; ++n) r.main[n] = integer(fields[n + 2]); r.extra = integer(fields[8]);
            ensure(fields[9] == "49-number" || fields[9] == "earlier-era-review-required", "Unknown CSV era");
            r.era = fields[9] == "49-number" ? Era::Current49 : Era::EarlierUnreviewed;
            // Provider/window are retained inertly as claimed lineage, not authentication.
            r.source = {meta.at("sourceId"),meta.at("url"),meta.at("retrievedUtc"),meta.at("csvSha256"),
                meta.at("lineage") + "; provider=" + fields[10] + "; window=" + fields[11], Trust::Unverified, ""};
            rows.push_back(canonicalize(std::move(r)));
        } catch (const std::exception& e) { throw std::invalid_argument("CSV record " + std::to_string(line + 1) + ": " + e.what()); }
    }
    return rows;
}
ImportPreview previewImport(const Archive& archive, std::string_view csv, std::string_view manifest) {
    ImportPreview out; out.archiveVersion = archive.version(); out.rows = parseImport(csv, manifest); out.csvDigest = sha256(csv);
    const auto existing = archive.revisions(); std::set<std::string> hashes; std::map<std::string, Result> known;
    for (const auto& r : existing) { hashes.insert(r.digest); if (r.active) known.emplace(drawKey(r.result), r.result); }
    for (const auto& r : out.rows) {
        if (r.era == Era::EarlierUnreviewed) ++out.earlierQuarantined;
        if (!hashes.insert(revisionDigest(r)).second) { ++out.duplicates; continue; }
        const auto found = known.find(drawKey(r));
        if (found != known.end()) {
            const auto& old = found->second;
            if (old.date != r.date || old.era != r.era || old.main != r.main || old.extra != r.extra) ++out.conflicts;
        } else known.emplace(drawKey(r), r);
    }
    if (archive.version() != out.archiveVersion) throw std::runtime_error("Archive changed during import preview; preview again");
    return out;
}
ImportOutcome acceptImport(Archive& archive, const ImportPreview& preview, std::stop_token stop) {
    // Revalidate the boundary even if a caller accidentally edits a preview in memory.
    for (const auto& r : preview.rows) if (r.source.trust != Trust::Unverified || r.source.contentSha256 != preview.csvDigest)
        throw std::invalid_argument("Import preview cannot promote source status or change provenance");
    return archive.ingest(preview.rows, stop, preview.archiveVersion);
}
}
