#pragma once

#include "release_info.hpp"
#include "installer_lease.hpp"

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <memory>

class QCryptographicHash;
class QNetworkReply;
class QSaveFile;
class QTemporaryDir;

namespace marksix::updates {
struct UpdateLimits {
    int checkTimeoutMs = 30000;
    int downloadTimeoutMs = 600000;
    int transferTimeoutMs = 15000;
};

class UpdateController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canDownload READ canDownload NOTIFY changed)
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(qint64 receivedBytes READ receivedBytes NOTIFY progressChanged)
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY progressChanged)
public:
    using Opener = std::function<bool(const QUrl&)>;
    explicit UpdateController(QString currentVersion, QString cacheRoot, Opener opener,
                              QNetworkAccessManager* network = nullptr, UpdateLimits limits = {},
                              QObject* parent = nullptr);
    ~UpdateController() override;
    QString currentVersion() const { return currentVersion_; }
    QString latestVersion() const { return release_ ? release_->version : QString(); }
    QString releaseNotes() const { return release_ ? readableReleaseNotes(release_->notes) : QString(); }
    QString state() const { return state_; }
    QString status() const { return status_; }
    bool busy() const;
    bool canDownload() const;
    bool canInstall() const { return state_ == QStringLiteral("ready"); }
    double progress() const;
    qint64 receivedBytes() const { return receivedBytes_; }
    qint64 totalBytes() const { return release_ ? release_->installerBytes : 0; }
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void downloadUpdate();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void openInstaller();
    Q_INVOKABLE void openReleasePage();
    void setInstallerLauncher(std::function<void(const std::filesystem::path&, marksix::maintenance::InstallerCompletion)> launcher) { installerLauncher_ = std::move(launcher); }
    void preserveDownloadForMaintenance();
signals:
    void changed();
    void progressChanged();
    void installerStarted();
private:
    enum class Phase { Metadata, Checksum, Installer };
    void begin(Phase phase, const QUrl& url);
    void request(const QUrl& url, int redirects);
    void consume(QNetworkReply* reply);
    void finished(QNetworkReply* reply, int redirects);
    void abandon();
    void clearDownload();
    void fail(const QString& message);
    void setState(const QString& state, const QString& status);
    QString currentVersion_;
    QString cacheRoot_;
    Opener opener_;
    std::function<void(const std::filesystem::path&, marksix::maintenance::InstallerCompletion)> installerLauncher_;
    QNetworkAccessManager* network_;
    UpdateLimits limits_;
    QPointer<QNetworkReply> reply_;
    QTimer deadline_;
    Phase phase_ = Phase::Metadata;
    QUrl originalUrl_;
    QByteArray body_;
    QByteArray expectedDigest_;
    qint64 receivedBytes_ = 0;
    qint64 responseBytes_ = 0;
    std::optional<ReleaseInfo> release_;
    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<QSaveFile> output_;
    std::unique_ptr<QCryptographicHash> hash_;
    QString verifiedPath_;
    QString state_ = QStringLiteral("idle");
    QString status_ = QStringLiteral("Check for a newer version when you are ready.");
};
}
