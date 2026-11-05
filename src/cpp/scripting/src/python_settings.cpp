#include "usdcc/scripting/python_settings.h"

#include "usdcc/core/settings.h"
#include "usdcc/scripting/python_runtime.h"

#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace usdcc::scripting {

namespace {
using usdcc::core::Settings;
using usdcc::core::SettingType;

constexpr auto kCategory = "Python";

QString defaultPackagesFolder() {
    return QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/python-env");
}
}  // namespace

void definePythonSettings() {
    Settings& settings = Settings::instance();
    settings.define({.key = kPythonUserSiteKey,
                     .category = kCategory,
                     .group = "Interpreter",
                     .label = "Use my Python user packages",
                     .description = "Also import from your own Python's user site-packages folder. A package there comes before "
                                    "the shared packages folder, and a plugin that relies on one won't work on other machines.",
                     .type = SettingType::Bool,
                     .defaultValue = false,
                     .appliesAtNextStart = true});
    settings.define({.key = kPackagesFolderKey,
                     .category = kCategory,
                     .group = "Packages",
                     .label = "Shared packages folder",
                     .description = "Where the packages plugins and your scripts need are installed. Changing it moves nothing: "
                                    "the new folder starts empty, and the old one stays until you delete it.",
                     .type = SettingType::Folder,
                     .defaultValue = defaultPackagesFolder().toStdString(),
                     .appliesAtNextStart = true});
    settings.define({.key = kExtraPackageFoldersKey,
                     .category = kCategory,
                     .group = "Packages",
                     .label = "Additional package folders",
                     .description = "Ready-made folders of packages, separated by ';', such as a studio's. They come before the "
                                    "shared folder, and what they provide isn't installed again.",
                     .type = SettingType::String,
                     .defaultValue = std::string(),
                     .appliesAtNextStart = true});
    settings.define({.key = kUserPackagesKey,
                     .category = kCategory,
                     .group = "Packages",
                     .label = "My packages",
                     .description = "Packages for your own scripts, separated by spaces: numpy>=2 pyyaml. Install them from "
                                    "View > Plugins.",
                     .type = SettingType::String,
                     .defaultValue = std::string()});
    settings.define({.key = kPackageIndexKey,
                     .category = kCategory,
                     .group = "Sources",
                     .label = "Package index",
                     .description = "The address of a package index to use instead of PyPI, such as a studio's.",
                     .type = SettingType::String,
                     .defaultValue = std::string()});
    settings.define({.key = kWheelsFolderKey,
                     .category = kCategory,
                     .group = "Sources",
                     .label = "Wheels folder",
                     .description = "A folder of downloaded .whl files to install from as well.",
                     .type = SettingType::Folder,
                     .defaultValue = std::string()});
    settings.define({.key = kPackagesOfflineKey,
                     .category = kCategory,
                     .group = "Sources",
                     .label = "Don't use the network",
                     .description = "Install only from the wheels folder and from what has been downloaded before.",
                     .type = SettingType::Bool,
                     .defaultValue = false});
}

std::filesystem::path packagesFolderSetting() {
    const std::string value = Settings::instance().get<std::string>(kPackagesFolderKey);
    const QString folder = value.empty() ? defaultPackagesFolder() : QString::fromStdString(value);
    return std::filesystem::path(folder.toStdU16String());
}

std::vector<std::filesystem::path> extraPackageFoldersSetting() {
    std::vector<std::filesystem::path> folders;
    const QString value = QString::fromStdString(Settings::instance().get<std::string>(kExtraPackageFoldersKey));
    for (const QString& folder : value.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString trimmed = folder.trimmed();
        if (!trimmed.isEmpty()) folders.emplace_back(trimmed.toStdU16String());
    }
    return folders;
}

}  // namespace usdcc::scripting
