#pragma once

#include <QFileSystemWatcher>
#include <QString>
#include <QTimer>

#include <filesystem>
#include <string>
#include <vector>

namespace usdcc::scripting {

class PythonRuntime;

// "Validation/folders": folders of rules files, separated by ";".
inline constexpr const char* kValidationFoldersKey = "Validation/folders";
void defineValidationSettings();

// Validation rules in Python files (docs/VALIDATION.md §5.2): loads the *.py in its folders with
// usdcc.validation.sync_files(), and again when one is added, saved or removed, in the app or
// outside it. Of two rules with one id the last loaded wins, with a warning. Main thread.
class ValidationRuleFiles {
public:
    ValidationRuleFiles(PythonRuntime& python, std::vector<std::filesystem::path> folders);

    // <user data>/validation.
    static std::filesystem::path userFolder();
    // The user's folder, those in the settings (defined by then, or none), then
    // USDCC_VALIDATION_PATH's.
    static std::vector<std::filesystem::path> defaultFolders();

    // Loads what's new or changed, and watches for more.
    void sync();

    // Writes a rule called `name` to start from into the user's folder (the first) and loads
    // it. Its file, or empty with `error` saying why.
    std::filesystem::path create(const QString& name, QString* error);

private:
    bool anyFiles() const;

    PythonRuntime& m_python;
    std::vector<std::filesystem::path> m_folders;
    QFileSystemWatcher m_watcher;
    QTimer m_timer;
    bool m_loaded = false;
};

}  // namespace usdcc::scripting
