#include "storage_inventory.hpp"
#include "native_runtime.hpp"
#include "native_command.hpp"
#include "payload_authority.hpp"
#include "wire_codec.hpp"
#include <algorithm>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <array>
#include <fstream>
#include <set>
#include <pwd.h>
#include <unistd.h>

namespace marksix::maintenance {
namespace {
template<class Value> struct CFGuard {
    Value value;
    ~CFGuard() { if (value) CFRelease(value); }
};
CFURLRef directoryUrl(const fs::path& path) {
    const auto text = pathUtf8(path);
    return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(text.data()), CFIndex(text.size()), true);
}
bool matches(CFDictionaryRef dictionary, CFStringRef key, CFStringRef expected) {
    const auto value = CFDictionaryGetValue(dictionary, key);
    return value && CFGetTypeID(value) == CFStringGetTypeID() && CFEqual(value, expected);
}
}
bool hasProductIdentity(const fs::path& root) {
    try {
        if (isDevelopmentLocation(root)) return false;
        const auto plist = root / "Contents/Info.plist";
        const auto executable = root / "Contents/MacOS/mark-six-studio";
        validateContainedPath(plist, root); validateContainedPath(executable, root);
        const auto before = identifyFile(plist);
        identifyFile(executable);
        const CFGuard url{directoryUrl(root)};
        if (!url.value) return false;
        const CFGuard info{CFBundleCopyInfoDictionaryInDirectory(url.value)};
        if (!info.value || !matches(info.value, kCFBundleIdentifierKey, CFSTR("com.brucerry.marksixstudio")) ||
            !matches(info.value, kCFBundleExecutableKey, CFSTR("mark-six-studio")) ||
            !matches(info.value, CFSTR("CFBundlePackageType"), CFSTR("APPL"))) return false;
        std::ifstream binary(executable, std::ios::binary);
        std::array<unsigned char, 4> magic{}; binary.read(reinterpret_cast<char*>(magic.data()), magic.size());
        if (!binary) return false;
        const auto word = (std::uint32_t(magic[0]) << 24) | (std::uint32_t(magic[1]) << 16) |
            (std::uint32_t(magic[2]) << 8) | magic[3];
        const bool machO = word == 0xcafebabe || word == 0xbebafeca || word == 0xcafebabf ||
            word == 0xbfbafeca || word == 0xcffaedfe || word == 0xfeedfacf;
        return machO && before == identifyFile(plist);
    } catch (...) { return false; }
}
std::vector<fs::path> registeredApplications() {
    std::vector<fs::path> result;
    passwd account{}; passwd* resolved = nullptr; std::array<char, 32768> accountBytes{};
    if (getpwuid_r(getuid(), &account, accountBytes.data(), accountBytes.size(), &resolved) || !resolved || !account.pw_dir)
        throw std::runtime_error("Native registration account home is unavailable");
    std::error_code homeError; fs::directory_iterator home(account.pw_dir, homeError);
    if (homeError) throw std::runtime_error("Native registration account home cannot be inspected");
    CFErrorRef error = nullptr;
    const CFGuard urls{LSCopyApplicationURLsForBundleIdentifier(CFSTR("com.brucerry.marksixstudio"), &error)};
    const CFGuard failure{error};
    if (!urls.value) {
        if (error && CFErrorGetCode(error) == kLSApplicationNotFoundErr) return result;
        throw std::runtime_error("Native account application registrations are inaccessible");
    }
    if (CFArrayGetCount(urls.value) > 4096) throw std::runtime_error("Native application registration limit exceeded");
    for (CFIndex i = 0; i < CFArrayGetCount(urls.value); ++i) {
        const auto url = static_cast<CFURLRef>(const_cast<void*>(CFArrayGetValueAtIndex(urls.value, i)));
        std::array<UInt8, 32768> path{};
        if (!CFURLGetFileSystemRepresentation(url, true, path.data(), path.size())) throw std::runtime_error("Registered application URL is unreadable");
        result.push_back(normalizePlatformPath(fs::path(reinterpret_cast<const char*>(path.data()))));
    }
    return result;
}
std::string nativeApplicationRegistrations() {
    Encoder out; out.text("M6NATIVEREGISTEREDAPPS1"); out.text(currentAccount());
    const auto apps = registeredApplications(); out.number(apps.size()); for (const auto& app : apps) out.path(app); return out.bytes;
}
bool applicationRegistered(const fs::path& root) {
    const auto apps = registeredApplications(); return std::find(apps.begin(), apps.end(), root) != apps.end();
}
std::vector<InstalledApplication> discoverApplications(const std::vector<UserStorage>& users,
                                                     std::vector<InventoryIssue>& issues) {
    std::set<fs::path> candidates{fs::path("/Applications/Mark Six Studio.app")};
    for (const auto& user : users) candidates.insert(user.home / "Applications/Mark Six Studio.app");
    const auto self = processIdentity(currentProcessId());
    for (const auto& user : users) {
        try {
            std::string snapshot;
            if (user.account == currentAccount()) snapshot = nativeApplicationRegistrations();
            else {
                if (!administrator() || !self || self->uncertain || !protectedProductFile(self->executable) || self->executable.filename() != "mark-six-maintenance")
                    throw std::runtime_error("Other-account application registrations require the native administrator review");
                if (user.account.empty() || user.account.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("Invalid native application registration account");
                const auto query = runNativeCommand("/usr/bin/sudo", {"-H", "-u", "#" + user.account, pathUtf8(self->executable), "--list-registered-applications"});
                if (query.status) throw std::runtime_error("Native application registrations are unavailable for this account: " + query.output);
                snapshot = query.output;
            }
            Decoder in(snapshot);
            if (in.text() != "M6NATIVEREGISTEREDAPPS1" || in.text() != user.account) throw std::runtime_error("Native application registration account changed");
            const auto count = in.count(); if (count > 4096) throw std::runtime_error("Native registration count exceeds limit");
            for (std::size_t i = 0; i < count; ++i) candidates.insert(in.path()); in.end();
        } catch (const std::exception& error) { issues.push_back({user.home, user.account, error.what(), true}); }
    }
    std::vector<InstalledApplication> applications;
    for (const auto& root : candidates) {
        if (isDevelopmentLocation(root)) continue;
        std::error_code error;
        const auto status = fs::symlink_status(root, error);
        if (error == std::errc::no_such_file_or_directory || (!error && status.type() == fs::file_type::not_found)) continue;
        if (error || !hasProductIdentity(root)) {
            issues.push_back({root, {}, "Registered application copy is inaccessible or has unverified product identity", true});
            continue;
        }
        std::string account;
        for (const auto& user : users) {
            const auto relative = root.lexically_relative(user.home);
            if (!relative.empty() && *relative.begin() != "..") { account = user.account; break; }
        }
        applications.push_back({root, root / "Contents/MacOS/mark-six-studio", account, "com.brucerry.marksixstudio"});
    }
    return applications;
}
}
