#pragma once
#include "storage_inventory.hpp"

namespace marksix::maintenance {
void syncDirectory(const fs::path& path);
void makePrivateDirectory(const fs::path& path); // Exclusive, never adopts an existing directory.
void durableWrite(const fs::path& path, std::string_view bytes);
std::string readBoundedFile(const fs::path& path, std::size_t maximum = 16 * 1024 * 1024);
void durableRename(const fs::path& from, const fs::path& to); // Never replaces an existing entry.
void durableRemove(const fs::path& path); // Single entry, never recursive.
bool entryExists(const fs::path& path); // Includes links; errors are not absence.
}
