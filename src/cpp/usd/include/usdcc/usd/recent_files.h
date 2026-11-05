#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <QString>
#include <QStringList>

#include <functional>

class QMenu;
class QObject;

namespace usdcc::usd {

class StageManager;

// The files last opened as stages, for File > Open Recent. How many is a
// preference (docs/SETTINGS.md); the list itself is UI state, in usdcc.ini.

// The setting saying how many to keep.
inline constexpr const char* kRecentFilesCountKey = "Files/recentFilesCount";

// Defines that setting. The list shrinks at once when it's lowered, for as long as
// `owner` lives.
void defineRecentFilesSetting(QObject* owner);

// Newest first, at most the setting's number.
QStringList recentFiles();
// Puts `path` first, once however it's spelled. Does nothing if the setting is 0.
void addRecentFile(const QString& path);
void removeRecentFile(const QString& path);
void clearRecentFiles();

// Fills `menu`, emptied first: a numbered entry per file with its whole path, newest
// first, calling `open` with it, then Clear Recent. A file that's no longer there is
// greyed out and says so.
void populateRecentFilesMenu(QMenu* menu, const std::function<void(const QString& path)>& open);

// Adds every file `stages` opens from now on, whichever way it's opened. In-memory
// stages have no file to add. Keep the connection for as long as it should.
usdcc::core::Signal<StageRefPtr>::Connection rememberOpenedStages(StageManager& stages);

}  // namespace usdcc::usd
