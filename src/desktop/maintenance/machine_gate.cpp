#include "machine_gate.hpp"
#include "durable_files.hpp"
#include "wire_codec.hpp"
#include "payload_authority.hpp"
#include <regex>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <array>
#else
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace marksix::maintenance {
namespace {
#ifdef _WIN32
constexpr wchar_t keyPath[] = L"SOFTWARE\\brucerry\\MarkSixStudio\\Maintenance";
struct Key { HKEY value = nullptr; ~Key() { if (value) RegCloseKey(value); } };
void protectedGateKey(HKEY key) {
    PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(reinterpret_cast<HANDLE>(key), SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) throw std::runtime_error("Cannot authenticate system maintenance registry state");
    struct Security { PSECURITY_DESCRIPTOR value; ~Security() { LocalFree(value); } } guard{descriptor};
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> admin{}, system{};
    DWORD adminSize = DWORD(admin.size()), systemSize = DWORD(system.size());
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin.data(), &adminSize) ||
        !CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &systemSize) || !owner ||
        (!EqualSid(owner, admin.data()) && !EqualSid(owner, system.data())) || !acl)
        throw std::runtime_error("System maintenance registry owner is untrusted");
    SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision) || !(control & SE_DACL_PROTECTED))
        throw std::runtime_error("System maintenance registry permissions inherit untrusted changes");
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* entry = nullptr;
        if (!GetAce(acl, index, &entry) || reinterpret_cast<ACE_HEADER*>(entry)->AceType != ACCESS_ALLOWED_ACE_TYPE)
            throw std::runtime_error("Unsupported system maintenance registry permissions");
        const auto* ace = reinterpret_cast<ACCESS_ALLOWED_ACE*>(entry);
        const auto sid = const_cast<DWORD*>(&ace->SidStart);
        constexpr DWORD writes = KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
        if ((ace->Mask & writes) && !EqualSid(sid, admin.data()) && !EqualSid(sid, system.data()))
            throw std::runtime_error("System maintenance registry permits unprivileged mutation");
    }
}
#else
const fs::path gatePath = "/Library/Preferences/com.brucerry.marksixstudio.maintenance";
void protectedGateFile() {
    if (!protectedProductFile(gatePath)) throw std::runtime_error("System maintenance gate owner or native permissions are untrusted");
}
#endif
}
std::string encodeMachineGate(const MachineGate& gate) {
    if (gate.nonce.size() != 64 || gate.nonce.find_first_not_of("0123456789abcdef") != std::string::npos ||
        !gate.stage.is_absolute() || gate.stage != gate.stage.lexically_normal() || !gate.executor.pid || !gate.executor.started || gate.executor.account.empty() || gate.executor.uncertain)
        throw std::runtime_error("Invalid system maintenance gate");
    Encoder out; out.text("M6MACHINEGATE1"); out.text(gate.nonce); out.path(gate.stage);
    out.number(gate.executor.pid); out.number(gate.executor.started); out.text(gate.executor.account); return out.bytes;
}
MachineGate decodeMachineGate(std::string_view bytes) {
    Decoder in(bytes); if (in.text() != "M6MACHINEGATE1") throw std::runtime_error("Unsupported system maintenance gate; administrator recovery is required");
    MachineGate gate; gate.nonce = in.text(); gate.stage = in.path();
    const auto pid = in.number(); if (pid > UINT32_MAX) throw std::runtime_error("Invalid maintenance gate process");
    gate.executor.pid = std::uint32_t(pid); gate.executor.started = in.number(); gate.executor.account = in.text(); in.end();
    (void)encodeMachineGate(gate); return gate;
}
std::optional<MachineGate> readMachineGate() {
#ifdef _WIN32
    Key key;
    const auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_READ | KEY_WOW64_64KEY, &key.value);
    if (status == ERROR_FILE_NOT_FOUND) return {};
    if (status != ERROR_SUCCESS) throw std::runtime_error("Cannot inspect system maintenance state");
    protectedGateKey(key.value);
    DWORD count = 0;
    const auto valueStatus = RegGetValueW(key.value, nullptr, L"Active", RRF_RT_REG_BINARY, nullptr, nullptr, &count);
    if (valueStatus != ERROR_SUCCESS || !count || count > 128 * 1024) throw std::runtime_error("Invalid or inaccessible system maintenance state");
    std::string bytes(count, '\0');
    if (RegGetValueW(key.value, nullptr, L"Active", RRF_RT_REG_BINARY, nullptr, bytes.data(), &count) != ERROR_SUCCESS || count != bytes.size())
        throw std::runtime_error("System maintenance state changed during inspection");
    return decodeMachineGate(bytes);
#else
    if (!entryExists(gatePath)) return {};
    protectedGateFile(); return decodeMachineGate(readBoundedFile(gatePath, 128 * 1024));
#endif
}
void publishMachineGate(const MachineGate& gate) {
    if (!administrator()) throw std::runtime_error("System maintenance requires native administrator authorization");
    const auto bytes = encodeMachineGate(gate);
    if (readMachineGate()) throw std::runtime_error("Another system maintenance operation needs completion or administrator recovery");
#ifdef _WIN32
    // HKLM's native ACL is the privilege boundary; no authorizing-account
    // environment path or caller-owned filesystem marker is consulted.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;BU)", SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("Cannot secure system maintenance registry state");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    Key key; DWORD disposition = 0;
    const auto created = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, nullptr, 0, KEY_SET_VALUE | KEY_READ | KEY_WOW64_64KEY, &security, &key.value, &disposition);
    LocalFree(descriptor);
    if (created != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) throw std::runtime_error("Another administrator reserved system maintenance state");
    protectedGateKey(key.value);
    if (RegSetValueExW(key.value, L"Active", 0, REG_BINARY, reinterpret_cast<const BYTE*>(bytes.data()), DWORD(bytes.size())) != ERROR_SUCCESS ||
        RegFlushKey(key.value) != ERROR_SUCCESS) throw std::runtime_error("Cannot persist system maintenance gate");
    const auto nativeNonce = pathFromUtf8(gate.nonce).native();
    if (RegSetValueExW(key.value, L"Operation", 0, REG_SZ, reinterpret_cast<const BYTE*>(nativeNonce.c_str()), DWORD((nativeNonce.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot persist native uninstall coordination identity");
#else
    validateContainedPath(gatePath, gatePath.parent_path());
    struct stat parent{};
    if (lstat(gatePath.parent_path().c_str(), &parent) || !S_ISDIR(parent.st_mode) || parent.st_uid != 0 || (parent.st_mode & 022))
        throw std::runtime_error("System maintenance parent directory is not protected");
    const int file = open(gatePath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (file < 0) throw std::runtime_error("Another administrator reserved system maintenance state");
    struct File { int value; ~File() { close(value); } } guard{file};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = write(file, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) throw std::runtime_error("System gate initialization was interrupted; administrator recovery is required");
        offset += std::size_t(count);
    }
    if (fchmod(file, 0644) || fsync(file)) throw std::runtime_error("Cannot persist readable system maintenance state");
    syncDirectory(gatePath.parent_path()); protectedGateFile();
#endif
}
void removeMachineGate(const std::string& nonce) {
    if (!administrator()) throw std::runtime_error("Administrator authorization is required to finish system maintenance");
    const auto gate = readMachineGate();
    if (!gate || gate->nonce != nonce) throw std::runtime_error("System maintenance operation identity changed");
#ifdef _WIN32
    if (RegDeleteKeyExW(HKEY_LOCAL_MACHINE, keyPath, KEY_WOW64_64KEY, 0) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot clear completed system maintenance gate");
#else
    protectedGateFile(); durableRemove(gatePath);
#endif
}
void updateMachineGateExecutor(const MachineGate& gate) {
    if (!administrator()) throw std::runtime_error("Administrator authorization is required for maintenance recovery");
    const auto previous = readMachineGate();
    if (!previous || previous->nonce != gate.nonce || previous->stage != gate.stage)
        throw std::runtime_error("Maintenance recovery refers to a different protected operation");
    const auto bytes = encodeMachineGate(gate);
#ifdef _WIN32
    Key key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_SET_VALUE | KEY_READ | KEY_WOW64_64KEY, &key.value) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot reopen protected maintenance state");
    protectedGateKey(key.value);
    if (RegSetValueExW(key.value, L"Active", 0, REG_BINARY, reinterpret_cast<const BYTE*>(bytes.data()), DWORD(bytes.size())) != ERROR_SUCCESS || RegFlushKey(key.value) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot persist maintenance recovery identity");
#else
    protectedGateFile(); durableWrite(gatePath, bytes);
    if (chmod(gatePath.c_str(), 0644)) throw std::runtime_error("Cannot expose the startup gate to other accounts");
    syncDirectory(gatePath.parent_path());
#endif
}
}
