#pragma once
#include "storage_inventory.hpp"

namespace marksix::maintenance {
// Records future custom-root discovery under the initiating native profile.
// Excludes developer archives and never upgrades discovery hints to ownership.
bool rememberWorkspace(const UserStorage& user, const fs::path& activeRoot);
void rememberCurrentWorkspace(const fs::path& activeRoot);
}
