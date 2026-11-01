#include "usdcc/ui/settings.h"

#include <QDir>
#include <QStandardPaths>

namespace usdcc::ui {

QSettings appSettings() {
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);
    return QSettings(configDir + "/usdcc.ini", QSettings::IniFormat);
}

}  // namespace usdcc::ui
