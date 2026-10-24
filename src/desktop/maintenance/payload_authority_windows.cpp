#include "payload_authority.hpp"
#define NOMINMAX
#include <Windows.h>
#include <aclapi.h>
#include <array>

namespace marksix::maintenance {
bool protectedProductFile(const fs::path& path, const FileIdentity& expected) {
    try { validateContainedPath(path, path.parent_path()); } catch (const std::exception&) { return false; }
    const auto file = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct File { HANDLE value; ~File() { CloseHandle(value); } } fileGuard{file};
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information) || (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    if (expected.volume != information.dwVolumeSerialNumber ||
        expected.file != ((std::uint64_t(information.nFileIndexHigh) << 32) | information.nFileIndexLow) ||
        expected.bytes != ((std::uint64_t(information.nFileSizeHigh) << 32) | information.nFileSizeLow) ||
        expected.modified != std::int64_t((std::uint64_t(information.ftLastWriteTime.dwHighDateTime) << 32) | information.ftLastWriteTime.dwLowDateTime)) return false;
    PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    struct Security { PSECURITY_DESCRIPTOR value; ~Security() { LocalFree(value); } } security{descriptor};
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> admin{}, system{}; DWORD adminSize = DWORD(admin.size()), systemSize = DWORD(system.size());
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin.data(), &adminSize) || !CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &systemSize) ||
        !owner || !acl || (!EqualSid(owner, admin.data()) && !EqualSid(owner, system.data()))) return false;
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* entry = nullptr; if (!GetAce(acl, index, &entry)) return false;
        const auto type = reinterpret_cast<ACE_HEADER*>(entry)->AceType;
        if (type == ACCESS_DENIED_ACE_TYPE) continue;
        if (type != ACCESS_ALLOWED_ACE_TYPE) return false;
        const auto* ace = reinterpret_cast<ACCESS_ALLOWED_ACE*>(entry); const auto sid = const_cast<DWORD*>(&ace->SidStart);
        constexpr DWORD writes = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
        if ((ace->Mask & writes) && !EqualSid(sid, admin.data()) && !EqualSid(sid, system.data())) return false;
    }
    return true;
}
bool nativeProductCodeValid(const InstalledApplication& application) { return hasProductIdentity(application.root); }
}
