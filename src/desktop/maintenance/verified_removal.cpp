#include "verified_removal.hpp"
#include "durable_files.hpp"
#include <array>
#include <regex>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#else
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#endif

namespace marksix::maintenance {
namespace {
std::string hex(const unsigned char* bytes, std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < count; ++i) { result += "0123456789abcdef"[bytes[i] >> 4]; result += "0123456789abcdef"[bytes[i] & 15]; }
    return result;
}
#ifdef _WIN32
struct Handle { HANDLE value = INVALID_HANDLE_VALUE; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
struct PinnedParent {
    std::vector<HANDLE> handles;
    explicit PinnedParent(const fs::path& path) {
        try {
            auto cursor = path.root_path();
            const auto pin = [&](const fs::path& part) {
                const auto handle = CreateFileW(part.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot pin approved removal ancestor");
                handles.push_back(handle); BY_HANDLE_FILE_INFORMATION information{};
                if (!GetFileInformationByHandle(handle, &information) || !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Redirected removal ancestor");
            };
            pin(cursor); for (const auto& part : path.relative_path()) { cursor /= part; pin(cursor); }
        } catch (...) { for (const auto handle : handles) CloseHandle(handle); throw; }
    }
    ~PinnedParent() { for (const auto handle : handles) CloseHandle(handle); }
};
bool matches(HANDLE handle, const FileIdentity& expected) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    return expected.volume == info.dwVolumeSerialNumber && expected.file == ((std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow) &&
        expected.bytes == ((std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow) &&
        expected.modified == std::int64_t((std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime);
}
std::string digest(HANDLE handle) {
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0)) throw std::runtime_error("Removal SHA-256 unavailable");
    struct Hash { BCRYPT_HASH_HANDLE value; ~Hash() { BCryptDestroyHash(value); } } guard{hash};
    std::array<unsigned char, 65536> bytes{}; std::array<unsigned char, 32> result{};
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(handle, bytes.data(), DWORD(bytes.size()), &count, nullptr)) throw std::runtime_error("Cannot verify approved removal content");
        if (!count) break;
        if (BCryptHashData(hash, bytes.data(), count, 0)) throw std::runtime_error("Cannot verify approved removal hash");
    }
    if (BCryptFinishHash(hash, result.data(), ULONG(result.size()), 0)) throw std::runtime_error("Cannot finalize approved removal hash");
    return hex(result.data(), result.size());
}
#else
struct Handle { int value = -1; ~Handle() { if (value >= 0) close(value); } };
struct PinnedParent {
    Handle handle;
    std::string chain;
    void record() {
        struct stat identity{};
        if (fstat(handle.value, &identity) || !S_ISDIR(identity.st_mode)) throw std::runtime_error("Cannot verify pinned removal ancestor");
        chain += std::to_string(std::uint64_t(identity.st_dev)) + ':' + std::to_string(std::uint64_t(identity.st_ino)) + '/';
    }
    explicit PinnedParent(const fs::path& path) {
        handle.value = open(path.root_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (handle.value < 0) throw std::runtime_error("Cannot pin approved removal root");
        record();
        for (const auto& part : path.relative_path()) {
            const auto next = openat(handle.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            close(handle.value); handle.value = next;
            if (next < 0) throw std::runtime_error("Cannot pin approved removal ancestor");
            record();
        }
    }
};
bool matches(const struct stat& info, const FileIdentity& expected) {
    return expected.volume == std::uint64_t(info.st_dev) && expected.file == std::uint64_t(info.st_ino) &&
        expected.bytes == std::uint64_t(info.st_size) && expected.modified == info.st_mtimespec.tv_sec * 1000000000LL + info.st_mtimespec.tv_nsec &&
        (expected.linkTarget.empty() ? S_ISREG(info.st_mode) : S_ISLNK(info.st_mode));
}
std::string digest(int handle) {
    CC_SHA256_CTX hash{}; CC_SHA256_Init(&hash);
    std::array<unsigned char, 65536> bytes{}; std::array<unsigned char, 32> result{};
    for (;;) {
        const auto count = read(handle, bytes.data(), bytes.size());
        if (count < 0) throw std::runtime_error("Cannot verify quarantined removal content");
        if (!count) break;
        CC_SHA256_Update(&hash, bytes.data(), CC_LONG(count));
    }
    CC_SHA256_Final(result.data(), &hash); return hex(result.data(), result.size());
}
bool matchesAt(int directory, const char* name, const FileIdentity& expected) {
    struct stat before{}; if (fstatat(directory, name, &before, AT_SYMLINK_NOFOLLOW) || !matches(before, expected)) return false;
    if (expected.linkTarget.empty()) {
        Handle handle{openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
        struct stat opened{};
        if (handle.value < 0 || fstat(handle.value, &opened) || !matches(opened, expected) || digest(handle.value) != expected.sha256) return false;
    } else {
        std::array<char, 32769> value{}; const auto count = readlinkat(directory, name, value.data(), value.size());
        if (count < 0 || std::size_t(count) >= value.size() || std::string_view(value.data(), std::size_t(count)) != expected.linkTarget) return false;
    }
    struct stat after{}; return !fstatat(directory, name, &after, AT_SYMLINK_NOFOLLOW) && matches(after, expected);
}
bool existsAt(int directory, const char* name) {
    struct stat info{};
    if (!fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW)) return true;
    if (errno == ENOENT) return false;
    throw std::runtime_error("Cannot inspect exact removal entry");
}
#endif
}
fs::path removalStagingPath(const Target& target, const std::string& nonce, std::size_t index) {
    if (nonce.size() != 64 || nonce.find_first_not_of("0123456789abcdef") != std::string::npos || index > 100000)
        throw std::runtime_error("Invalid removal staging identity");
    return target.path.parent_path() / (".marksix-purge-" + nonce + '-' + std::to_string(index));
}
bool removeVerifiedEmptyDirectory(const fs::path& path, const FileIdentity& identity) {
    if (legacyRecordingName(path)) return false;
    const PinnedParent parent(path.parent_path());
    if (identifyDirectory(path) != identity) throw std::runtime_error("Approved directory identity changed; retained");
#ifdef _WIN32
    Handle file{CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        identity.volume != info.dwVolumeSerialNumber || identity.file != ((std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow))
        throw std::runtime_error("Cannot pin approved empty directory");
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition))) {
        if (GetLastError() == ERROR_DIR_NOT_EMPTY) return false;
        throw std::runtime_error("Cannot remove approved empty directory");
    }
#else
    if (parent.chain != identity.ancestors) throw std::runtime_error("Approved directory ancestor changed");
    struct stat named{};
    if (fstatat(parent.handle.value, path.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(named.st_mode) ||
        std::uint64_t(named.st_dev) != identity.volume || std::uint64_t(named.st_ino) != identity.file)
        throw std::runtime_error("Approved directory changed before removal");
    // unlinkat can remove only an empty directory. Even a last-instant
    // substitution cannot make it recurse into or remove another user's files.
    if (unlinkat(parent.handle.value, path.filename().c_str(), AT_REMOVEDIR)) {
        if (errno == ENOTEMPTY || errno == EEXIST) return false;
        throw std::runtime_error("Cannot remove approved empty directory");
    }
    if (fsync(parent.handle.value)) throw std::runtime_error("Cannot persist empty-directory removal");
#endif
    return true;
}
void removeVerifiedTarget(const Target& target, const fs::path& staging, const ResetCheckpoint& checkpoint) {
    validateContainedPath(target.path, target.boundary);
    static const std::regex stageName("\\.marksix-purge-[0-9a-f]{64}-(0|[1-9][0-9]{0,5})");
    if (staging.parent_path() != target.path.parent_path() || !std::regex_match(pathUtf8(staging.filename()), stageName) || legacyRecordingName(target.path))
        throw std::runtime_error("Invalid or preserved removal target");
    validateContainedPath(staging, target.boundary);
#ifdef _WIN32
    if (!target.identity.linkTarget.empty()) throw std::runtime_error("Windows payload links are not removal targets");
    const PinnedParent parent(target.path.parent_path());
    if (!stillMatches(target)) throw std::runtime_error("Approved file changed before removal");
    // DELETE and read access, with no write/delete sharing: the object cannot
    // be replaced or edited between verification and handle disposition.
    Handle file{CreateFileW(target.path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE || !matches(file.value, target.identity) || digest(file.value) != target.identity.sha256 || !matches(file.value, target.identity))
        throw std::runtime_error("Approved file is locked or changed; preserved for retry");
    if (checkpoint) checkpoint("verified");
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition))) throw std::runtime_error("Approved file could not be removed");
    if (checkpoint) checkpoint("removed");
#else
    const PinnedParent parent(target.path.parent_path());
    if (parent.chain != target.identity.ancestors) throw std::runtime_error("Approved removal ancestor identity changed");
    const auto name = target.path.filename().native(), stageNameNative = staging.filename().native();
    bool created = false;
    if (!existsAt(parent.handle.value, stageNameNative.c_str())) {
        if (!stillMatches(target) || mkdirat(parent.handle.value, stageNameNative.c_str(), 0700)) throw std::runtime_error("Cannot reserve approved-file quarantine");
        created = true;
        if (fsync(parent.handle.value)) throw std::runtime_error("Cannot sync approved-file quarantine");
    }
    Handle stage{openat(parent.handle.value, stageNameNative.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat security{};
    if (stage.value < 0 || fstat(stage.value, &security) || security.st_uid != geteuid() || (security.st_mode & 0777) != 0700)
        throw std::runtime_error("Removal quarantine is not a private directory owned by the executor");
    try {
        const int copy = dup(stage.value);
        if (copy < 0) throw std::runtime_error("Cannot inspect removal quarantine");
        DIR* entries = fdopendir(copy);
        if (!entries) { close(copy); throw std::runtime_error("Cannot inspect removal quarantine entries"); }
        bool unknown = false; errno = 0;
        while (const auto* entry = readdir(entries)) {
            const std::string_view item(entry->d_name);
            if (item != "." && item != ".." && item != "item") unknown = true;
        }
        const bool failed = errno != 0; closedir(entries);
        if (unknown || failed) throw std::runtime_error("Unknown quarantine content is preserved; removal requires review");
        // Inspection through the pinned descriptor never follows a substituted
        // parent path. Unknown entries prevent nonrecursive final removal.
        const bool original = existsAt(parent.handle.value, name.c_str()), moved = existsAt(stage.value, "item");
        if (original && moved) throw std::runtime_error("A replacement appeared beside an interrupted removal; both files are retained");
        if (original) {
            if (!matchesAt(parent.handle.value, name.c_str(), target.identity)) throw std::runtime_error("Approved file changed before quarantine");
            if (renameatx_np(parent.handle.value, name.c_str(), stage.value, "item", RENAME_EXCL)) throw std::runtime_error("Cannot quarantine approved file");
            if (fsync(stage.value) || fsync(parent.handle.value)) throw std::runtime_error("Cannot sync quarantined removal");
            if (checkpoint) checkpoint("staged");
        }
        if (existsAt(stage.value, "item")) {
            if (!matchesAt(stage.value, "item", target.identity)) {
                if (!existsAt(parent.handle.value, name.c_str())) (void)renameatx_np(stage.value, "item", parent.handle.value, name.c_str(), RENAME_EXCL);
                throw std::runtime_error("Changed quarantined entry was retained; no deletion was approved for its new identity");
            }
            if (checkpoint) checkpoint("verified");
            if (unlinkat(stage.value, "item", 0) || fsync(stage.value)) throw std::runtime_error("Cannot remove exact approved quarantine entry");
            if (checkpoint) checkpoint("removed");
        }
        struct stat named{};
        if (fstatat(parent.handle.value, stageNameNative.c_str(), &named, AT_SYMLINK_NOFOLLOW) || named.st_dev != security.st_dev || named.st_ino != security.st_ino)
        {
            std::array<char, PATH_MAX> actual{};
            const auto location = !fcntl(stage.value, F_GETPATH, actual.data()) ? std::string(actual.data()) : pathUtf8(staging);
            throw std::runtime_error("Quarantine location changed. Remaining pinned directory: " + quotedReportField(location) + ". Review this location before retrying.");
        }
        if (unlinkat(parent.handle.value, stageNameNative.c_str(), AT_REMOVEDIR) || fsync(parent.handle.value)) throw std::runtime_error("Quarantine contains unknown files or cannot be finalized");
    } catch (...) {
        if (created) {
            struct stat named{};
            if (!fstatat(parent.handle.value, stageNameNative.c_str(), &named, AT_SYMLINK_NOFOLLOW) && named.st_dev == security.st_dev && named.st_ino == security.st_ino)
                (void)unlinkat(parent.handle.value, stageNameNative.c_str(), AT_REMOVEDIR);
            (void)fsync(parent.handle.value);
        }
        throw;
    }
#endif
}
}
