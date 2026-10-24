#include "storage_inventory.hpp"

namespace marksix::maintenance {
bool isDevelopmentLocation(const fs::path& path) {
    if (!path.is_absolute() || path != path.lexically_normal()) return true;
    // Running a bundle during development can register it with the OS. That
    // registration must never turn a source checkout/build tree into a target.
    for (auto directory = path; !directory.empty();) {
        for (const auto* marker : {".git", "CMakeCache.txt"}) {
            std::error_code error;
            const auto status = fs::symlink_status(directory / marker, error);
            if (error && error != std::errc::no_such_file_or_directory) return true;
            if (!error && status.type() != fs::file_type::not_found) return true;
        }
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return false;
}
}
