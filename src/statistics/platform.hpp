#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace marksix::statistics::platform {
bool validUtf8(std::string_view bytes);
std::string utcNow();
void randomBytes(void* destination, size_t count);
void reserveFile(const std::filesystem::path& path);
size_t peakMemoryBytes();
double cpuMilliseconds();
}
