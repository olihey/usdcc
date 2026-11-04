#pragma once

// Qt glue for usdcc::core::Settings (docs/SETTINGS.md): the backend that
// stores preferences in usdcc.ini, and helpers that hand a setting's value to
// Qt as a QColor/QKeySequence and keep Qt objects in step with it.

#include "usdcc/core/settings.h"

#include <QColor>
#include <QKeySequence>
#include <QSettings>
#include <QString>

#include <functional>
#include <string>

class QAction;
class QObject;

namespace usdcc::ui {

// Stores settings in an INI file — usdcc.ini for the app (appSettings()), so a
// preference sits next to the UI state that file already holds, under the
// group/key of its SettingDefinition::key.
class QSettingsBackend : public core::SettingsBackend {
public:
    explicit QSettingsBackend(const QString& fileName);
    std::optional<core::SettingValue> read(const core::SettingDefinition& definition) override;
    void write(const core::SettingDefinition& definition, const core::SettingValue& value) override;
    void remove(const std::string& key) override;

private:
    // Held open: QSettings shares one cache per file within the process, so
    // appSettings() users of the same file see these writes.
    QSettings m_settings;
};

// Makes Settings::instance() load from and save to `fileName` (usdcc.ini
// unless given). The app does this before defining any setting.
void installAppSettings(const QString& fileName = {});

// A Color setting as a QColor (invalid for an unknown key).
QColor settingColor(std::string_view key, const core::Settings& settings = core::Settings::instance());
// A Shortcut setting as a QKeySequence (empty for none or an unknown key).
QKeySequence settingShortcut(std::string_view key, const core::Settings& settings = core::Settings::instance());
// The text a Shortcut setting stores for `sequence` (its portable text).
std::string shortcutText(const QKeySequence& sequence);

// Calls `changed` whenever setting `key` changes, for as long as `owner`
// lives (not now; read the current value yourself). Returns the connection: a
// hidden child QObject of `owner`; deleting it disconnects.
QObject* onSettingChanged(QObject* owner, std::string key, std::function<void()> changed,
                      core::Settings& settings = core::Settings::instance());
// Calls `changed` with the key of every setting that changes, while `owner`
// lives. Returns the connection, as onSettingChanged() does.
QObject* onSettingsChanged(QObject* owner, std::function<void(const std::string& key)> changed,
                       core::Settings& settings = core::Settings::instance());

// Gives `action` Shortcut setting `key` now and again whenever it changes.
void bindShortcut(QAction* action, std::string key, core::Settings& settings = core::Settings::instance());

}  // namespace usdcc::ui
