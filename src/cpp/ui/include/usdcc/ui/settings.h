#pragma once

#include <QSettings>
#include <QString>

namespace usdcc::ui {

// The application's persistent settings: a plain INI file (usdcc.ini in
// userConfigFolder()) rather than QSettings' native format, which on Windows
// is the registry under HKCU\Software\usdcc\usdcc — a real file a developer
// can find, inspect or delete by hand, which matters while the saved panel
// layout and per-panel state still change shape often.
QSettings appSettings();

// Where usdcc keeps the user's own files: usdcc.ini in the config folder; logs,
// sessions, workspaces, plugins, scripts, python-env and the rest in the data folder.
// Qt's AppConfigLocation and AppLocalDataLocation, so Qt's test mode and the
// application name (USDCC_TEST_PROFILE) move them, except on Windows, where both are
// %LOCALAPPDATA%\usdcc_config: the installer puts the app in %LOCALAPPDATA%\usdcc and
// empties that folder when usdcc is uninstalled (docs/UPDATES.md §2).
// On Linux: ~/.config/usdcc and ~/.local/share/usdcc.
QString userConfigFolder();
QString userDataFolder();

// Where this usdcc's own parts are (usdcc/core/install_paths.h), from the running
// program's folder: installFolder() holds plugins/, resources/ and tools/, and
// programDataFolder() what a program has beside it. Both are Contents/Resources in a
// macOS bundle.
QString installFolder();
QString programDataFolder();

}  // namespace usdcc::ui
