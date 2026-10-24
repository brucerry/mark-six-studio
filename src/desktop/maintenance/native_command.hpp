#pragma once
#include "native_runtime.hpp"
#include <chrono>

namespace marksix::maintenance {
struct NativeCommandResult {
    int status = -1;
    std::string output;
    std::optional<ProcessIdentity> process;
};
// Explicit executable + argv only. Callers must independently validate any
// product executable; no shell or command-language expansion is performed.
NativeCommandResult runNativeCommand(const fs::path& executable, const std::vector<std::string>& arguments,
    std::chrono::seconds timeout = std::chrono::seconds(30));
}
