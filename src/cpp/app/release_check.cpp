#include "release_check.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QStringList>
#include <QSysInfo>

#include <array>

namespace usdcc::app {

namespace {

struct Version {
    std::array<qulonglong, 3> numbers{};
    QStringList prerelease;
};

std::optional<Version> parseVersion(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral("^v?(\\d+)\\.(\\d+)\\.(\\d+)(?:-([0-9A-Za-z.-]+))?(?:\\+[0-9A-Za-z.-]+)?$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) return std::nullopt;
    Version version;
    for (int i = 0; i < 3; ++i) version.numbers[i] = match.captured(i + 1).toULongLong();
    if (!match.captured(4).isEmpty()) version.prerelease = match.captured(4).split(QLatin1Char('.'));
    return version;
}

int compareIdentifiers(const QString& a, const QString& b) {
    bool aNumber = false;
    bool bNumber = false;
    const qulonglong aValue = a.toULongLong(&aNumber);
    const qulonglong bValue = b.toULongLong(&bNumber);
    if (aNumber && bNumber) return aValue < bValue ? -1 : (aValue > bValue ? 1 : 0);
    if (aNumber != bNumber) return aNumber ? -1 : 1;  // numbers come first
    const int order = a.compare(b);
    return order < 0 ? -1 : (order > 0 ? 1 : 0);
}

}  // namespace

std::optional<int> compareVersions(const QString& a, const QString& b) {
    const std::optional<Version> left = parseVersion(a);
    const std::optional<Version> right = parseVersion(b);
    if (!left || !right) return std::nullopt;
    if (left->numbers != right->numbers) return left->numbers < right->numbers ? -1 : 1;
    if (left->prerelease.isEmpty() || right->prerelease.isEmpty()) {
        if (left->prerelease.isEmpty() == right->prerelease.isEmpty()) return 0;
        return left->prerelease.isEmpty() ? 1 : -1;
    }
    for (qsizetype i = 0; i < qMin(left->prerelease.size(), right->prerelease.size()); ++i) {
        if (const int order = compareIdentifiers(left->prerelease[i], right->prerelease[i])) return order;
    }
    if (left->prerelease.size() == right->prerelease.size()) return 0;
    return left->prerelease.size() < right->prerelease.size() ? -1 : 1;
}

QString releaseFileSuffix() {
    const QString cpu = QSysInfo::buildCpuArchitecture();
    const QString arch = cpu == QLatin1String("x86_64") ? QStringLiteral("x64") : cpu;
    if (arch != QLatin1String("x64") && arch != QLatin1String("arm64")) return {};
#if defined(Q_OS_WIN)
    return QStringLiteral("windows-%1.7z").arg(arch);
#elif defined(Q_OS_MACOS)
    return arch == QLatin1String("arm64") ? QStringLiteral("macos-arm64.dmg") : QString();
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux-%1.tar.xz").arg(arch);
#else
    return {};
#endif
}

std::optional<FoundRelease> newestRelease(const QByteArray& json, const QString& current, const QString& fileSuffix,
                                          bool prereleases, QString* error) {
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isArray()) {
        if (error) *error = QStringLiteral("The answer isn't a list of releases.");
        return std::nullopt;
    }
    std::optional<FoundRelease> newest;
    for (const QJsonValue& each : document.array()) {
        const QJsonObject release = each.toObject();
        if (release.value(QLatin1String("draft")).toBool()) continue;
        if (release.value(QLatin1String("prerelease")).toBool() && !prereleases) continue;
        const QString tag = release.value(QLatin1String("tag_name")).toString();
        const QString version = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
        if (compareVersions(version, newest ? newest->version : current).value_or(-1) <= 0) continue;
        const QString file = QStringLiteral("usdcc-%1-%2").arg(version, fileSuffix);
        for (const QJsonValue& asset : release.value(QLatin1String("assets")).toArray()) {
            // "uploaded" once it's complete; a release's files arrive one by one.
            const QString state = asset.toObject().value(QLatin1String("state")).toString(QStringLiteral("uploaded"));
            if (asset.toObject().value(QLatin1String("name")).toString() != file || state != QLatin1String("uploaded")) continue;
            newest = FoundRelease{version, tag};
            break;
        }
    }
    return newest;
}

InstallLocation installLocation(const QString& executable, const QString& appImage) {
    using Kind = InstallLocation::Kind;
#if defined(Q_OS_WIN)
    Q_UNUSED(appImage);
    // As Velopack's own locator: Update.exe beside the "current" folder.
    const QString path = QString(executable).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const qsizetype at = path.lastIndexOf(QLatin1String("/current/"));
    if (at > 0 && QFileInfo::exists(path.left(at) + QLatin1String("/Update.exe"))) return {Kind::Windows};
#elif defined(Q_OS_MACOS)
    Q_UNUSED(appImage);
    const QFileInfo info(executable);
    if (!executable.contains(QLatin1String(".app/")) || !QFileInfo::exists(info.path() + QLatin1String("/UpdateMac"))) {
        return {};
    }
    // macOS runs a downloaded app that wasn't moved from a read-only place of its own.
    if (executable.contains(QLatin1String("/AppTranslocation/")) || QStorageInfo(info.path()).isReadOnly()) {
        return {Kind::MacMoved};
    }
    return {Kind::MacApp};
#else
    // $APPIMAGE reaches the programs usdcc starts too, so this program has to be in one.
    const qsizetype at = executable.indexOf(QLatin1String("/usr/bin/"));
    if (at >= 0 && !appImage.isEmpty() && QFileInfo::exists(appImage)
        && QFileInfo::exists(executable.left(at) + QLatin1String("/usr/bin/UpdateNix"))) {
        return {Kind::AppImage};
    }
#endif
    return {};
}

}  // namespace usdcc::app
