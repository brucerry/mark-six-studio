#pragma once
#include "storage_inventory.hpp"
#include <limits>
#include <stdexcept>

namespace marksix::maintenance {
// One bounded, length-prefixed encoding for journals and private IPC. No paths
// or message contents are ever interpreted as executable source.
struct Encoder {
    std::string bytes;
    void number(std::uint64_t value) { for (int i = 0; i < 8; ++i) bytes += char((value >> (i * 8)) & 255); }
    void text(std::string_view value) {
        if (value.size() > 32768) throw std::runtime_error("Maintenance field exceeds limit");
        number(value.size()); bytes.append(value);
        if (bytes.size() > 16 * 1024 * 1024) throw std::runtime_error("Maintenance message exceeds limit");
    }
    void path(const fs::path& value) { text(pathUtf8(value)); }
    void identity(const FileIdentity& value) {
        number(value.volume); number(value.file); number(value.bytes); number(std::uint64_t(value.modified));
        text(value.ancestors); text(value.sha256); text(value.linkTarget);
    }
};
struct Decoder {
    std::string_view bytes;
    explicit Decoder(std::string_view data) : bytes(data) {
        if (bytes.size() > 16 * 1024 * 1024) throw std::runtime_error("Maintenance message exceeds limit");
    }
    std::uint64_t number() {
        if (bytes.size() < 8) throw std::runtime_error("Truncated maintenance message");
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value |= std::uint64_t(static_cast<unsigned char>(bytes[std::size_t(i)])) << (i * 8);
        bytes.remove_prefix(8); return value;
    }
    std::size_t count() {
        const auto value = number();
        if (value > 100000) throw std::runtime_error("Maintenance entry limit exceeded");
        return std::size_t(value);
    }
    std::string text() {
        const auto size = number();
        if (size > 32768 || size > bytes.size()) throw std::runtime_error("Invalid maintenance field");
        std::string result(bytes.substr(0, std::size_t(size))); bytes.remove_prefix(std::size_t(size)); return result;
    }
    fs::path path() { return pathFromUtf8(text()); }
    FileIdentity identity() {
        FileIdentity value; value.volume = number(); value.file = number(); value.bytes = number(); value.modified = std::int64_t(number());
        value.ancestors = text(); value.sha256 = text(); value.linkTarget = text(); return value;
    }
    void end() const { if (!bytes.empty()) throw std::runtime_error("Trailing maintenance data"); }
};
}
