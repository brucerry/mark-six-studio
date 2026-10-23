#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <optional>

namespace marksix::updates {
inline constexpr qint64 maximumInstallerBytes = 512 * 1024 * 1024;
inline constexpr qint64 maximumMetadataBytes = 1024 * 1024;
inline constexpr qint64 maximumChecksumBytes = 4096;

struct ReleaseInfo {
    QString tag;
    QString version;
    QString notes;
    QUrl pageUrl;
    QString installerName;
    QUrl installerUrl;
    QUrl checksumUrl;
    qint64 installerBytes = 0;
    QByteArray apiDigest;
    bool newer = false;
};

QString platformKey();
QUrl latestReleaseUrl();
std::optional<ReleaseInfo> parseRelease(const QByteArray& json, const QString& currentVersion,
                                      const QString& platform, QString& error);
std::optional<QByteArray> parseChecksum(const QByteArray& text, const QString& installerName);
bool allowedAssetLocation(const QUrl& url, const QUrl& originalAsset);
QString readableReleaseNotes(QString markdown);
}
