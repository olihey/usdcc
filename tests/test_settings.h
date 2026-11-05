#pragma once

// Keeps a test away from the user's settings, for as long as it lives:
// - Qt's test mode moves the app config folder, so the usdcc.ini that
//   appSettings() (window layout, panel state) opens is a throwaway one;
// - the preferences registry (usdcc::core::Settings::instance(),
//   docs/SETTINGS.md) stores in memory only, starting from the defaults.
// Both are cleared again at the end.

#include "usdcc/core/settings.h"

#include <QFile>
#include <QStandardPaths>
#include <QString>

#include <memory>

struct TestSettings {
    TestSettings() {
        QStandardPaths::setTestModeEnabled(true);
        remove();
        usdcc::core::Settings::instance().setBackend(std::make_unique<usdcc::core::MemorySettingsBackend>());
    }
    ~TestSettings() {
        usdcc::core::Settings::instance().setBackend(std::make_unique<usdcc::core::MemorySettingsBackend>());
        remove();
        QStandardPaths::setTestModeEnabled(false);
    }
    TestSettings(const TestSettings&) = delete;
    TestSettings& operator=(const TestSettings&) = delete;

    static QString path() { return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/usdcc.ini"; }
    static void remove() { QFile::remove(path()); }
};
