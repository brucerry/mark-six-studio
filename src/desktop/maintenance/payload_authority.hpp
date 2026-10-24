#pragma once
#include "storage_inventory.hpp"
#include <functional>

namespace marksix::maintenance {
bool protectedProductFile(const fs::path& path, const FileIdentity& expected); // Owner/ACL and the exact native file object.
inline bool protectedProductFile(const fs::path& path) { try { return protectedProductFile(path, identifyFile(path)); } catch (const std::exception&) { return false; } }
bool nativeProductCodeValid(const InstalledApplication& application);
struct PayloadAuthority {
    std::function<bool(const fs::path&, const FileIdentity&)> protectedFile = [](const auto& path, const auto& identity) { return protectedProductFile(path, identity); };
    std::function<bool(const InstalledApplication&)> codeValid = nativeProductCodeValid;
};
struct TrustedPayload {
    fs::path root;
    std::vector<Target> files;
};
// References are built from protected installed metadata by the elevated actor.
// They must never be decoded from an unprivileged UI message.
std::vector<TrustedPayload> trustedPayloads(const std::vector<InstalledApplication>& applications,
    const std::vector<fs::path>& homes, const PayloadAuthority& authority = {}, const std::vector<PreservedFile>& retainedLegacy = {});
Inventory authorizePayload(const InstalledApplication& application, const PayloadInspection& payload,
    const std::vector<TrustedPayload>& references, const PayloadAuthority& authority = {});
}
