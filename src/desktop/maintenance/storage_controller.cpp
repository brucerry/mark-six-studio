#include "storage_controller.hpp"
#include "native_runtime.hpp"
#include "installer_lease.hpp"
#include <QFileInfo>
#include <QLocale>
#include <array>
#include <algorithm>

namespace marksix::maintenance {
namespace {
QString qpath(const fs::path& value) {
    try { return QString::fromStdU16String(value.u16string()); }
    catch (const std::exception&) { return QString::fromStdString(quotedReportField(pathUtf8(value))); }
}
QString qtext(const std::string& value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); }
QString folderTarget(const QFileInfo& info) {
    return info.isBundle() ? info.absolutePath() : info.absoluteFilePath();
}
}
StorageController::StorageController(fs::path root, Discovery discovery, FolderOpener opener, QObject* parent, ApplicationDiscovery applications, CatalogFetcher catalog)
    : QObject(parent), root_(std::move(root)), discovery_(std::move(discovery)), opener_(std::move(opener)), applications_(std::move(applications)), fetchCatalog_(std::move(catalog)) {
    timer_.setInterval(30); connect(&timer_, &QTimer::timeout, this, &StorageController::poll);
}
StorageController::~StorageController() { if (future_.valid()) future_.wait(); }
QString StorageController::activeRoot() const { return qpath(root_); }
UninstallScope StorageController::uninstallScope() const {
    UninstallScope scope; const auto account = currentAccount();
    scope.additional.push_back({account, root_, LocationKind::SavedData});
    for (const auto& location : extra_) if (location.path != root_ || location.account != account || location.kind != LocationKind::SavedData) scope.additional.push_back(location);
    return scope;
}
void StorageController::refresh(bool allUsers) {
    if (busy()) return;
    allUsers_ = allUsers; status_ = QStringLiteral("Inspecting local files without changing saved records…");
    const auto root = root_; const auto extra = extra_;
    const auto discovery = discovery_; const auto applications = applications_; const auto account = currentAccount();
    const auto catalog = catalog_; const auto fetchCatalog = fetchCatalog_; const bool fetch = fetchRequested_; fetchRequested_ = false;
    future_ = std::async(std::launch::async, [=] {
        Result result; std::vector<InventoryIssue> issues;
        result.users = discovery(allUsers, issues);
        for (auto& user : result.users) if (user.account == account) {
            if (root != user.dataRoot) user.additionalDataRoots.push_back(root);
        }
        for (const auto& location : extra) for (auto& user : result.users) if (user.account == location.account) {
            auto& paths = location.kind == LocationKind::SavedData ? user.additionalDataRoots : user.additionalDownloadRoots;
            paths.push_back(location.path);
        }
        std::vector<fs::path> protectedFiles;
        for (const auto& user : result.users) {
            const auto leased = protectedInstallerFiles(user, issues);
            protectedFiles.insert(protectedFiles.end(), leased.begin(), leased.end());
        }
        result.inventory = inspectStorage({Operation::Uninstall, result.users, root, protectedFiles});
        result.catalog = fetch ? std::optional<ReleaseCatalog>(fetchCatalog()) : catalog;
        if (result.catalog) inspectDownloads(result.inventory, result.users, *result.catalog, protectedFiles);
        result.inventory.issues.insert(result.inventory.issues.end(), issues.begin(), issues.end());
        result.applications = applications(result.users, result.inventory.issues);
        preserveApplicationRecordingHints(result.inventory, result.users, result.applications);
        std::vector<fs::path> homes;
        for (const auto& user : result.users) homes.push_back(user.home);
        for (const auto& application : result.applications) {
            const auto payload = inspectApplicationPayload(application, homes, result.inventory.preservedLegacy);
            result.inventory.targets.insert(result.inventory.targets.end(), payload.inventory.targets.begin(), payload.inventory.targets.end());
            result.inventory.issues.insert(result.inventory.issues.end(), payload.inventory.issues.begin(), payload.inventory.issues.end());
            for (const auto& legacy : payload.inventory.preservedLegacy)
                preserveLegacyFile(result.inventory, legacy.path, legacy.account, legacy.reason);
        }
        return result;
    });
    timer_.start(); emit changed();
}
void StorageController::verifyDownloads() {
    if (busy()) return;
    fetchRequested_ = true; refresh(allUsers_);
}
void StorageController::addLocation(const QUrl& folder, bool downloads, const QString& selectedAccount) {
    if (busy() || !folder.isLocalFile()) return;
    try {
        const auto path = fs::path(folder.toLocalFile().toStdU16String()).lexically_normal();
        const auto account = selectedAccount.isEmpty() ? currentAccount() : selectedAccount.toStdString();
        std::vector<InventoryIssue> issues; const auto users = discovery_(true, issues);
        const auto owner = std::find_if(users.begin(), users.end(), [&](const auto& user) { return user.account == account; });
        if (owner == users.end()) throw std::runtime_error("Selected native OS account could not be resolved");
        for (const auto& user : users) {
            validateBoundary(path, user.home);
            if (user.account == account) continue;
            for (auto parent = path; !parent.empty() && parent != parent.root_path(); parent = parent.parent_path()) {
                std::error_code error;
                if (fs::equivalent(parent, user.home, error) && !error) throw std::runtime_error("Select the OS account that owns this profile location");
            }
        }
        validateContainedPath(path / ".inspection", path);
        if (!fs::is_directory(fs::symlink_status(path))) throw std::runtime_error("Select an accessible physical folder");
        const AdditionalLocation location{account, path, downloads ? LocationKind::Downloads : LocationKind::SavedData};
        if (std::find(extra_.begin(), extra_.end(), location) == extra_.end()) extra_.push_back(location);
        refresh(allUsers_ || account != currentAccount());
    } catch (const std::exception& error) { status_ = qtext(error.what()); emit changed(); }
}
bool StorageController::openFolder(const QString& path) {
    bool reviewed = false;
    for (const auto& row : locations_) if (row.toMap().value(QStringLiteral("path")).toString() == path) reviewed = true;
    if (!reviewed) {
        status_ = QStringLiteral("Refresh storage before opening an unreviewed location.");
        emit changed(); return false;
    }
    const QFileInfo info(path);
    const auto folder = folderTarget(info);
    if (!info.isDir() || !QFileInfo(folder).isDir()) {
        status_ = QStringLiteral("This location is unavailable. Refresh storage and try again: ") + path;
        emit changed(); return false;
    }
    if (opener_(QUrl::fromLocalFile(folder))) return true;
    status_ = QStringLiteral("Could not open folder: ") + folder;
    emit changed(); return false;
}
void StorageController::poll() {
    if (!future_.valid() || future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    timer_.stop();
    try {
        const auto result = future_.get(); categories_.clear(); locations_.clear(); preserved_.clear(); accounts_.clear();
        auto accounts = result.users;
        std::stable_sort(accounts.begin(), accounts.end(), [](const auto& a, const auto& b) { return a.account == currentAccount() && b.account != currentAccount(); });
        for (const auto& user : accounts) accounts_.push_back(QVariantMap{{QStringLiteral("account"), qtext(user.account)},
            {QStringLiteral("label"), qtext(user.account) + QStringLiteral(" · ") + qpath(user.home)}});
        catalog_ = result.catalog;
        downloadStatus_ = !catalog_ ? QStringLiteral("Downloaded installers have not been verified. Verification contacts GitHub for release identities; it does not download or run installers.") :
            catalog_->complete ? QStringLiteral("Published release identities verified. Only matching file hashes are attributable; nonstandard and nested download folders require selection.") :
            QStringLiteral("Published release verification is incomplete. Uncertain downloads are preserved and inspection limits are listed below.");
        std::array<qulonglong, 7> bytes{}, counts{};
        for (const auto& target : result.inventory.targets) {
            const auto index = std::size_t(target.category); bytes[index] += target.identity.bytes; ++counts[index];
        }
        const QStringList labels{QStringLiteral("Disposable caches"), QStringLiteral("Saved records and learning history"),
            QStringLiteral("Migration recovery copies"), QStringLiteral("Verified downloaded installers"),
            QStringLiteral("Application files"), QStringLiteral("OS registrations"), QStringLiteral("Maintenance files")};
        for (qsizetype i = 0; i < labels.size(); ++i) categories_.push_back(QVariantMap{
            {QStringLiteral("name"), labels[i]}, {QStringLiteral("files"), QVariant::fromValue(counts[std::size_t(i)])},
            {QStringLiteral("size"), QLocale().formattedDataSize(qint64(bytes[std::size_t(i)]))},
            {QStringLiteral("complete"), result.inventory.complete() &&
                (i != qsizetype(Category::Installer) || (catalog_ && catalog_->complete)) &&
                i != qsizetype(Category::Registration) && i != qsizetype(Category::Maintenance)}});
        const auto location = [&](const fs::path& path, const std::string& account, const QString& kind) {
            const QFileInfo info(qpath(path));
            locations_.push_back(QVariantMap{{QStringLiteral("path"), qpath(path)}, {QStringLiteral("account"), qtext(account)},
                {QStringLiteral("kind"), kind}, {QStringLiteral("available"), info.isDir()},
                {QStringLiteral("openPath"), folderTarget(info)},
                {QStringLiteral("openLabel"), info.isBundle() ? QStringLiteral("Open containing folder") : QStringLiteral("Open folder")}});
        };
        for (const auto& user : result.users) {
            location(user.dataRoot, user.account, QStringLiteral("Saved records"));
            for (const auto& path : user.additionalDataRoots) location(path, user.account, QStringLiteral("Custom saved records"));
            for (const auto& cache : nativeCacheRoots(user)) location(cache, user.account, cache == user.cacheRoot ? QStringLiteral("Cache") : QStringLiteral("Native cache"));
            location(user.downloads, user.account, catalog_ ? QStringLiteral("Reviewed downloads") : QStringLiteral("Downloads — installer validation pending"));
            for (const auto& path : user.additionalDownloadRoots) location(path, user.account, catalog_ ? QStringLiteral("Reviewed additional downloads") : QStringLiteral("Additional downloads — installer validation pending"));
        }
        for (const auto& application : result.applications) location(application.root, application.account, QStringLiteral("Installed application"));
        for (const auto& file : result.inventory.preservedLegacy) preserved_.push_back(QVariantMap{
            {QStringLiteral("path"), qpath(file.path)}, {QStringLiteral("account"), qtext(file.account)}, {QStringLiteral("reason"), qtext(file.reason)}});
        report_ = qtext(preservedLegacyReport(result.inventory));
        coverage_ = allUsers_ ? QStringLiteral("Discoverable OS accounts were inspected. ") : QStringLiteral("Only the current OS account was inspected. ");
        coverage_ += QStringLiteral("Unregistered custom folders and nonstandard download locations require selection. These totals are a storage preview; the native helper builds a fresh final inventory before deletion.");
        for (const auto& issue : result.inventory.issues)
            coverage_ += QStringLiteral("\n%1 · %2: %3").arg(qtext(issue.account), qpath(issue.path), qtext(issue.reason));
        status_ = result.inventory.complete() ? QStringLiteral("Inspection complete for validated local files. No files changed.") :
            QStringLiteral("Inspection has unresolved locations or files. Sizes show only validated files; unavailable locations are not empty.");
    } catch (const std::exception& error) {
        for (auto& row : categories_) { auto values = row.toMap(); values[QStringLiteral("complete")] = false; row = values; }
        status_ = QStringLiteral("Inspection failed: ") + qtext(error.what()) + QStringLiteral(". Any displayed values are from the previous inspection; refresh is required.");
    }
    emit changed();
}
}
