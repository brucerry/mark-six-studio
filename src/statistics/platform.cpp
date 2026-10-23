#include "platform.hpp"
#include <chrono>
#include <climits>
#include <ctime>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#include <psapi.h>
#else
#include <cstdlib>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace marksix::statistics::platform {
bool validUtf8(std::string_view bytes) {
    for (size_t i = 0; i < bytes.size();) {
        const auto first = static_cast<unsigned char>(bytes[i++]);
        if (first < 0x80) continue;
        unsigned value = 0, minimum = 0, count = 0;
        if (first >= 0xc2 && first <= 0xdf) { value = first & 31; count = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { value = first & 15; count = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { value = first & 7; count = 3; minimum = 0x10000; }
        else return false;
        if (count > bytes.size() - i) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(bytes[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
std::string utcNow() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &now)) throw std::runtime_error("UTC clock unavailable");
#else
    if (!gmtime_r(&now, &utc)) throw std::runtime_error("UTC clock unavailable");
#endif
    char text[32]{};
    if (!std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc)) throw std::runtime_error("UTC conversion failed");
    return text;
}
void randomBytes(void* destination, size_t count) {
#ifdef _WIN32
    if (count > ULONG_MAX || BCryptGenRandom(nullptr, static_cast<PUCHAR>(destination), ULONG(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        throw std::runtime_error("System random source unavailable");
#else
    arc4random_buf(destination, count);
#endif
}
void reserveFile(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot reserve migration backup");
    CloseHandle(handle);
#else
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) throw std::runtime_error("Cannot reserve migration backup");
    close(descriptor);
#endif
}
size_t peakMemoryBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{}; memory.cb = sizeof memory;
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory) ? memory.PeakWorkingSetSize : 0;
#else
    rusage usage{};
    return getrusage(RUSAGE_SELF, &usage) == 0 ? size_t(usage.ru_maxrss) : 0;
#endif
}
double cpuMilliseconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return double(k.QuadPart + u.QuadPart) / 10000.0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage)) return -1;
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
        double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
#endif
}
}
