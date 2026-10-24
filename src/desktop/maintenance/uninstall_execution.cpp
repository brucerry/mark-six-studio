#include "uninstall_execution.hpp"
#include "durable_files.hpp"
#include <algorithm>
#include <sstream>

namespace marksix::maintenance {
std::string uninstallRecoveryReview(const UninstallJournal& journal) {
    auto result = uninstallReview(journal.inspection());
    const auto states = journal.currentStates();
    result += "\nRecovery status: " + std::to_string(std::count(states.begin(), states.end(), RemovalState::Removed)) +
        " previously approved files are already absent. A new approval is required for every retry.\n";
    for (const auto& issue : journal.failures()) result += "Remaining: " + quotedReportField(pathUtf8(issue.path)) +
        " | account " + quotedReportField(issue.account) + " | " + quotedReportField(issue.reason) + "\n";
    return result;
}
UninstallOutcome executeUninstall(UninstallJournal& journal, const std::string& approvalDigest,
    bool permitIncomplete, const UninstallExecutionEnvironment& environment) {
    if (!environment.idle || !environment.removeRegistrations) throw std::runtime_error("Native uninstall provider is unavailable");
    if (!environment.idle()) throw std::runtime_error("Close other application instances and native installers before deletion");
    journal.approve(approvalDigest, permitIncomplete);
    std::uint64_t completed = 0;
    const auto total = journal.inspection().inventory.targets.size();
    journal.execute({}, [&](std::string_view phase) {
        if (!environment.idle()) throw std::runtime_error("Application or native installer activity resumed; remaining targets are retained");
        if (phase == "removed") ++completed;
        if (environment.progress) environment.progress("Removing the approved current-app files; legacy recordings are preserved", completed, total);
    });
    UninstallOutcome outcome; outcome.remaining = journal.failures();
    const auto& inspection = journal.inspection();
    if (journal.filesRemoved()) {
        try {
            if (!environment.idle()) throw std::runtime_error("Native activity resumed before registration removal");
            environment.removeRegistrations(inspection);
        } catch (const std::exception& error) { outcome.remaining.push_back({{}, {}, error.what(), true}); }
    }
    auto directories = inspection.ownedDirectories;
    std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) { return a.path.native().size() > b.path.native().size(); });
    std::vector<fs::path> retained;
    for (const auto& file : inspection.inventory.preservedLegacy) if (entryExists(file.path.parent_path())) retained.push_back(file.path.parent_path());
    for (const auto& directory : directories) {
        try {
            if (!entryExists(directory.path)) continue;
            if (!environment.idle()) throw std::runtime_error("Native activity resumed before empty-directory cleanup");
            if (!removeVerifiedEmptyDirectory(directory.path, directory.identity)) retained.push_back(directory.path);
        } catch (const std::exception& error) { outcome.remaining.push_back({directory.path, directory.account, error.what(), true}); }
    }
    for (const auto& issue : inspection.inventory.issues) if (issue.unresolved) outcome.remaining.push_back(issue);
    outcome.complete = journal.filesRemoved() && outcome.remaining.empty();
    std::ostringstream report;
    report << (outcome.complete ? "Current-app files and native registrations removed. Final helper cleanup is still pending.\n" :
        "Cleanup is blocked or partially completed. Remaining current-app work is listed below.\n");
    report << "\n" << preservedLegacyReport(inspection.inventory);
    if (!retained.empty()) {
        std::sort(retained.begin(), retained.end()); retained.erase(std::unique(retained.begin(), retained.end()), retained.end());
        report << "\nDirectories retained for legacy, unrelated or remaining files (never recursively deleted):\n";
        for (const auto& path : retained) report << quotedReportField(pathUtf8(path)) << '\n';
    }
    if (!outcome.remaining.empty()) {
        report << "\nRemaining work / incomplete inspection:\n";
        for (const auto& issue : outcome.remaining) report << "Path: " << quotedReportField(pathUtf8(issue.path)) << "\nAccount: " <<
            quotedReportField(issue.account) << "\nReason: " << quotedReportField(issue.reason) << "\n\n";
    }
    outcome.report = report.str(); return outcome;
}
}
