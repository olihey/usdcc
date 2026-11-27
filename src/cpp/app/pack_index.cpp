#include "pack_index.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace usdcc::app {

namespace {
// The newest index this usdcc reads; a later one says something it wouldn't understand.
constexpr int kSchema = 1;

bool isPackName(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    return pattern.match(name).hasMatch();
}

// One file's name, beside the index: no folder, no drive, nothing hidden.
bool isFileName(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+-]*$"));
    return pattern.match(name).hasMatch() && !name.contains(QStringLiteral(".."));
}

bool isSha256(const QByteArray& text) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(QString::fromLatin1(text)).hasMatch();
}
}  // namespace

QString packIndexFileName(const QString& version, const QString& platform) {
    return QStringLiteral("usdcc-%1-%2-packs.json").arg(version, platform);
}

QUrl packsBaseUrl(const QString& version, const QString& repositoryUrl, const QString& custom) {
    if (!custom.isEmpty()) {
        // A folder, unless it says otherwise ("https://...", "file:///...").
        const QUrl given(custom);
        const bool isUrl = given.isValid() && given.scheme().size() > 1 && !QFileInfo(custom).isAbsolute();
        QUrl url = isUrl ? given : QUrl::fromLocalFile(QDir(custom).absolutePath());
        if (!url.path().endsWith(QLatin1Char('/'))) url.setPath(url.path() + QLatin1Char('/'));
        return url;
    }
    // The tag is the version (docs/RELEASE.md §1).
    return QUrl(repositoryUrl + QStringLiteral("/releases/download/") + QString::fromLatin1(QUrl::toPercentEncoding(version)) +
                QLatin1Char('/'));
}

std::optional<std::vector<PackIndexEntry>> parsePackIndex(const QByteArray& json, const QString& version, const QString& platform,
                                                          QString* error) {
    const auto fail = [error](const QString& why) -> std::optional<std::vector<PackIndexEntry>> {
        if (error) *error = why;
        return std::nullopt;
    };
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject() || !document.object().value(QStringLiteral("packs")).isArray()) {
        return fail(QStringLiteral("The answer isn't a list of packs."));
    }
    const QJsonObject index = document.object();
    if (index.value(QStringLiteral("schema")).toInt() > kSchema) {
        return fail(QStringLiteral("The list of packs is made for a newer usdcc."));
    }
    const QString forVersion = index.value(QStringLiteral("usdcc")).toString();
    const QString forPlatform = index.value(QStringLiteral("platform")).toString();
    if (forVersion != version || forPlatform != platform) {
        return fail(QStringLiteral("The list of packs is for usdcc %1 on %2, not %3 on %4.").arg(forVersion, forPlatform, version, platform));
    }

    std::vector<PackIndexEntry> packs;
    for (const QJsonValue& value : index.value(QStringLiteral("packs")).toArray()) {
        const QJsonObject item = value.toObject();
        PackIndexEntry entry;
        entry.name = item.value(QStringLiteral("name")).toString();
        entry.title = item.value(QStringLiteral("title")).toString();
        entry.description = item.value(QStringLiteral("description")).toString();
        entry.version = item.value(QStringLiteral("version")).toString();
        entry.license = item.value(QStringLiteral("license")).toString();
        entry.homepage = item.value(QStringLiteral("homepage")).toString();
        for (const QJsonValue& renderer : item.value(QStringLiteral("renderers")).toArray()) entry.renderers << renderer.toString();
        entry.file = item.value(QStringLiteral("file")).toString();
        entry.size = static_cast<qint64>(item.value(QStringLiteral("size")).toDouble());
        entry.sha256 = item.value(QStringLiteral("sha256")).toString().toLatin1().toLower();
        entry.installedSize = static_cast<qint64>(item.value(QStringLiteral("installed_size")).toDouble());
        entry.content = item.value(QStringLiteral("content")).toString();
        if (!isPackName(entry.name) || !isFileName(entry.file) || !isSha256(entry.sha256) || entry.size <= 0) continue;
        if (entry.title.isEmpty()) entry.title = entry.name;
        // A link opens in the browser, so only what a browser should open.
        if (!entry.homepage.startsWith(QLatin1String("https://"))) entry.homepage.clear();
        const bool twice = std::any_of(packs.begin(), packs.end(), [&entry](const PackIndexEntry& other) { return other.name == entry.name; });
        if (!twice) packs.push_back(std::move(entry));
    }
    return packs;
}

QString packSizeText(qint64 bytes) {
    const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (megabytes >= 10.0) return QStringLiteral("%1 MB").arg(QString::number(megabytes, 'f', 0));
    if (megabytes >= 1.0) return QStringLiteral("%1 MB").arg(QString::number(megabytes, 'f', 1));
    return QStringLiteral("%1 KB").arg(QString::number(std::max<qint64>(1, bytes / 1024)));
}

}  // namespace usdcc::app
