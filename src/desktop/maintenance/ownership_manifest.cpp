#include "storage_inventory.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace marksix::maintenance {
namespace {
constexpr std::uint32_t maximumEntries = 4096;
constexpr std::uint32_t maximumString = 32768;
constexpr std::uintmax_t maximumManifest = 1024 * 1024;
constexpr char signature[] = "M6OWNERSHIP\n1\n";
void number(std::ostream& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.put(char((value >> shift) & 255));
}
std::uint32_t number(std::istream& in) {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const auto byte = in.get();
        if (byte < 0) throw std::runtime_error("Truncated ownership manifest");
        value |= std::uint32_t(byte) << shift;
    }
    return value;
}
void text(std::ostream& out, const std::string& value) {
    if (value.size() > maximumString || value.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid ownership manifest string");
    number(out, std::uint32_t(value.size())); out.write(value.data(), std::streamsize(value.size()));
}
std::string text(std::istream& in) {
    const auto length = number(in);
    if (length > maximumString) throw std::runtime_error("Ownership manifest string exceeds limit");
    std::string value(length, '\0'); in.read(value.data(), length);
    if (!in || value.find('\0') != std::string::npos) throw std::runtime_error("Invalid ownership manifest string");
    return value;
}
void checkPath(const fs::path& path) {
    if (!path.is_absolute() || path != path.lexically_normal() || path == path.root_path())
        throw std::runtime_error("Invalid ownership manifest path");
}
void checkDigest(const std::string& digest) {
    if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("Invalid ownership manifest digest");
}
void paths(std::ostream& out, const std::vector<fs::path>& values) {
    if (values.size() > maximumEntries) throw std::runtime_error("Too many ownership entries");
    number(out, std::uint32_t(values.size()));
    for (const auto& value : values) { checkPath(value); text(out, pathUtf8(value)); }
}
std::vector<fs::path> paths(std::istream& in) {
    const auto count = number(in);
    if (count > maximumEntries) throw std::runtime_error("Too many ownership entries");
    std::vector<fs::path> result;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto path = pathFromUtf8(text(in)); checkPath(path); result.push_back(std::move(path));
    }
    return result;
}
void files(std::ostream& out, const std::vector<VerifiedFile>& values) {
    if (values.size() > maximumEntries) throw std::runtime_error("Too many ownership entries");
    number(out, std::uint32_t(values.size()));
    for (const auto& value : values) {
        checkPath(value.path); checkDigest(value.sha256); text(out, pathUtf8(value.path)); text(out, value.sha256);
    }
}
std::vector<VerifiedFile> files(std::istream& in) {
    const auto count = number(in);
    if (count > maximumEntries) throw std::runtime_error("Too many ownership entries");
    std::vector<VerifiedFile> result;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto path = pathFromUtf8(text(in)); auto digest = text(in);
        checkPath(path); checkDigest(digest); result.push_back({std::move(path), std::move(digest)});
    }
    return result;
}
}

OwnershipManifest readManifest(const fs::path& path) {
    validateContainedPath(path, path.parent_path());
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || (!error && status.type() == fs::file_type::not_found)) return {};
    if (error || !fs::is_regular_file(status)) throw std::runtime_error("Ownership manifest is inaccessible or redirected");
    const auto identity = identifyFile(path);
    if (identity.bytes > maximumManifest) throw std::runtime_error("Ownership manifest exceeds limit");
    std::ifstream input(path, std::ios::binary);
    std::string header(sizeof(signature) - 1, '\0'); input.read(header.data(), std::streamsize(header.size()));
    if (header != signature) throw std::runtime_error("Unsupported ownership manifest version");
    OwnershipManifest result;
    result.dataRoots = paths(input); result.createdDirectories = paths(input);
    result.downloads = files(input); result.recordings = files(input);
    if (input.peek() != std::char_traits<char>::eof() || identifyFile(path) != identity)
        throw std::runtime_error("Ownership manifest changed or has trailing data");
    return result;
}

void writeManifest(const fs::path& path, const OwnershipManifest& manifest) {
    std::ostringstream output(std::ios::binary);
    output.write(signature, sizeof(signature) - 1);
    paths(output, manifest.dataRoots); paths(output, manifest.createdDirectories);
    files(output, manifest.downloads); files(output, manifest.recordings);
    const auto payload = output.str();
    if (!output || payload.size() > maximumManifest) throw std::runtime_error("Ownership manifest exceeds limit");
    validateContainedPath(path, path.parent_path());
    fs::create_directories(path.parent_path());
    validateContainedPath(path, path.parent_path());
    std::array<unsigned char, 16> random{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, random.data(), ULONG(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        throw std::runtime_error("Random source unavailable");
#else
    arc4random_buf(random.data(), random.size());
#endif
    std::string suffix;
    for (const auto byte : random) { suffix += "0123456789abcdef"[byte >> 4]; suffix += "0123456789abcdef"[byte & 15]; }
    auto temporary = path; temporary += "." + suffix + ".tmp";
    struct Guard { fs::path path; ~Guard() { std::error_code ignored; fs::remove(path, ignored); } } guard{temporary};
#ifdef _WIN32
    const auto reserved = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reserved == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot reserve ownership manifest");
    DWORD written = 0;
    const bool published = WriteFile(reserved, payload.data(), DWORD(payload.size()), &written, nullptr) && written == payload.size() && FlushFileBuffers(reserved);
    CloseHandle(reserved);
    if (!published) throw std::runtime_error("Cannot write ownership manifest");
#else
    const int reserved = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (reserved < 0) throw std::runtime_error("Cannot reserve ownership manifest");
    std::size_t position = 0;
    while (position < payload.size()) {
        const auto count = write(reserved, payload.data() + position, payload.size() - position);
        if (count <= 0) { close(reserved); throw std::runtime_error("Cannot write ownership manifest"); }
        position += std::size_t(count);
    }
    const int synced = fsync(reserved);
    close(reserved);
    if (synced) throw std::runtime_error("Cannot sync ownership manifest");
#endif
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish ownership manifest");
#else
    fs::rename(temporary, path);
    const int parent = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (parent < 0) throw std::runtime_error("Cannot sync ownership directory");
    const int directorySynced = fsync(parent); close(parent);
    if (directorySynced) throw std::runtime_error("Cannot sync ownership directory");
#endif
}

void rememberDataRoot(OwnershipManifest& manifest, const fs::path& root) {
    checkPath(root);
    if (std::find(manifest.dataRoots.begin(), manifest.dataRoots.end(), root) == manifest.dataRoots.end())
        manifest.dataRoots.push_back(root);
}
void rememberDownload(OwnershipManifest& manifest, const fs::path& file) {
    const auto identity = identifyFile(file);
    std::erase_if(manifest.downloads, [&](const auto& entry) { return entry.path == file; });
    manifest.downloads.push_back({file, identity.sha256});
}
}
