#include "usdcc/ui/settings.h"

#include "usdcc/core/install_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace usdcc::ui {

namespace {

QString userFolder(QStandardPaths::StandardLocation location) {
    QString folder = QStandardPaths::writableLocation(location);
#ifdef _WIN32
    // Both locations are %LOCALAPPDATA%\<application name> on Windows.
    folder += QStringLiteral("_config");
#endif
    return folder;
}

}  // namespace

QSettings appSettings() {
    const QString configDir = userConfigFolder();
    QDir().mkpath(configDir);
    return QSettings(configDir + "/usdcc.ini", QSettings::IniFormat);
}

QString userConfigFolder() { return userFolder(QStandardPaths::AppConfigLocation); }

QString userDataFolder() { return userFolder(QStandardPaths::AppLocalDataLocation); }

namespace {

std::filesystem::path programFolder() {
    return std::filesystem::path(QCoreApplication::applicationDirPath().toStdU16String());
}

QString toQString(const std::filesystem::path& path) { return QDir::cleanPath(QString::fromStdU16String(path.u16string())); }

}  // namespace

QString installFolder() { return toQString(usdcc::core::installRoot(programFolder())); }

QString programDataFolder() { return toQString(usdcc::core::programDataDir(programFolder())); }

}  // namespace usdcc::ui
