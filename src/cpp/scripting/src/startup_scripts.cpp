#include "usdcc/scripting/startup_scripts.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/python_runtime.h"

#include <QStandardPaths>
#include <QString>

#include <algorithm>
#include <vector>

namespace usdcc::scripting {

namespace {
std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

QString fileName(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.filename().u16string());
}
}  // namespace

std::filesystem::path userScriptsFolder() {
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/scripts";
    return std::filesystem::path(folder.toStdU16String());
}

StartupScriptsResult runStartupScripts(PythonRuntime& runtime, const std::filesystem::path& scriptsFolder,
                                       bool runStartup) {
    StartupScriptsResult result;
    if (!runtime.isAvailable()) return result;
    runtime.addToPath(scriptsFolder);

    const std::filesystem::path startupFolder = scriptsFolder / "startup";
    std::error_code error;
    std::filesystem::create_directories(startupFolder, error);
    if (!runStartup) {
        USDCC_LOG_INFO("usdcc.python", "Startup scripts skipped (--no-startup-scripts)");
        return result;
    }

    std::vector<std::filesystem::path> scripts;
    for (const auto& entry : std::filesystem::directory_iterator(startupFolder, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == ".py") scripts.push_back(entry.path());
    }
    std::sort(scripts.begin(), scripts.end(), [](const auto& a, const auto& b) {
        return fileName(a).compare(fileName(b), Qt::CaseInsensitive) < 0;
    });
    if (scripts.empty()) return result;

    USDCC_LOG_INFO("usdcc.python", "Running {} startup {} from {}", scripts.size(),
                   scripts.size() == 1 ? "script" : "scripts", toUtf8(startupFolder));
    for (const std::filesystem::path& script : scripts) {
        ++result.ran;
        if (!runtime.runFile(script)) ++result.failed;
    }
    if (result.ran == 1 && result.failed == 1) {
        USDCC_LOG_WARNING("usdcc.python", "The startup script stopped with an error");
    } else if (result.failed > 0) {
        USDCC_LOG_WARNING("usdcc.python", "{} of {} startup scripts stopped with an error", result.failed, result.ran);
    }
    return result;
}

}  // namespace usdcc::scripting
