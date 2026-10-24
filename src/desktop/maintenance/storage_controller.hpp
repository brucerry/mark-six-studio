#pragma once
#include "storage_inventory.hpp"
#include "download_inventory.hpp"
#include "uninstall_inventory.hpp"
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <future>
#include <functional>
#include <optional>

namespace marksix::maintenance {
class StorageController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString registrationNotice READ registrationNotice NOTIFY changed)
    Q_PROPERTY(QString activeRoot READ activeRoot CONSTANT)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY changed)
    Q_PROPERTY(QVariantList locations READ locations NOTIFY changed)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY changed)
    Q_PROPERTY(QVariantList preservedFiles READ preservedFiles NOTIFY changed)
    Q_PROPERTY(QString preservedReport READ preservedReport NOTIFY changed)
    Q_PROPERTY(QString coverage READ coverage NOTIFY changed)
    Q_PROPERTY(QString downloadStatus READ downloadStatus NOTIFY changed)
public:
    using Discovery = std::function<std::vector<UserStorage>(bool, std::vector<InventoryIssue>&)>;
    using FolderOpener = std::function<bool(const QUrl&)>;
    using ApplicationDiscovery = std::function<std::vector<InstalledApplication>(const std::vector<UserStorage>&, std::vector<InventoryIssue>&)>;
    using CatalogFetcher = std::function<ReleaseCatalog()>;
    explicit StorageController(fs::path activeRoot, Discovery discovery, FolderOpener opener, QObject* parent = nullptr,
        ApplicationDiscovery applications = discoverApplications, CatalogFetcher catalog = fetchReleaseCatalog);
    ~StorageController() override;
    bool busy() const { return future_.valid(); }
    QString status() const { return status_; }
    QString activeRoot() const;
    QString registrationNotice() const { return registrationNotice_; }
    void setRegistrationNotice(const QString& notice) { registrationNotice_ = notice; emit changed(); }
    UninstallScope uninstallScope() const;
    QVariantList categories() const { return categories_; }
    QVariantList locations() const { return locations_; }
    QVariantList accounts() const { return accounts_; }
    QVariantList preservedFiles() const { return preserved_; }
    QString preservedReport() const { return report_; }
    QString coverage() const { return coverage_; }
    QString downloadStatus() const { return downloadStatus_; }
    Q_INVOKABLE void refresh(bool allUsers = false);
    Q_INVOKABLE void addLocation(const QUrl& folder, bool downloads, const QString& account = {});
    Q_INVOKABLE bool openFolder(const QString& path);
    Q_INVOKABLE void verifyDownloads();
signals:
    void changed();
private:
    struct Result { Inventory inventory; std::vector<UserStorage> users; std::vector<InstalledApplication> applications; std::optional<ReleaseCatalog> catalog; };
    void poll();
    fs::path root_;
    Discovery discovery_;
    FolderOpener opener_;
    ApplicationDiscovery applications_;
    CatalogFetcher fetchCatalog_;
    std::optional<ReleaseCatalog> catalog_;
    std::vector<AdditionalLocation> extra_;
    std::future<Result> future_;
    QTimer timer_;
    QString registrationNotice_;
    QString status_ = QStringLiteral("Inspect local storage to see locations and preserved legacy files.");
    QString report_, coverage_;
    QString downloadStatus_ = QStringLiteral("Downloaded installers have not been verified. Verification contacts the project's GitHub release service; it does not download or run installers.");
    QVariantList categories_, locations_, preserved_, accounts_;
    bool allUsers_ = false;
    bool fetchRequested_ = false;
};
}
