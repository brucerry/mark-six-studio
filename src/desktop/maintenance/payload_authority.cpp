#include "payload_authority.hpp"
#include <algorithm>

namespace marksix::maintenance {
namespace {
fs::path manifestPath(const fs::path& root) {
#ifdef _WIN32
    return root / "maintenance-payload.txt";
#else
    return root / "Contents/Resources/maintenance-payload.txt";
#endif
}
std::vector<std::pair<fs::path, FileIdentity>> protectedMetadata(const InstalledApplication& application, const PayloadAuthority& authority) {
    std::vector<fs::path> paths{manifestPath(application.root), application.executable};
#ifdef __APPLE__
    paths.push_back(application.root / "Contents/_CodeSignature/CodeResources");
#endif
    std::vector<std::pair<fs::path, FileIdentity>> proofs;
    for (const auto& path : paths) {
        const auto identity = identifyFile(path);
        if (!authority.protectedFile(path, identity)) return {};
        proofs.emplace_back(path, identity);
    }
    if (!authority.codeValid(application)) return {};
    return proofs;
}

bool content(const FileIdentity& a, const FileIdentity& b) {
    // A copied payload has different inode/ancestor identities. Its current
    // identities still enter approval and are checked again during removal.
    return a.bytes == b.bytes && a.sha256 == b.sha256 && a.linkTarget == b.linkTarget;
}
}
std::vector<TrustedPayload> trustedPayloads(const std::vector<InstalledApplication>& applications,
    const std::vector<fs::path>& homes, const PayloadAuthority& authority, const std::vector<PreservedFile>& retainedLegacy) {
    std::vector<TrustedPayload> result;
    for (const auto& application : applications) {
        try {
            Inventory preserved; preserved.preservedLegacy = retainedLegacy;
            if (intentionallyPreserved(preserved, application.executable) || intentionallyPreserved(preserved, manifestPath(application.root))) continue;
#ifdef __APPLE__
            if (intentionallyPreserved(preserved, application.root / "Contents/_CodeSignature/CodeResources")) continue;
#endif
            const auto proofs = protectedMetadata(application, authority);
            if (proofs.empty()) continue;
            const auto inspection = inspectApplicationPayload(application, homes, retainedLegacy);
            if (!std::any_of(inspection.inventory.targets.begin(), inspection.inventory.targets.end(), [&](const auto& target) { return target.path == manifestPath(application.root); })) continue;
            if (!std::all_of(proofs.begin(), proofs.end(), [&](const auto& proof) {
                return std::any_of(inspection.inventory.targets.begin(), inspection.inventory.targets.end(), [&](const auto& target) {
                    return target.path == proof.first && target.identity == proof.second;
                });
            })) continue;
            result.push_back({application.root, inspection.inventory.targets});
        } catch (const std::exception&) { /* No trusted reference is manufactured from failed native checks. */ }
    }
    return result;
}
Inventory authorizePayload(const InstalledApplication& application, const PayloadInspection& payload,
    const std::vector<TrustedPayload>& references, const PayloadAuthority& authority) {
    auto result = payload.inventory;
    bool validCode = false;
    try { if (!intentionallyPreserved(result, application.executable)) validCode = authority.codeValid(application); } catch (const std::exception&) {}
    std::erase_if(result.targets, [&](const auto& target) {
        bool owned = false;
        if (validCode && target.category == Category::Application && target.boundary == application.root && !intentionallyPreserved(result, target.path)) {
            const auto relative = target.path.lexically_relative(application.root);
            for (const auto& reference : references) {
                owned = std::any_of(reference.files.begin(), reference.files.end(), [&](const auto& expected) {
                    return expected.path.lexically_relative(reference.root) == relative && content(target.identity, expected.identity);
                });
                if (owned) break;
            }
        }
        if (!owned) result.issues.push_back({target.path, target.account,
            "Payload ownership is not bound to protected native installation metadata or a trusted published manifest; preserved for review", true});
        return !owned;
    });
    return result;
}
}
