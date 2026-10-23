#include "release_info.hpp"
#include "update_controller.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSslError>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>

using namespace marksix::updates;
namespace {
int checks = 0;
const char* stage = "setup";
void check(bool value) {
    ++checks;
    if (!value) throw std::runtime_error(std::string(stage) + " failed at check " + std::to_string(checks));
}
template<class Predicate> void until(Predicate condition) {
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    check(condition());
}

struct Fixture {
    QByteArray installer = "fixture installer bytes, never executed";
    QString name, checksumName;
    QUrl installerUrl, checksumUrl;
    QByteArray checksum;
    QJsonObject metadata;
    explicit Fixture(QString version = "0.2.0", QString platform = platformKey()) {
        const QString stem = QStringLiteral("MarkSixStudio-") + version + '-' + platform + QStringLiteral("-unsigned");
        name = platform == QStringLiteral("windows-x64") ? stem + QStringLiteral("-setup.exe") : stem + QStringLiteral(".pkg");
        checksumName = platform == QStringLiteral("windows-x64") ? name + QStringLiteral(".sha256") : stem + QStringLiteral(".sha256");
        const QString base = QStringLiteral("https://github.com/brucerry/mark-six-studio/releases/download/v") + version + '/';
        installerUrl = QUrl(base + name);
        checksumUrl = QUrl(base + checksumName);
        refresh();
        metadata["tag_name"] = 'v' + version;
        metadata["draft"] = false;
        metadata["prerelease"] = false;
        metadata["body"] = "## Changes\n- A fixture release.";
        metadata["html_url"] = QStringLiteral("https://github.com/brucerry/mark-six-studio/releases/tag/v") + version;
    }
    void refresh() {
        const auto digest = QCryptographicHash::hash(installer, QCryptographicHash::Sha256).toHex();
        checksum = digest + "  " + name.toUtf8() + '\n';
        metadata["assets"] = QJsonArray{
            QJsonObject{{"name", name}, {"size", double(installer.size())}, {"state", "uploaded"},
                        {"browser_download_url", installerUrl.toString()}, {"digest", QString::fromLatin1("sha256:" + digest)}},
            QJsonObject{{"name", checksumName}, {"size", double(checksum.size())}, {"state", "uploaded"},
                        {"browser_download_url", checksumUrl.toString()}}};
    }
    QByteArray json() const { return QJsonDocument(metadata).toJson(QJsonDocument::Compact); }
};

struct Response {
    QUrl url;
    QByteArray body;
    int status = 200;
    QUrl redirect;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    bool tlsError = false;
    bool hang = false;
    int chunkSize = 4096;
    qint64 contentLength = -1;
};

class FixtureReply final : public QNetworkReply {
public:
    FixtureReply(const QNetworkRequest& request, Response response, QObject* parent)
        : QNetworkReply(parent), response_(std::move(response)) {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        if (response_.status) setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response_.status);
        if (!response_.redirect.isEmpty()) setAttribute(QNetworkRequest::RedirectionTargetAttribute, response_.redirect);
        if (response_.contentLength >= 0) setHeader(QNetworkRequest::ContentLengthHeader, response_.contentLength);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            if (aborted_) return;
            emit metaDataChanged();
            if (aborted_) return;
            if (response_.tlsError) {
                emit sslErrors({QSslError(QSslError::CertificateUntrusted)});
                if (aborted_) return;
            }
            if (!response_.hang) sendChunk();
        });
    }
    void abort() override {
        if (aborted_ || isFinished()) return;
        aborted_ = true;
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("fixture cancellation"));
        setFinished(true);
        emit finished();
        // A cancelled transport can still have queued signals; these must be harmless.
        emit readyRead();
    }
    qint64 bytesAvailable() const override { return available_.size() + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char* output, qint64 maximum) override {
        const qint64 count = std::min(maximum, qint64(available_.size()));
        if (!count) return 0;
        std::memcpy(output, available_.constData(), size_t(count));
        available_.remove(0, qsizetype(count));
        return count;
    }
private:
    void sendChunk() {
        if (aborted_) return;
        const auto chunk = response_.body.mid(offset_, response_.chunkSize);
        offset_ += chunk.size();
        available_.append(chunk);
        if (!chunk.isEmpty()) emit readyRead();
        if (aborted_) return;
        if (offset_ < response_.body.size()) {
            QTimer::singleShot(1, this, [this] { sendChunk(); });
        } else {
            if (response_.error != QNetworkReply::NoError) setError(response_.error, QStringLiteral("fixture failure"));
            setFinished(true);
            emit finished();
        }
    }
    Response response_;
    QByteArray available_;
    qsizetype offset_ = 0;
    bool aborted_ = false;
};

class FixtureNetwork final : public QNetworkAccessManager {
public:
    std::deque<Response> responses;
    QList<QNetworkRequest> requests;
    bool policyOkay = true;
    Response& queue(const QUrl& url, QByteArray body = {}, int status = 200, QUrl redirect = {}) {
        responses.push_back({url, std::move(body), status, std::move(redirect)});
        return responses.back();
    }
protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& input, QIODevice*) override {
        requests.push_back(input);
        policyOkay = policyOkay && operation == GetOperation &&
            input.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt() == QNetworkRequest::ManualRedirectPolicy &&
            input.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt() == QNetworkRequest::Manual &&
            input.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt() == QNetworkRequest::Manual &&
            input.attribute(QNetworkRequest::AuthenticationReuseAttribute).toInt() == QNetworkRequest::Manual &&
            input.transferTimeout() > 0;
        Response response;
        if (responses.empty()) {
            policyOkay = false;
            response.status = 0;
            response.error = QNetworkReply::HostNotFoundError;
        } else {
            response = std::move(responses.front());
            responses.pop_front();
            policyOkay = policyOkay && response.url == input.url();
        }
        return new FixtureReply(input, std::move(response), this);
    }
};

struct Harness {
    QTemporaryDir cache;
    FixtureNetwork network;
    QList<QUrl> opened;
    bool allowOpen = true;
    int started = 0;
    UpdateController controller;
    explicit Harness(QString current = "0.1.0", UpdateLimits limits = {})
        : controller(std::move(current), cache.path(), [this](const QUrl& url) {
            opened.push_back(url);
            return allowOpen;
        }, &network, limits) {
        check(cache.isValid());
        QObject::connect(&controller, &UpdateController::installerStarted, [&] { ++started; });
    }
};

void discover(Harness& test, const Fixture& fixture) {
    test.network.queue(latestReleaseUrl(), fixture.json());
    test.controller.checkForUpdates();
    until([&] { return !test.controller.busy(); });
    check(test.controller.state() == QStringLiteral("available"));
    check(test.controller.canDownload() && !test.controller.canInstall());
    check(test.network.policyOkay);
}

void ready(Harness& test, const Fixture& fixture) {
    discover(test, fixture);
    test.network.queue(fixture.checksumUrl, fixture.checksum);
    test.network.queue(fixture.installerUrl, fixture.installer);
    test.controller.downloadUpdate();
    until([&] { return !test.controller.busy(); });
    check(test.controller.canInstall());
    check(test.opened.isEmpty() && test.started == 0);
}

bool cacheEmpty(const Harness& test) {
    return !QDirIterator(test.cache.path(), QDir::Files, QDirIterator::Subdirectories).hasNext();
}

void parserTests() {
    stage = "release policy";
    QString error;
    for (const auto& platform : {QStringLiteral("windows-x64"), QStringLiteral("macos-universal")}) {
        Fixture fixture("0.10.0", platform);
        const auto result = parseRelease(fixture.json(), "0.2.9", platform, error);
        check(result && result->newer && result->installerName == fixture.name && error.isEmpty());
        check(parseChecksum(fixture.checksum, fixture.name).has_value());
        fixture.metadata["assets"] = QJsonArray{};
        const auto current = parseRelease(fixture.json(), "0.10.0", platform, error);
        const auto older = parseRelease(fixture.json(), "1.0.0", platform, error);
        check(current && !current->newer && older && !older->newer);
    }
    const auto rejected = [&](QJsonObject object) {
        check(!parseRelease(QJsonDocument(object).toJson(), "0.1.0", platformKey(), error));
        check(!error.isEmpty());
    };
    for (const auto& tag : {"v01.2.3", "v1.2.3-beta.1", "v1.2.3\n", "v4294967296.0.0", "1.2.3", "v1.2"}) {
        Fixture fixture;
        fixture.metadata["tag_name"] = tag;
        rejected(fixture.metadata);
    }
    for (const auto& key : {"draft", "prerelease"}) {
        Fixture fixture;
        fixture.metadata[key] = true;
        rejected(fixture.metadata);
        fixture.metadata[key] = "false";
        rejected(fixture.metadata);
    }
    Fixture base;
    auto object = base.metadata;
    object["html_url"] = "https://example.invalid/release";
    rejected(object);
    object = base.metadata;
    object["assets"] = QJsonArray{};
    rejected(object);
    const auto assets = base.metadata["assets"].toArray();
    for (int index = 0; index < 2; ++index) {
        object = base.metadata;
        auto duplicate = assets;
        duplicate.append(assets[index]);
        object["assets"] = duplicate;
        rejected(object);
    }
    for (const auto& property : {"browser_download_url", "size", "digest", "state"}) {
        object = base.metadata;
        auto changed = assets;
        auto asset = changed[0].toObject();
        if (QString(property) == "size") asset[property] = double(maximumInstallerBytes + 1);
        else asset[property] = "invalid";
        changed[0] = asset;
        object["assets"] = changed;
        rejected(object);
    }
    check(!parseRelease("{}", "0.1.0", platformKey(), error));
    check(!parseRelease(base.json(), "0.1", platformKey(), error));
    check(!parseChecksum(base.checksum + "extra", base.name));
    check(!parseChecksum(base.checksum, "other.pkg"));
    check(!parseChecksum(QByteArray(64, '0') + "  ../" + base.name.toUtf8(), base.name));
    check(readableReleaseNotes("## Changes\n- **New** [feature](https://example.invalid/feature)") == "Changes\n- New feature");
    check(readableReleaseNotes("![image](https://example.invalid/image.png)") == "image");
    check(readableReleaseNotes("`--example` and __text__") == "--example and text");
    check(allowedAssetLocation(base.installerUrl, base.installerUrl));
    check(allowedAssetLocation(QUrl("https://release-assets.githubusercontent.com/asset?token=fixture"), base.installerUrl));
    check(allowedAssetLocation(QUrl("https://objects.githubusercontent.com/asset"), base.installerUrl));
    for (const auto& url : {"http://release-assets.githubusercontent.com/asset", "https://example.invalid/asset",
                            "https://release-assets.githubusercontent.com.evil.invalid/asset", "file:///tmp/installer.pkg",
                            "https://github.com/another/repository", "https://user@objects.githubusercontent.com/asset",
                            "https://objects.githubusercontent.com:443/asset", "https://objects.githubusercontent.com/asset#fragment"})
        check(!allowedAssetLocation(QUrl(url), base.installerUrl));
}

void checkTests() {
    stage = "offline startup and version checks";
    Harness current("0.2.0");
    check(current.network.requests.isEmpty() && !current.controller.busy());
    Fixture fixture;
    current.network.queue(latestReleaseUrl(), fixture.json());
    current.controller.checkForUpdates();
    until([&] { return !current.controller.busy(); });
    check(current.controller.state() == "current" && !current.controller.canDownload());
    current.controller.openInstaller();
    check(current.opened.isEmpty());

    stage = "check errors and retry";
    for (const int status : {0, 403, 429, 404, 500}) {
        Harness test;
        auto& response = test.network.queue(latestReleaseUrl(), {}, status);
        if (!status) response.error = QNetworkReply::HostNotFoundError;
        test.controller.checkForUpdates();
        until([&] { return !test.controller.busy(); });
        check(test.controller.state() == "error" && !test.controller.canDownload() && cacheEmpty(test));
        discover(test, fixture);
    }
    for (int kind = 0; kind < 4; ++kind) {
        Harness test;
        auto& response = test.network.queue(latestReleaseUrl(), kind == 0 ? QByteArray("invalid json") : fixture.json());
        if (kind == 1) response.tlsError = true;
        if (kind == 2) response.contentLength = maximumMetadataBytes + 1;
        if (kind == 3) { response.status = 302; response.redirect = QUrl("https://example.invalid/api"); }
        test.controller.checkForUpdates();
        until([&] { return !test.controller.busy(); });
        check(test.controller.state() == "error" && test.network.requests.size() == 1);
    }
    stage = "cancel, stale signals and timeout";
    Harness cancelled;
    cancelled.network.queue(latestReleaseUrl()).hang = true;
    cancelled.controller.checkForUpdates();
    cancelled.controller.cancel();
    check(cancelled.controller.state() == "idle");
    discover(cancelled, fixture);
    check(cancelled.network.requests.size() == 2);
    Harness timed("0.1.0", {25, 25, 25});
    timed.network.queue(latestReleaseUrl()).hang = true;
    timed.controller.checkForUpdates();
    until([&] { return !timed.controller.busy(); });
    check(timed.controller.state() == "error" && timed.controller.status().contains("timed out"));
    Harness oversized;
    oversized.network.queue(latestReleaseUrl(), QByteArray(qsizetype(maximumMetadataBytes + 1), 'x')).chunkSize = 65536;
    oversized.controller.checkForUpdates();
    until([&] { return !oversized.controller.busy(); });
    check(oversized.controller.state() == "error");
}

void downloadTests() {
    Fixture fixture;
    stage = "verified CDN download and explicit handoff";
    Harness success;
    discover(success, fixture);
    const QUrl checksumCdn("https://release-assets.githubusercontent.com/checksum?fixture=1");
    const QUrl installerCdn("https://objects.githubusercontent.com/installer?fixture=2");
    success.network.queue(fixture.checksumUrl, {}, 302, checksumCdn);
    success.network.queue(checksumCdn, fixture.checksum);
    success.network.queue(fixture.installerUrl, {}, 302, installerCdn);
    success.network.queue(installerCdn, fixture.installer);
    success.controller.downloadUpdate();
    until([&] { return !success.controller.busy(); });
    check(success.controller.canInstall() && success.controller.progress() == 1);
    check(success.controller.receivedBytes() == fixture.installer.size() && success.opened.isEmpty());
    check(success.network.policyOkay && success.network.responses.empty());
    success.allowOpen = false;
    success.controller.openInstaller();
    check(success.controller.canInstall() && success.started == 0 && success.opened.size() == 1);
    success.allowOpen = true;
    success.controller.openInstaller();
    check(success.started == 1 && success.controller.state() == "opening");
    QFile installed(success.opened.back().toLocalFile());
    check(installed.open(QIODevice::ReadOnly) && installed.readAll() == fixture.installer);

    stage = "checksum and installer mismatch rejection";
    for (int kind = 0; kind < 5; ++kind) {
        Harness test;
        discover(test, fixture);
        QByteArray checksum = fixture.checksum;
        QByteArray bytes = fixture.installer;
        if (kind == 0) checksum = QByteArray(64, '0') + "  " + fixture.name.toUtf8() + '\n';
        if (kind == 1) checksum = checksum.replace(fixture.name.toUtf8(), "wrong.pkg");
        if (kind == 2) bytes[0] = 'X';
        if (kind == 3) bytes.chop(1);
        if (kind == 4) bytes.append('x');
        test.network.queue(fixture.checksumUrl, checksum);
        if (kind >= 2) test.network.queue(fixture.installerUrl, bytes);
        test.controller.downloadUpdate();
        until([&] { return !test.controller.busy(); });
        check(test.controller.state() == "error" && test.controller.canDownload() && cacheEmpty(test));
        test.controller.openInstaller();
        check(test.opened.isEmpty() && test.started == 0);
    }
    stage = "post-download modification";
    Harness modified;
    ready(modified, fixture);
    QDirIterator files(modified.cache.path(), QDir::Files, QDirIterator::Subdirectories);
    check(files.hasNext());
    QFile changed(files.next());
    check(changed.open(QIODevice::WriteOnly));
    check(changed.write(QByteArray(fixture.installer.size(), 'x')) == fixture.installer.size());
    changed.close();
    modified.controller.openInstaller();
    check(modified.controller.state() == "error" && modified.opened.isEmpty() && cacheEmpty(modified));

    stage = "cancel active stream";
    Harness cancel;
    Fixture large;
    large.installer = QByteArray(100000, 'x');
    large.refresh();
    discover(cancel, large);
    cancel.network.queue(large.checksumUrl, large.checksum);
    cancel.network.queue(large.installerUrl, large.installer).chunkSize = 1000;
    cancel.controller.downloadUpdate();
    until([&] { return cancel.controller.progress() > 0; });
    cancel.controller.cancel();
    check(cancel.controller.state() == "available" && cancel.controller.canDownload() && cacheEmpty(cancel));
    check(cancel.opened.isEmpty());

    stage = "download timeout";
    Harness timeout("0.1.0", {1000, 25, 1000});
    discover(timeout, fixture);
    timeout.network.queue(fixture.checksumUrl, fixture.checksum);
    timeout.network.queue(fixture.installerUrl).hang = true;
    timeout.controller.downloadUpdate();
    until([&] { return !timeout.controller.busy(); });
    check(timeout.controller.state() == "error" && cacheEmpty(timeout));
}

void redirectTests() {
    stage = "unapproved and excessive redirects";
    const Fixture fixture;
    for (const auto& url : {"https://example.invalid/update.pkg", "http://objects.githubusercontent.com/asset",
                            "https://github.com/another/repository"}) {
        Harness test;
        discover(test, fixture);
        test.network.queue(fixture.checksumUrl, {}, 302, QUrl(url));
        test.controller.downloadUpdate();
        until([&] { return !test.controller.busy(); });
        check(test.controller.state() == "error" && test.network.requests.size() == 2 && cacheEmpty(test));
    }
    Harness loop;
    discover(loop, fixture);
    const QUrl cdn("https://release-assets.githubusercontent.com/loop");
    loop.network.queue(fixture.checksumUrl, {}, 302, cdn);
    for (int index = 0; index < 4; ++index) loop.network.queue(cdn, {}, 302, cdn);
    loop.controller.downloadUpdate();
    until([&] { return !loop.controller.busy(); });
    check(loop.controller.state() == "error" && loop.network.requests.size() == 6);
}

int liveDownload(QCoreApplication& app) {
    QTemporaryDir cache;
    // Explicit developer probe: exercise real TLS/CDN/checksums without opening software.
    UpdateController controller("0.0.0", cache.path(), [](const QUrl&) { return false; });
    QObject::connect(&controller, &UpdateController::changed, &app, [&] {
        if (controller.state() == "available") {
            QTimer::singleShot(0, &controller, &UpdateController::downloadUpdate);
        } else if (controller.state() == "ready") {
            std::printf("PASS live GitHub release %s: %lld verified bytes; installer not opened\n",
                        controller.latestVersion().toUtf8().constData(), static_cast<long long>(controller.totalBytes()));
            app.exit(0);
        } else if (controller.state() == "error") {
            std::fprintf(stderr, "%s\n", controller.status().toUtf8().constData());
            app.exit(1);
        }
    });
    QTimer::singleShot(0, &controller, &UpdateController::checkForUpdates);
    return app.exec();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().contains(QStringLiteral("--live-download"))) return liveDownload(app);
        parserTests();
        checkTests();
        downloadTests();
        redirectTests();
        std::printf("PASS %d application update checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
