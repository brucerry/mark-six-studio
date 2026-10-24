#include "storage_inventory.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#else
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace marksix::maintenance {
namespace {
std::string hex(const unsigned char* data, std::size_t count) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string text;
    text.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        text += alphabet[data[i] >> 4]; text += alphabet[data[i] & 15];
    }
    return text;
}
struct Node { std::uint64_t volume, file, bytes; std::int64_t modified; };
Node node(const fs::path& path, bool directory, bool link = false) {
#ifdef _WIN32
    if (link) throw std::runtime_error("Symbolic-link payload entries are not supported on Windows");
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Unavailable or redirected path: " + pathUtf8(path));
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot inspect file identity");
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(handle, &info) != 0;
    CloseHandle(handle);
    if (!ok || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
        throw std::runtime_error("Changed or unsupported file identity");
    return {info.dwVolumeSerialNumber, (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        static_cast<std::int64_t>((std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime)};
#else
    struct stat info{};
    if (lstat(path.c_str(), &info) || (link ? !S_ISLNK(info.st_mode) : directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)))
        throw std::runtime_error("Unavailable, redirected or unsupported path: " + pathUtf8(path));
    return {std::uint64_t(info.st_dev), std::uint64_t(info.st_ino), std::uint64_t(info.st_size),
        info.st_mtimespec.tv_sec * 1000000000LL + info.st_mtimespec.tv_nsec};
#endif
}
std::string ancestors(const fs::path& path) {
    std::string result;
    auto cursor = path.root_path();
    const auto append = [&](const fs::path& component) {
        const auto identity = node(component, true);
        result += std::to_string(identity.volume) + ":" + std::to_string(identity.file) + "/";
    };
    append(cursor);
    for (const auto& part : path.relative_path().parent_path()) {
        cursor /= part;
        append(cursor);
    }
    return result;
}
}

std::string pathUtf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
fs::path pathFromUtf8(const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (!first) throw std::invalid_argument("NUL in path");
        if (first < 0x80) continue;
        unsigned value = 0, minimum = 0, count = 0;
        if (first >= 0xc2 && first <= 0xdf) { value = first & 31; count = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { value = first & 15; count = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { value = first & 7; count = 3; minimum = 0x10000; }
        else throw std::invalid_argument("Invalid UTF-8 path");
        if (count > text.size() - i) throw std::invalid_argument("Truncated UTF-8 path");
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) throw std::invalid_argument("Invalid UTF-8 path");
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            throw std::invalid_argument("Invalid UTF-8 path");
    }
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string sha256File(const fs::path& path) {
    std::array<unsigned char, 32> digest{};
    std::array<char, 64 * 1024> buffer{};
#ifdef _WIN32
    const auto input = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (input == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot read file for verification");
    struct InputGuard { HANDLE handle; ~InputGuard() { CloseHandle(handle); } } inputGuard{input};
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(input, &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("Redirected or unsupported verification target");
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0))
        throw std::runtime_error("SHA-256 unavailable");
    struct Guard { BCRYPT_HASH_HANDLE value; ~Guard() { BCryptDestroyHash(value); } } guard{hash};
#else
    const int input = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (input < 0) throw std::runtime_error("Cannot read file for verification");
    struct InputGuard { int descriptor; ~InputGuard() { close(descriptor); } } inputGuard{input};
    struct stat information{};
    if (fstat(input, &information) || !S_ISREG(information.st_mode)) throw std::runtime_error("Unsupported verification target");
    CC_SHA256_CTX hash{};
    CC_SHA256_Init(&hash);
#endif
    for (;;) {
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(input, buffer.data(), DWORD(buffer.size()), &count, nullptr)) throw std::runtime_error("File read interrupted");
        if (count && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(count), 0))
            throw std::runtime_error("SHA-256 read failed");
#else
        const auto count = read(input, buffer.data(), buffer.size());
        if (count < 0) throw std::runtime_error("File read interrupted");
        if (count) CC_SHA256_Update(&hash, buffer.data(), CC_LONG(count));
#endif
        if (!count) break;
    }
#ifdef _WIN32
    if (BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0)) throw std::runtime_error("SHA-256 failed");
#else
    CC_SHA256_Final(digest.data(), &hash);
#endif
    return hex(digest.data(), digest.size());
}

std::string sha256Bytes(std::string_view bytes) {
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Digest input exceeds limit");
    std::array<unsigned char, 32> digest{};
#ifdef _WIN32
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0)) throw std::runtime_error("SHA-256 unavailable");
    const bool ok = !BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), ULONG(bytes.size()), 0) &&
        !BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0);
    BCryptDestroyHash(hash);
    if (!ok) throw std::runtime_error("SHA-256 failed");
#else
    CC_SHA256(bytes.data(), CC_LONG(bytes.size()), digest.data());
#endif
    return hex(digest.data(), digest.size());
}

FileIdentity identifyFileMetadata(const fs::path& path) {
    if (!path.is_absolute() || path != path.lexically_normal()) throw std::runtime_error("Absolute normalized path required");
    const auto before = node(path, false); const auto chain = ancestors(path); const auto after = node(path, false);
    if (before.volume != after.volume || before.file != after.file || before.bytes != after.bytes || before.modified != after.modified || chain != ancestors(path))
        throw std::runtime_error("File metadata changed during inspection");
    return {before.volume, before.file, before.bytes, before.modified, chain, {}, {}};
}
FileIdentity identifyDirectory(const fs::path& path) {
    if (!path.is_absolute() || path != path.lexically_normal()) throw std::runtime_error("Absolute normalized directory required");
    const auto before = node(path, true); const auto chain = ancestors(path); const auto after = node(path, true);
    if (before.volume != after.volume || before.file != after.file || chain != ancestors(path))
        throw std::runtime_error("Directory changed during inspection");
    return {before.volume, before.file, 0, 0, chain, {}, {}};
}
FileIdentity identifyFile(const fs::path& path) {
    if (!path.is_absolute() || path != path.lexically_normal()) throw std::runtime_error("Absolute normalized path required");
    const auto before = node(path, false);
    const auto chain = ancestors(path);
    const auto digest = sha256File(path);
    const auto after = node(path, false);
    if (before.volume != after.volume || before.file != after.file || before.bytes != after.bytes ||
        before.modified != after.modified || chain != ancestors(path)) throw std::runtime_error("File changed during inspection");
    return {before.volume, before.file, before.bytes, before.modified, chain, digest, {}};
}
FileIdentity identifySymbolicLink(const fs::path& path) {
    if (!path.is_absolute() || path != path.lexically_normal()) throw std::runtime_error("Absolute normalized path required");
    const auto before = node(path, false, true);
    const auto chain = ancestors(path);
    const auto text = pathUtf8(fs::read_symlink(path));
    const auto after = node(path, false, true);
    if (text.empty() || text.size() > 32768 || before.volume != after.volume || before.file != after.file ||
        before.bytes != after.bytes || before.modified != after.modified || chain != ancestors(path))
        throw std::runtime_error("Symbolic link changed during inspection");
    return {before.volume, before.file, before.bytes, before.modified, chain, {}, text};
}

void validateContainedPath(const fs::path& path, const fs::path& boundary) {
    if (path.native().find(fs::path::value_type{}) != fs::path::string_type::npos ||
        boundary.native().find(fs::path::value_type{}) != fs::path::string_type::npos)
        throw std::runtime_error("NUL in path");
    if (!path.is_absolute() || path != path.lexically_normal() || !boundary.is_absolute() ||
        boundary != boundary.lexically_normal()) throw std::runtime_error("Absolute normalized paths required");
    auto filePart = path.begin();
    for (auto rootPart = boundary.begin(); rootPart != boundary.end(); ++rootPart, ++filePart)
        if (filePart == path.end() || *filePart != *rootPart) throw std::runtime_error("Path escapes approved location");
    if (filePart == path.end()) throw std::runtime_error("An entire storage root is not a file target");
    // Validate existing parents without resolving a symbolic link or junction.
    auto cursor = path.root_path();
    node(cursor, true);
    for (const auto& part : path.relative_path().parent_path()) {
        cursor /= part;
        std::error_code error;
        const auto status = fs::symlink_status(cursor, error);
        if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) break;
        if (error) throw std::runtime_error("Cannot inspect parent directory");
        node(cursor, true);
    }
}

void validateBoundary(const fs::path& boundary, const fs::path& home) {
    if (!boundary.is_absolute() || boundary != boundary.lexically_normal() || boundary == boundary.root_path() ||
        boundary == home || boundary == home.parent_path()) throw std::runtime_error("Unsafe storage root");
    // Native filesystems may accept different case/spelling for an existing
    // home. Such aliases cannot turn the home itself into a cleanup boundary.
    for (const auto& protectedRoot : {home, home.parent_path()}) {
        std::error_code error;
        if (!protectedRoot.empty() && fs::equivalent(boundary, protectedRoot, error) && !error)
            throw std::runtime_error("OS home directory is not a storage root");
    }
    // These broad roots cannot become removal boundaries even through a custom-root selection.
    static const char* forbidden[] = {"/Applications", "/System", "/Library", "/Users", "/Volumes", "/usr", "/bin", "/sbin", "/etc", "/var", "/private", "/tmp", "/private/var", "/private/tmp", "/private/etc"};
    for (const auto* value : forbidden) if (boundary == fs::path(value)) throw std::runtime_error("System directory is not a storage root");
#ifdef _WIN32
    std::vector<fs::path> systemRoots;
    std::array<wchar_t, 32768> buffer{};
    const auto windowsCount = GetWindowsDirectoryW(buffer.data(), UINT(buffer.size()));
    if (!windowsCount || windowsCount >= buffer.size()) throw std::runtime_error("Cannot resolve Windows system roots");
    systemRoots.emplace_back(buffer.data());
    const auto systemCount = GetSystemDirectoryW(buffer.data(), UINT(buffer.size()));
    if (!systemCount || systemCount >= buffer.size()) throw std::runtime_error("Cannot resolve Windows system roots");
    systemRoots.emplace_back(buffer.data());
    for (const auto* id : {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramData}) {
        PWSTR path = nullptr;
        const auto status = SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &path);
        if (FAILED(status) || !path) { CoTaskMemFree(path); throw std::runtime_error("Cannot resolve Windows protected folders"); }
        systemRoots.emplace_back(path); CoTaskMemFree(path);
    }
    for (const auto& system : systemRoots) {
        std::error_code error;
        if (fs::equivalent(boundary, system, error))
            throw std::runtime_error("System directory is not a storage root");
    }
#endif
    validateContainedPath(boundary / ".inventory-probe", boundary);
}

bool stillMatches(const Target& target) {
    try {
        validateContainedPath(target.path, target.boundary);
        return (target.identity.linkTarget.empty() ? identifyFile(target.path) : identifySymbolicLink(target.path)) == target.identity;
    } catch (...) { return false; }
}
}
