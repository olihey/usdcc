#pragma once

// The version check of a usdcc that can't update itself (docs/UPDATES.md §9): what
// GitHub's list of releases says, without Velopack and without the network, which
// AppUpdates does.

#include <QByteArray>
#include <QString>

#include <optional>

namespace usdcc::app {

// Orders two versions as Semantic Versioning 2 does, as Velopack: -1, 0 or 1. A leading
// "v" and a "+suffix" don't count; "26.12.1-beta.1" comes before "26.12.1". Nothing when
// one of them isn't a version (MAJOR.MINOR.PATCH).
std::optional<int> compareVersions(const QString& a, const QString& b);

// What this platform's release file is called after "usdcc-<version>-", such as
// "linux-x64.tar.xz"; empty where no release is made for it.
QString releaseFileSuffix();

struct FoundRelease {
    QString version;  // without a leading "v"
    QString tag;
};

// The newest release in `json` (GitHub's /repos/<repo>/releases) that has
// usdcc-<version>-<fileSuffix> among its files, and is newer than `current`. Releases
// without the file are passed over: a release's jobs can fail one by one, and the
// dependencies' releases aren't versions at all. `error` is set when `json` isn't a list.
std::optional<FoundRelease> newestRelease(const QByteArray& json, const QString& current, const QString& fileSuffix,
                                          bool prereleases, QString* error);

// What AppUpdates can do with the program at `executable`.
struct InstallLocation {
    enum class Kind {
        None,      // a build, or an unpacked 7z, tar.xz or copy without Velopack's updater
        Windows,   // the installer's
        MacApp,    // usdcc.app with Velopack's updater
        MacMoved,  // the same, where it can't be replaced: in the disk image, or translocated
        AppImage,
    };
    Kind kind = Kind::None;
    bool canUpdate() const { return kind == Kind::Windows || kind == Kind::MacApp || kind == Kind::AppImage; }
};

// `executable` is the program's path, `appImage` the $APPIMAGE it was started with.
InstallLocation installLocation(const QString& executable, const QString& appImage);

}  // namespace usdcc::app
