#include "update_controller.hpp"

#include <QAuthenticator>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSslError>
#include <QTemporaryDir>

namespace marksix::updates {
UpdateController::UpdateController(QString currentVersion, QString cacheRoot, Opener opener,
                                   QNetworkAccessManager* network, UpdateLimits limits, QObject* parent)
    : QObject(parent), currentVersion_(std::move(currentVersion)), cacheRoot_(std::move(cacheRoot)),
      opener_(std::move(opener)), network_(network ? network : new QNetworkAccessManager(this)), limits_(limits) {
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("The update request timed out. Check your connection and try again."));
    });
    connect(network_, &QNetworkAccessManager::authenticationRequired, this,
            [this](QNetworkReply* reply, QAuthenticator*) {
                if (reply == reply_) fail(QStringLiteral("The update service requested authentication. Try again later."));
            });
    connect(network_, &QNetworkAccessManager::proxyAuthenticationRequired, this,
            [this](const QNetworkProxy&, QAuthenticator*) {
                if (busy()) fail(QStringLiteral("The network proxy requires authentication. Check your connection and retry."));
            });
}

UpdateController::~UpdateController() { abandon(); }

bool UpdateController::busy() const {
    return state_ == QStringLiteral("checking") || state_ == QStringLiteral("downloading") ||
           state_ == QStringLiteral("opening");
}

bool UpdateController::canDownload() const {
    return release_ && release_->newer && !busy() && !canInstall();
}

double UpdateController::progress() const {
    if (canInstall() || state_ == QStringLiteral("opening")) return 1;
    if (state_ != QStringLiteral("downloading") || phase_ != Phase::Installer || totalBytes() <= 0) return -1;
    return double(receivedBytes_) / double(totalBytes());
}

void UpdateController::setState(const QString& state, const QString& status) {
    state_ = state;
    status_ = status;
    emit changed();
    emit progressChanged();
}

void UpdateController::abandon() {
    deadline_.stop();
    if (reply_) {
        auto* old = reply_.data();
        reply_.clear();
        old->abort();
        old->deleteLater();
    }
}

void UpdateController::clearDownload() {
    output_.reset();
    hash_.reset();
    directory_.reset();
    verifiedPath_.clear();
    expectedDigest_.clear();
    receivedBytes_ = 0;
}

void UpdateController::fail(const QString& message) {
    abandon();
    clearDownload();
    setState(QStringLiteral("error"), message);
}

void UpdateController::checkForUpdates() {
    if (busy()) return;
    clearDownload();
    release_.reset();
    setState(QStringLiteral("checking"), QStringLiteral("Checking for updates…"));
    begin(Phase::Metadata, latestReleaseUrl());
}

void UpdateController::downloadUpdate() {
    if (!canDownload()) return;
    clearDownload();
    setState(QStringLiteral("downloading"), QStringLiteral("Getting the installer checksum…"));
    begin(Phase::Checksum, release_->checksumUrl);
}

void UpdateController::cancel() {
    if (state_ != QStringLiteral("checking") && state_ != QStringLiteral("downloading")) return;
    const bool downloading = state_ == QStringLiteral("downloading");
    abandon();
    clearDownload();
    setState(release_ && release_->newer ? QStringLiteral("available") : QStringLiteral("idle"),
             downloading ? QStringLiteral("Download cancelled. You can try again.") : QStringLiteral("Update check cancelled."));
}

void UpdateController::begin(Phase phase, const QUrl& url) {
    phase_ = phase;
    originalUrl_ = url;
    body_.clear();
    receivedBytes_ = 0;
    deadline_.start(phase == Phase::Installer ? limits_.downloadTimeoutMs : limits_.checkTimeoutMs);
    request(url, 0);
    emit progressChanged();
}

void UpdateController::request(const QUrl& url, int redirects) {
    QNetworkRequest input(url);
    input.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    input.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    input.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    input.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    input.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    input.setTransferTimeout(limits_.transferTimeoutMs);
    input.setRawHeader("User-Agent", QByteArray("MarkSixStudio/") + currentVersion_.toUtf8());
    input.setRawHeader("Accept", phase_ == Phase::Metadata ? "application/vnd.github+json" : "application/octet-stream");
    if (phase_ == Phase::Metadata) input.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    auto* active = network_->get(input);
    reply_ = active;
    responseBytes_ = 0;
    active->setReadBufferSize(65536);
    connect(active, &QNetworkReply::finished, active, &QObject::deleteLater);
    connect(active, &QNetworkReply::readyRead, this, [this, active] { if (active == reply_) consume(active); });
    connect(active, &QNetworkReply::metaDataChanged, this, [this, active] {
        if (active != reply_ || active->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
        const auto length = active->header(QNetworkRequest::ContentLengthHeader);
        const qint64 limit = phase_ == Phase::Metadata ? maximumMetadataBytes :
            phase_ == Phase::Checksum ? maximumChecksumBytes : totalBytes();
        if (length.isValid() && length.toLongLong() > limit)
            fail(QStringLiteral("The update response is larger than expected. Download stopped."));
    });
    connect(active, &QNetworkReply::sslErrors, this, [this, active](const QList<QSslError>&) {
        if (active == reply_) fail(QStringLiteral("The update connection could not be verified. Check your connection and retry."));
    });
    connect(active, &QNetworkReply::finished, this, [this, active, redirects] { finished(active, redirects); });
}

void UpdateController::consume(QNetworkReply* active) {
    while (active == reply_ && active->bytesAvailable() > 0) {
        const auto chunk = active->read(65536);
        if (chunk.isEmpty()) break;
        responseBytes_ += chunk.size();
        const int status = active->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            if (responseBytes_ > 65536) fail(QStringLiteral("The update service returned an unexpected response. Try again later."));
            continue;
        }
        const qint64 limit = phase_ == Phase::Metadata ? maximumMetadataBytes :
            phase_ == Phase::Checksum ? maximumChecksumBytes : totalBytes();
        if (chunk.size() > limit - receivedBytes_) {
            fail(QStringLiteral("The update response is larger than expected. Download stopped."));
            return;
        }
        receivedBytes_ += chunk.size();
        if (phase_ == Phase::Installer) {
            if (!output_ || output_->write(chunk) != chunk.size()) {
                fail(QStringLiteral("The installer could not be saved. Check available disk space and try again."));
                return;
            }
            hash_->addData(chunk);
            emit progressChanged();
        } else body_.append(chunk);
    }
}

void UpdateController::finished(QNetworkReply* active, int redirects) {
    if (active != reply_) return;
    consume(active);
    if (active != reply_) return;
    reply_.clear();
    const int status = active->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        const auto location = active->url().resolved(active->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
        if (phase_ == Phase::Metadata || redirects >= 4 || !allowedAssetLocation(location, originalUrl_)) {
            fail(QStringLiteral("The update download redirected to an unsupported location. Try again later."));
            return;
        }
        request(location, redirects + 1);
        return;
    }
    deadline_.stop();
    if (status == 403 || status == 429) {
        fail(QStringLiteral("GitHub is limiting update requests or denied access. Wait a while and try again."));
        return;
    }
    if (active->error() != QNetworkReply::NoError || status != 200) {
        fail(status == 404 ? QStringLiteral("A published update could not be found. Try again later.") :
             QStringLiteral("Could not reach the update service. Check your connection and try again."));
        return;
    }
    if (phase_ == Phase::Metadata) {
        QString error;
        release_ = parseRelease(body_, currentVersion_, platformKey(), error);
        if (!release_) { fail(error); return; }
        setState(release_->newer ? QStringLiteral("available") : QStringLiteral("current"),
                 release_->newer ? QStringLiteral("Version %1 is available.").arg(release_->version) :
                 QStringLiteral("You are up to date. Latest published version: %1.").arg(release_->version));
    } else if (phase_ == Phase::Checksum) {
        const auto checksum = parseChecksum(body_, release_->installerName);
        if (!checksum || (!release_->apiDigest.isEmpty() && *checksum != release_->apiDigest)) {
            fail(QStringLiteral("The installer checksum could not be verified. Download stopped."));
            return;
        }
        expectedDigest_ = *checksum;
        if (cacheRoot_.isEmpty() || !QDir().mkpath(cacheRoot_)) {
            fail(QStringLiteral("The update cache could not be created. Check available disk space and retry."));
            return;
        }
        directory_ = std::make_unique<QTemporaryDir>(QDir(cacheRoot_).filePath(QStringLiteral("update-XXXXXX")));
        if (!directory_->isValid()) { fail(QStringLiteral("The update cache could not be created. Try again.")); return; }
        output_ = std::make_unique<QSaveFile>(QDir(directory_->path()).filePath(release_->installerName));
        if (!output_->open(QIODevice::WriteOnly)) {
            fail(QStringLiteral("The installer could not be saved. Check available disk space and retry."));
            return;
        }
        hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
        setState(QStringLiteral("downloading"), QStringLiteral("Downloading version %1…").arg(release_->version));
        begin(Phase::Installer, release_->installerUrl);
    } else {
        if (receivedBytes_ != release_->installerBytes || hash_->result() != expectedDigest_) {
            fail(QStringLiteral("Installer verification failed. Download it again before installing."));
            return;
        }
        if (!output_->commit()) {
            fail(QStringLiteral("The verified installer could not be saved. Check disk space and retry."));
            return;
        }
        verifiedPath_ = output_->fileName();
        output_.reset();
        hash_.reset();
        setState(QStringLiteral("ready"), QStringLiteral("Installer verified and ready."));
    }
}

void UpdateController::openInstaller() {
    if (!canInstall()) return;
    const QFileInfo info(verifiedPath_);
    QFile file(verifiedPath_);
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!info.isFile() || info.isSymbolicLink() || info.size() != totalBytes() || !file.open(QIODevice::ReadOnly) ||
        !digest.addData(&file) || digest.result() != expectedDigest_) {
        fail(QStringLiteral("The downloaded installer has changed. Download it again before installing."));
        return;
    }
    file.close();
    if (!opener_ || !opener_(QUrl::fromLocalFile(verifiedPath_))) {
        setState(QStringLiteral("ready"), QStringLiteral("The installer could not be opened. Try Install and close app again."));
        return;
    }
    directory_->setAutoRemove(false);
    setState(QStringLiteral("opening"), QStringLiteral("Finish the installation wizard, then reopen Mark Six Studio."));
    emit installerStarted();
}

void UpdateController::openReleasePage() {
    if (opener_) opener_(release_ ? release_->pageUrl : QUrl(QStringLiteral("https://github.com/brucerry/mark-six-studio/releases/latest")));
}
}
