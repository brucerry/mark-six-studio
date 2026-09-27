#include "statistics_data_root.hpp"

#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>

#include <stdexcept>

std::filesystem::path defaultStatisticsDataRoot() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path)))
        throw std::runtime_error("Local AppData unavailable");
    const std::filesystem::path root(path);
    CoTaskMemFree(path);
    return root / "MarkSixEmulator";
}
