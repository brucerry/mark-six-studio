#include "durable_files.hpp"
#include "native_runtime.hpp"
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#endif

namespace marksix::maintenance {
namespace {
#ifdef _WIN32
// Hold every existing ancestor without FILE_SHARE_DELETE. A junction/rename
// cannot redirect a privileged path operation after this validation.
struct PinnedParent {
    std::vector<HANDLE> handles;
    explicit PinnedParent(const fs::path& path) {
        try {
            auto cursor = path.root_path();
            const auto pin = [&](const fs::path& part) {
                const auto handle = CreateFileW(part.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot pin maintenance ancestor");
                handles.push_back(handle); BY_HANDLE_FILE_INFORMATION info{};
                if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                    !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Redirected maintenance ancestor");
            };
            pin(cursor);
            for (const auto& part : path.relative_path()) { cursor /= part; pin(cursor); }
        } catch (...) { for (const auto handle : handles) CloseHandle(handle); throw; }
    }
    ~PinnedParent() { for (const auto handle : handles) CloseHandle(handle); }
};
#else
struct PinnedParent {
    int handle = -1;
    explicit PinnedParent(const fs::path& path) {
        handle = open(path.root_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (handle < 0) throw std::runtime_error("Cannot pin maintenance root");
        for (const auto& part : path.relative_path()) {
            const auto next = openat(handle, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            close(handle); handle = next;
            if (handle < 0) throw std::runtime_error("Cannot pin maintenance ancestor");
        }
    }
    ~PinnedParent() { if (handle >= 0) close(handle); }
};
#endif
}
bool entryExists(const fs::path& path) {
    std::error_code error; const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return false;
    if (error) throw fs::filesystem_error("Cannot inspect maintenance path", path, error);
    return status.type() != fs::file_type::not_found;
}
void syncDirectory(const fs::path& path) {
    validateContainedPath(path / ".sync", path);
#ifndef _WIN32
    const auto fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("Cannot open maintenance directory for sync");
    const int result = fsync(fd); close(fd);
    if (result) throw std::runtime_error("Cannot sync maintenance directory");
#endif
}
void makePrivateDirectory(const fs::path& path) {
    validateContainedPath(path, path.parent_path());
    const PinnedParent parent(path.parent_path());
#ifdef _WIN32
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto sddl = administrator() ? L"D:P(A;;FA;;;SY)(A;;FA;;;BA)" : L"D:P(A;;FA;;;SY)(A;;FA;;;OW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("Cannot create private maintenance permissions");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const bool ok = CreateDirectoryW(path.c_str(), &security) != 0; LocalFree(descriptor);
    if (!ok) throw std::runtime_error("Cannot reserve private maintenance directory");
#else
    if (mkdirat(parent.handle, path.filename().c_str(), 0700)) throw std::runtime_error("Cannot reserve private maintenance directory");
#endif
    syncDirectory(path.parent_path());
}
void durableWrite(const fs::path& path, std::string_view bytes) {
    if (bytes.size() > 64 * 1024 * 1024) throw std::runtime_error("Maintenance file exceeds limit");
    validateContainedPath(path, path.parent_path());
    const PinnedParent parent(path.parent_path());
    const auto temporary = path.parent_path() / (".write-" + randomToken());
    struct Guard { fs::path path; ~Guard() { std::error_code error; fs::remove(path, error); } } guard{temporary};
#ifdef _WIN32
    const auto fd = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fd == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot reserve maintenance file");
    DWORD written = 0;
    const bool ok = WriteFile(fd, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(fd);
    CloseHandle(fd);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish maintenance file");
#else
    const auto fd = openat(parent.handle, temporary.filename().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("Cannot reserve maintenance file");
    std::size_t position = 0;
    while (position < bytes.size()) {
        const auto count = write(fd, bytes.data() + position, bytes.size() - position);
        if (count <= 0) { close(fd); throw std::runtime_error("Cannot write maintenance file"); }
        position += std::size_t(count);
    }
    const int synced = fsync(fd); close(fd);
    if (synced || renameat(parent.handle, temporary.filename().c_str(), parent.handle, path.filename().c_str()))
        throw std::runtime_error("Cannot publish maintenance file");
#endif
    syncDirectory(path.parent_path());
}
std::string readBoundedFile(const fs::path& path, std::size_t maximum) {
    const auto before = identifyFile(path);
    if (before.bytes > maximum) throw std::runtime_error("Maintenance file exceeds limit");
    std::string result(std::size_t(before.bytes), '\0');
    std::ifstream input(path, std::ios::binary); input.read(result.data(), std::streamsize(result.size()));
    if (!input || identifyFile(path) != before) throw std::runtime_error("Maintenance file changed while reading");
    return result;
}
void durableRename(const fs::path& from, const fs::path& to) {
    validateContainedPath(from, from.parent_path()); validateContainedPath(to, to.parent_path());
    const PinnedParent source(from.parent_path()), destination(to.parent_path());
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot stage or restore maintenance file");
#else
    if (renameatx_np(source.handle, from.filename().c_str(), destination.handle, to.filename().c_str(), RENAME_EXCL))
        throw std::runtime_error("Cannot stage or restore maintenance file");
#endif
    syncDirectory(from.parent_path());
    if (from.parent_path() != to.parent_path()) syncDirectory(to.parent_path());
}
void durableRemove(const fs::path& path) {
    validateContainedPath(path, path.parent_path());
    const PinnedParent parent(path.parent_path());
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open maintenance entry for removal");
    FILE_DISPOSITION_INFO disposition{TRUE};
    const bool ok = SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
    CloseHandle(handle);
    if (!ok) throw std::runtime_error("Cannot remove maintenance entry");
#else
    struct stat info{};
    if (fstatat(parent.handle, path.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) ||
        unlinkat(parent.handle, path.filename().c_str(), S_ISDIR(info.st_mode) ? AT_REMOVEDIR : 0))
        throw std::runtime_error("Cannot remove maintenance entry");
    if (fsync(parent.handle)) throw std::runtime_error("Cannot sync removed maintenance entry");
#endif
    syncDirectory(path.parent_path());
}
}
