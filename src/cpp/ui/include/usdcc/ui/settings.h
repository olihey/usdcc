#pragma once

#include <QSettings>

namespace usdcc::ui {

// The application's persistent settings: a plain INI file (usdcc.ini in the
// app config location) rather than QSettings' native format, which on Windows
// is the registry under HKCU\Software\usdcc\usdcc — a real file a developer
// can find, inspect or delete by hand, which matters while the saved panel
// layout and per-panel state still change shape often.
QSettings appSettings();

}  // namespace usdcc::ui
