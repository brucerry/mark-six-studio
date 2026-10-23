#include "statistics_data_root.hpp"
#ifdef _WIN32
#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#else
#include <QStandardPaths>
#endif
#include <stdexcept>

std::filesystem::path defaultStatisticsDataRoot() {
#ifdef _WIN32
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path)))
        throw std::runtime_error("Local AppData unavailable");
    const std::filesystem::path root(path);
    CoTaskMemFree(path);
    return root / "MarkSixEmulator";
#else
    const auto location = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (location.isEmpty()) throw std::runtime_error("Application Support unavailable");
    return std::filesystem::path(location.toStdU16String()) / "MarkSixStudio";
#endif
}
