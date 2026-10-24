#include "storage_inventory.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace marksix::maintenance {
std::string quotedReportField(const std::string& value) {
    std::string result = "\"";
    for (std::size_t index = 0; index < value.size();) {
        const auto start = index;
        const auto byte = static_cast<unsigned char>(value[index++]);
        if (byte >= 128) {
            unsigned scalar = 0, minimum = 0, count = 0;
            if (byte >= 0xc2 && byte <= 0xdf) { scalar = byte & 31; minimum = 0x80; count = 1; }
            else if (byte >= 0xe0 && byte <= 0xef) { scalar = byte & 15; minimum = 0x800; count = 2; }
            else if (byte >= 0xf0 && byte <= 0xf4) { scalar = byte & 7; minimum = 0x10000; count = 3; }
            bool valid = count && count <= value.size() - index;
            for (unsigned offset = 0; valid && offset < count; ++offset) {
                const auto next = static_cast<unsigned char>(value[index + offset]);
                if ((next & 0xc0) != 0x80) valid = false;
                scalar = (scalar << 6) | (next & 63);
            }
            valid = valid && scalar >= minimum && scalar <= 0x10ffff && !(scalar >= 0xd800 && scalar <= 0xdfff);
            if (!valid) {
                result += "\\u00"; result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15];
            } else {
                index += count;
                if ((scalar >= 0x80 && scalar <= 0x9f) || scalar == 0x061c || scalar == 0x200e || scalar == 0x200f ||
                    (scalar >= 0x2028 && scalar <= 0x202e) || (scalar >= 0x2066 && scalar <= 0x2069) || scalar == 0xfeff) {
                    result += "\\u";
                    for (int shift = 12; shift >= 0; shift -= 4) result += "0123456789abcdef"[(scalar >> shift) & 15];
                } else result.append(value, start, count + 1);
            }
            continue;
        }
        if (byte == '\\' || byte == '"') { result += '\\'; result += char(byte); }
        else if (byte == '\n') result += "\\n";
        else if (byte == '\r') result += "\\r";
        else if (byte == '\t') result += "\\t";
        else if (byte < 32 || byte == 127) {
            result += "\\u00"; result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15];
        } else result += char(byte);
    }
    return result + '"';
}
bool legacyRecordingName(const fs::path& path) {
    auto name = pathUtf8(path.filename());
    for (auto& byte : name) if (byte >= 'A' && byte <= 'Z') byte = char(byte - 'A' + 'a');
    return name.ends_with(".msx");
}
bool intentionallyPreserved(const Inventory& inventory, const fs::path& path) {
    return std::any_of(inventory.preservedLegacy.begin(), inventory.preservedLegacy.end(),
        [&](const auto& item) {
            const auto relative = path.lexically_relative(item.path);
            return item.path == path || (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..");
        });
}
void preserveLegacyFile(Inventory& inventory, const fs::path& path, const std::string& account, const std::string& reason) {
    if (!std::any_of(inventory.preservedLegacy.begin(), inventory.preservedLegacy.end(),
        [&](const auto& item) { return item.path == path && item.account == account; }))
        inventory.preservedLegacy.push_back({path, account, reason});
    std::erase_if(inventory.targets, [&](const auto& target) { return intentionallyPreserved(inventory, target.path); });
}
void discoverPreservedLegacy(Inventory& inventory, const fs::path& root, const std::string& account) {
    try {
        validateContainedPath(root / ".legacy-inspection", root);
        std::error_code error;
        const auto rootStatus = fs::symlink_status(root, error);
        if (error == std::errc::no_such_file_or_directory || (!error && rootStatus.type() == fs::file_type::not_found)) return;
        if (error) throw fs::filesystem_error("Cannot inspect legacy location", root, error);
        if (legacyRecordingName(root)) { preserveLegacyFile(inventory, root, account); return; }
        if (!fs::is_directory(rootStatus)) throw std::runtime_error("Legacy inspection root is not an accessible physical directory");
        std::size_t count = 0;
        fs::recursive_directory_iterator entry(root, fs::directory_options::none, error), end;
        if (error) throw fs::filesystem_error("Cannot inspect legacy location", root, error);
        while (entry != end) {
            if (++count > 100000) throw std::runtime_error("Legacy discovery stopped at its file limit; listing may be incomplete");
            const auto path = entry->path();
            try {
                validateContainedPath(path, root);
                const auto status = entry->symlink_status();
                if (legacyRecordingName(path) || intentionallyPreserved(inventory, path)) {
                    preserveLegacyFile(inventory, path, account); entry.disable_recursion_pending();
                } else if (fs::is_directory(status)) {
                    validateContainedPath(path / ".legacy-inspection", root);
                    std::error_code access;
                    fs::directory_iterator probe(path, access);
                    if (access) throw fs::filesystem_error("Cannot inspect directory", path, access);
                } else if (fs::is_symlink(status)) {
                    entry.disable_recursion_pending();
                    const auto target = fs::read_symlink(path);
                    const auto relative = (path.parent_path() / target).lexically_normal().lexically_relative(root);
                    if (relative.empty() || *relative.begin() == "..")
                        inventory.issues.push_back({path, account, "Legacy listing did not follow this link; linked contents were not inspected", false});
                }
            } catch (const std::exception& failure) {
                entry.disable_recursion_pending();
                inventory.issues.push_back({path, account, std::string("Legacy listing incomplete: ") + failure.what(), false});
            }
            entry.increment(error);
            if (error) throw fs::filesystem_error("Legacy listing incomplete", path, error);
        }
    } catch (const std::exception& failure) {
        inventory.issues.push_back({root, account, std::string("Legacy listing incomplete: ") + failure.what(), false});
    }
}
std::string preservedLegacyReport(const Inventory& inventory) {
    std::ostringstream report;
    report << "Preserved legacy files (" << inventory.preservedLegacy.size() << ")\n";
    if (inventory.preservedLegacy.empty()) report << "No legacy recordings were found in the inspected locations.\n";
    auto files = inventory.preservedLegacy;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        if (a.account != b.account) return a.account < b.account;
        return a.path < b.path;
    });
    for (const auto& file : files)
        report << "\nAccount: " << quotedReportField(file.account.empty() ? "Shared installation" : file.account)
               << "\nPath: " << quotedReportField(pathUtf8(file.path)) << "\nReason: " << quotedReportField(file.reason) << '\n';
    if (!inventory.issues.empty()) {
        report << "\nInspection notes\n";
        for (const auto& issue : inventory.issues)
            report << "\nAccount: " << quotedReportField(issue.account.empty() ? "Shared installation" : issue.account)
                   << "\nLocation: " << quotedReportField(pathUtf8(issue.path)) << "\nReason: " << quotedReportField(issue.reason) << '\n';
    }
    return report.str();
}
}
