#pragma once
#include "archive.hpp"
#include <map>

namespace marksix::statistics {
inline constexpr size_t MaxImportBytes = 16 * 1024 * 1024, MaxImportRows = 100000;
std::string readBounded(const std::filesystem::path& file, size_t maximum = MaxImportBytes);
std::vector<std::vector<std::string>> parseCsv(std::string_view input);
// Flat string-only object with duplicate-key rejection. No code/URL evaluation.
std::map<std::string, std::string> parseManifest(std::string_view json);
std::vector<Result> parseImport(std::string_view csv, std::string_view manifest);
struct ImportPreview {
    std::vector<Result> rows;
    size_t duplicates = 0, conflicts = 0, earlierQuarantined = 0;
    uint64_t archiveVersion = 0;
    std::string csvDigest;
};
ImportPreview previewImport(const Archive& archive, std::string_view csv, std::string_view manifest);
// Acceptance rechecks the archive generation inside the same write transaction.
ImportOutcome acceptImport(Archive& archive, const ImportPreview& preview, std::stop_token stop = {});
}
