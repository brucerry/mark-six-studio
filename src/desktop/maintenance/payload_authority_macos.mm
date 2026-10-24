#include "payload_authority.hpp"
#include <Security/Security.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace marksix::maintenance {
bool protectedProductFile(const fs::path& path, const FileIdentity& expected) {
    try {
        validateContainedPath(path, path.parent_path());
        const int file = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (file < 0) return false;
        struct File { int value; ~File() { close(value); } } fileGuard{file};
        struct stat info{};
        if (fstat(file, &info) || !S_ISREG(info.st_mode) || info.st_uid != 0 || (info.st_mode & 022) ||
            std::uint64_t(info.st_dev) != expected.volume || std::uint64_t(info.st_ino) != expected.file ||
            std::uint64_t(info.st_size) != expected.bytes || info.st_mtimespec.tv_sec * 1000000000LL + info.st_mtimespec.tv_nsec != expected.modified) return false;
        const auto acl = acl_get_fd_np(file, ACL_TYPE_EXTENDED);
        if (!acl) return errno == ENOENT; // No extended ACL on this still-open, verified object.
        struct Guard { acl_t value; ~Guard() { acl_free(value); } } guard{acl};
        acl_entry_t entry = nullptr; int position = ACL_FIRST_ENTRY, status = 0;
        while ((status = acl_get_entry(acl, position, &entry)) == 0) {
            position = ACL_NEXT_ENTRY; acl_tag_t tag{};
            if (acl_get_tag_type(entry, &tag)) return false;
            if (tag != ACL_EXTENDED_ALLOW) continue;
            acl_permset_t permissions{}; if (acl_get_permset(entry, &permissions)) return false;
            for (const auto write : {ACL_WRITE_DATA, ACL_APPEND_DATA, ACL_DELETE, ACL_WRITE_ATTRIBUTES, ACL_WRITE_EXTATTRIBUTES, ACL_WRITE_SECURITY, ACL_CHANGE_OWNER})
                if (acl_get_perm_np(permissions, write) != 0) return false;
        }
        // Darwin returns -1/EINVAL when the ACL iterator reaches its end.
        return status == -1 && errno == EINVAL;
    } catch (const std::exception&) { return false; }
}
bool nativeProductCodeValid(const InstalledApplication& application) {
    const auto path = pathUtf8(application.root);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.data()), CFIndex(path.size()), true);
    if (!url) return false;
    SecStaticCodeRef code = nullptr; SecRequirementRef requirement = nullptr;
    const auto created = SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code); CFRelease(url);
    if (created != errSecSuccess) return false;
    const auto parsed = SecRequirementCreateWithString(CFSTR("identifier \"com.brucerry.marksixstudio\""), kSecCSDefaultFlags, &requirement);
    // Resource ownership comes from protected payload metadata and exact hashes.
    // Do not ask Security.framework to read/hash an opaque legacy recording.
    const auto checked = parsed == errSecSuccess ? SecStaticCodeCheckValidity(code,
        kSecCSCheckAllArchitectures | kSecCSDoNotValidateResources, requirement) : errSecParam;
    if (requirement) CFRelease(requirement); CFRelease(code); return checked == errSecSuccess;
}
}
