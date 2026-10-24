#include "uninstall_execution.hpp"
#include "native_command.hpp"
#include "payload_authority.hpp"
#include "durable_files.hpp"
#include "wire_codec.hpp"
#include "installer_lease.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <libproc.h>
#include <algorithm>
#include <array>
#include <set>

namespace marksix::maintenance {
namespace {
const fs::path receipts = "/private/var/db/receipts";
const fs::path lsregister = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister";
const std::string product = "com.brucerry.marksixstudio";
}
NativeInstallationState nativeInstallationState() {
    try {
        const fs::path root = "/Applications/Mark Six Studio.app";
        const auto receipt = receipts / (product + ".plist");
        const auto receiptId = identifyFile(receipt);
        if (!protectedProductFile(receipt, receiptId) || !nativeProductCodeValid({root, root / "Contents/MacOS/mark-six-studio", {}, {}})) return {};
        const auto result = runNativeCommand("/usr/sbin/pkgutil", {"--pkg-info-plist", product});
        if (result.status || result.output.size() > 1024 * 1024 || identifyFile(receipt) != receiptId) return {};
        const auto data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(result.output.data()), CFIndex(result.output.size()));
        if (!data) return {};
        const auto plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, nullptr, nullptr); CFRelease(data);
        if (!plist) return {};
        const auto string = [](CFDictionaryRef dictionary, CFStringRef key) {
            const auto value = static_cast<CFStringRef>(CFDictionaryGetValue(dictionary, key)); std::array<char, 1024> bytes{};
            return value && CFGetTypeID(value) == CFStringGetTypeID() && CFStringGetCString(value, bytes.data(), CFIndex(bytes.size()), kCFStringEncodingUTF8) ? std::string(bytes.data()) : std::string();
        };
        std::string version;
        if (CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
            const auto dictionary = static_cast<CFDictionaryRef>(plist);
            if (string(dictionary, CFSTR("pkgid")) == product && string(dictionary, CFSTR("volume")) == "/") version = string(dictionary, CFSTR("pkg-version"));
        }
        CFRelease(plist);
        const auto infoPath = root / "Contents/Info.plist"; const auto info = readBoundedFile(infoPath, 1024 * 1024);
        const auto infoData = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(info.data()), CFIndex(info.size()));
        if (!infoData) return {};
        const auto dictionary = CFPropertyListCreateWithData(kCFAllocatorDefault, infoData, kCFPropertyListImmutable, nullptr, nullptr); CFRelease(infoData);
        if (!dictionary) return {};
        const auto bundleVersion = CFGetTypeID(dictionary) == CFDictionaryGetTypeID() ? string(static_cast<CFDictionaryRef>(dictionary), CFSTR("CFBundleShortVersionString")) : std::string();
        CFRelease(dictionary);
        const auto manifest = root / "Contents/Resources/maintenance-payload.txt"; const auto manifestId = identifyFile(manifest);
        if (version.empty() || version != bundleVersion || !protectedProductFile(manifest, manifestId)) return {};
        return {version, sha256Bytes(receiptId.sha256 + manifestId.sha256 + version), true};
    } catch (const std::exception&) { return {}; }
}
bool nativeInstallersIdle() {
    const auto bytes = proc_listallpids(nullptr, 0);
    if (bytes <= 0 || bytes > 1000000) return false;
    std::vector<pid_t> pids(std::size_t(bytes) + 128);
    const auto count = proc_listallpids(pids.data(), int(pids.size() * sizeof(pid_t)));
    if (count <= 0 || std::size_t(count) >= pids.size()) return false;
    for (int i = 0; i < count; ++i) {
        std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
        if (proc_pidpath(pids[std::size_t(i)], path.data(), uint32_t(path.size())) <= 0) continue;
        const auto name = fs::path(path.data()).filename();
        // PackageKit keeps its script service alive when idle. The native
        // Installer client remains alive until its transaction completes;
        // treating the idle service PID as a transaction blocks forever.
        if (name == "Installer" || name == "installer" || name == "InstallAssistant") return false;
    }
    return true;
}
void inspectNativeRegistrations(UninstallInspection& inspection) {
    Encoder state; state.text("M6MACREG1");
    std::vector<std::pair<fs::path, FileIdentity>> files;
    for (const auto* suffix : {".plist", ".bom"}) {
        const auto path = receipts / (product + suffix);
        try {
            if (!entryExists(path)) continue;
            const auto identity = identifyFile(path);
            if (!protectedProductFile(path, identity)) throw std::runtime_error("Product receipt owner or permissions are untrusted");
            files.emplace_back(path, identity);
        } catch (const std::exception& error) { inspection.inventory.issues.push_back({path, {}, error.what(), true}); }
    }
    state.number(files.size());
    for (const auto& [path, identity] : files) { state.path(path); state.identity(identity); }
    inspection.nativeState = state.bytes;
    inspection.nativeReview = "Matching package receipts: " + product + " (" + std::to_string(files.size()) + " native receipt files).\n";
    for (const auto& app : inspection.applications) inspection.nativeReview += "Unregister reviewed application URL: " + quotedReportField(pathUtf8(app.root)) + " for each listed account.\n";
}
void removeNativeRegistrations(const UninstallInspection& inspection) {
    Decoder state(inspection.nativeState);
    if (state.text() != "M6MACREG1") throw std::runtime_error("Unsupported native receipt recovery proof");
    const auto count = state.count(); if (count > 2) throw std::runtime_error("Invalid native product receipt count");
    bool present = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto path = state.path(); const auto identity = state.identity();
        if (path != receipts / (product + ".plist") && path != receipts / (product + ".bom")) throw std::runtime_error("Foreign package receipt rejected");
        if (!entryExists(path)) continue;
        if (identifyFile(path) != identity || !protectedProductFile(path, identity)) throw std::runtime_error("Package receipt changed since approval; retained for review");
        present = true;
    }
    state.end();
    if (present) {
        const auto result = runNativeCommand("/usr/sbin/pkgutil", {"--forget", product});
        if (result.status) throw std::runtime_error("Native package receipt removal failed: " + result.output);
    }
    for (const auto* suffix : {".plist", ".bom"}) if (entryExists(receipts / (product + suffix)))
        throw std::runtime_error("Product package receipt remains after native removal");
    for (const auto& app : inspection.applications) {
        // Never unregister a still-runnable reviewed copy after a partial purge.
        if (entryExists(app.executable)) throw std::runtime_error("A reviewed application executable remains; its registration is retained");
        for (const auto& user : inspection.users) {
            if (user.account.empty() || user.account.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("Invalid native account for application registration cleanup");
            const auto result = runNativeCommand("/usr/bin/sudo", {"-H", "-u", "#" + user.account, pathUtf8(lsregister), "-u", pathUtf8(app.root)});
            const auto self = processIdentity(currentProcessId());
            if (!self || self->uncertain || !protectedProductFile(self->executable)) throw std::runtime_error("Cannot authenticate native registration verification");
            const auto verified = runNativeCommand("/usr/bin/sudo", {"-H", "-u", "#" + user.account, pathUtf8(self->executable), "--verify-unregistered-application", pathUtf8(app.root)});
            if (verified.status) throw std::runtime_error("Application URL registration remains or cannot be inspected for account " + user.account + ": " + result.output + verified.output);
        }
    }
}
}
