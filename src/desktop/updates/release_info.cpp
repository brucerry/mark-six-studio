#include "release_info.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <array>
#include <cmath>

namespace marksix::updates {
namespace {
const QString repository = QStringLiteral("https://github.com/brucerry/mark-six-studio");

std::optional<std::array<quint32, 3>> versionParts(const QString& version, bool tag) {
    const QRegularExpression expression((tag ? QStringLiteral("\\Av") : QStringLiteral("\\A")) +
        QStringLiteral("(0|[1-9][0-9]{0,9})\\.(0|[1-9][0-9]{0,9})\\.(0|[1-9][0-9]{0,9})\\z"));
    const auto match = expression.match(version);
    if (!match.hasMatch()) return std::nullopt;
    std::array<quint32, 3> parts{};
    for (int index = 0; index < 3; ++index) {
        bool okay = false;
        parts[size_t(index)] = match.captured(index + 1).toUInt(&okay);
        if (!okay) return std::nullopt;
    }
    return parts;
}

bool validSize(const QJsonValue& value, qint64 maximum) {
    if (!value.isDouble()) return false;
    const double size = value.toDouble();
    return std::isfinite(size) && size > 0 && size <= double(maximum) && std::floor(size) == size;
}
}

QString platformKey() {
#ifdef Q_OS_WIN
    return QStringLiteral("windows-x64");
#else
    return QStringLiteral("macos-universal");
#endif
}

QUrl latestReleaseUrl() {
    return QUrl(QStringLiteral("https://api.github.com/repos/brucerry/mark-six-studio/releases/latest"));
}

std::optional<ReleaseInfo> parseRelease(const QByteArray& json, const QString& currentVersion,
                                      const QString& platform, QString& error) {
    error = QStringLiteral("The published release information is incomplete or unsupported. Try again later.");
    if (json.size() > maximumMetadataBytes) return std::nullopt;
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isObject()) return std::nullopt;
    const auto object = document.object();
    if (!object.value("draft").isBool() || object.value("draft").toBool() ||
        !object.value("prerelease").isBool() || object.value("prerelease").toBool()) return std::nullopt;
    ReleaseInfo release;
    release.tag = object.value("tag_name").toString();
    const auto latest = versionParts(release.tag, true);
    const auto current = versionParts(currentVersion, false);
    if (!latest || !current) return std::nullopt;
    release.version = release.tag.mid(1);
    const QString page = repository + QStringLiteral("/releases/tag/") + release.tag;
    if (object.value("html_url").toString() != page) return std::nullopt;
    release.pageUrl = QUrl(page);
    release.notes = object.value("body").toString();
    if (release.notes.size() > 256 * 1024) return std::nullopt;
    release.newer = *latest > *current;
    if (!release.newer) { error.clear(); return release; }
    QString checksumName;
    const QString stem = QStringLiteral("MarkSixStudio-") + release.version;
    if (platform == QStringLiteral("windows-x64")) {
        release.installerName = stem + QStringLiteral("-windows-x64-unsigned-setup.exe");
        checksumName = release.installerName + QStringLiteral(".sha256");
    } else if (platform == QStringLiteral("macos-universal")) {
        const QString mac = stem + QStringLiteral("-macos-universal-unsigned");
        release.installerName = mac + QStringLiteral(".pkg");
        checksumName = mac + QStringLiteral(".sha256");
    } else return std::nullopt;
    const QString assetBase = repository + QStringLiteral("/releases/download/") + release.tag + '/';
    release.installerUrl = QUrl(assetBase + release.installerName);
    release.checksumUrl = QUrl(assetBase + checksumName);
    bool installerFound = false, checksumFound = false;
    if (!object.value("assets").isArray()) return std::nullopt;
    for (const auto& value : object.value("assets").toArray()) {
        const auto asset = value.toObject();
        const auto name = asset.value("name").toString();
        if (name != release.installerName && name != checksumName) continue;
        if (asset.value("state").toString() != QStringLiteral("uploaded") ||
            asset.value("browser_download_url").toString() != assetBase + name) return std::nullopt;
        if (name == release.installerName) {
            if (installerFound || !validSize(asset.value("size"), maximumInstallerBytes)) return std::nullopt;
            installerFound = true;
            release.installerBytes = qint64(asset.value("size").toDouble());
            const auto digest = asset.value("digest");
            if (!digest.isUndefined() && !digest.isNull()) {
                const auto match = QRegularExpression(QStringLiteral("\\Asha256:([0-9a-fA-F]{64})\\z"))
                    .match(digest.toString());
                if (!match.hasMatch()) return std::nullopt;
                release.apiDigest = QByteArray::fromHex(match.captured(1).toLatin1());
            }
        } else {
            if (checksumFound || !validSize(asset.value("size"), maximumChecksumBytes)) return std::nullopt;
            checksumFound = true;
        }
    }
    if (!installerFound || !checksumFound) return std::nullopt;
    error.clear();
    return release;
}

std::optional<QByteArray> parseChecksum(const QByteArray& text, const QString& installerName) {
    if (text.size() > maximumChecksumBytes) return std::nullopt;
    const auto match = QRegularExpression(QStringLiteral("\\A([0-9a-fA-F]{64})[ \\t]+\\*?") +
        QRegularExpression::escape(installerName) + QStringLiteral("(?:\\r?\\n)?\\z")).match(QString::fromUtf8(text));
    if (!match.hasMatch()) return std::nullopt;
    return QByteArray::fromHex(match.captured(1).toLatin1());
}

bool allowedAssetLocation(const QUrl& url, const QUrl& originalAsset) {
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || !url.userInfo().isEmpty() ||
        url.port(-1) != -1 || url.hasFragment()) return false;
    if (url.host() == QStringLiteral("github.com")) return url == originalAsset;
    return (url.host() == QStringLiteral("release-assets.githubusercontent.com") ||
            url.host() == QStringLiteral("objects.githubusercontent.com")) && url.path().startsWith('/');
}

QString readableReleaseNotes(QString markdown) {
    // Keep note text readable without loading HTML, remote images or link resources.
    markdown.replace(QRegularExpression(QStringLiteral("!?\\[([^\\]\\n]*)\\]\\([^\\n)]*\\)")), QStringLiteral("\\1"));
    markdown.remove(QRegularExpression(QStringLiteral("(?m)^#{1,6}[ \\t]+")));
    markdown.remove(QStringLiteral("**"));
    markdown.remove(QStringLiteral("__"));
    markdown.remove('`');
    return markdown;
}
}
