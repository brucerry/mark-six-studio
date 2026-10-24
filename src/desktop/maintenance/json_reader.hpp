#pragma once
#include <sqlite3.h>
#include <string>
#include <cstdint>
#include <stdexcept>

namespace marksix::maintenance {
// SQLite's bundled JSON parser runs SELECTs against a read-only in-memory
// connection. It never opens the archive, creates a disk database or runs SQL
// supplied by a document. Reuse the pinned parser instead of adding a runtime.
class JsonQuery {
public:
    explicit JsonQuery(sqlite3_stmt* statement) : statement_(statement) {}
    ~JsonQuery() { sqlite3_finalize(statement_); }
    JsonQuery(const JsonQuery&) = delete;
    bool next() {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_ROW && result != SQLITE_DONE) throw std::runtime_error("JSON inspection failed or exceeded its work budget");
        return result == SQLITE_ROW;
    }
    std::string text(int column) const {
        if (sqlite3_column_type(statement_, column) != SQLITE_TEXT) throw std::runtime_error("JSON field is not text");
        return {reinterpret_cast<const char*>(sqlite3_column_text(statement_, column)), std::size_t(sqlite3_column_bytes(statement_, column))};
    }
    std::int64_t integer(int column) const {
        if (sqlite3_column_type(statement_, column) != SQLITE_INTEGER) throw std::runtime_error("JSON field is not an integer");
        return sqlite3_column_int64(statement_, column);
    }
private:
    sqlite3_stmt* statement_;
};
class JsonDocument {
public:
    explicit JsonDocument(std::string bytes) : bytes_(std::move(bytes)) {
        if (bytes_.size() > 8 * 1024 * 1024) throw std::runtime_error("JSON document exceeds limit");
        if (sqlite3_open_v2(":memory:", &database_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
            sqlite3_close(database_); database_ = nullptr; throw std::runtime_error("JSON parser unavailable");
        }
        sqlite3_db_config(database_, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        sqlite3_db_config(database_, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
        sqlite3_limit(database_, SQLITE_LIMIT_LENGTH, 8 * 1024 * 1024);
        sqlite3_progress_handler(database_, 1000, [](void* self) -> int { return --static_cast<JsonDocument*>(self)->budget_ <= 0; }, this);
        try {
            auto valid = query("SELECT json_valid(?1, 1)");
            if (!valid.next() || valid.integer(0) != 1) throw std::runtime_error("Malformed JSON document");
            auto duplicate = query("SELECT 1 FROM json_tree(?1) GROUP BY fullkey HAVING count(*) > 1 LIMIT 1");
            if (duplicate.next()) throw std::runtime_error("Duplicate JSON object fields");
        } catch (...) { sqlite3_close(database_); database_ = nullptr; throw; }
    }
    ~JsonDocument() { sqlite3_close(database_); }
    JsonDocument(const JsonDocument&) = delete;
    JsonQuery query(const char* sql) {
        budget_ = 10000; sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) throw std::runtime_error("Invalid JSON inspection query");
        if (sqlite3_bind_text(statement, 1, bytes_.data(), int(bytes_.size()), SQLITE_STATIC) != SQLITE_OK) {
            sqlite3_finalize(statement); throw std::runtime_error("Cannot bind JSON document");
        }
        return JsonQuery(statement);
    }
private:
    std::string bytes_;
    sqlite3* database_ = nullptr;
    int budget_ = 0;
};
}
