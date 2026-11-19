#include "usdcc/ui/settings.h"

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

}  // namespace usdcc::ui
