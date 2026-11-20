#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/stage.h"

#include <QDialog>
#include <QKeySequence>
#include <QString>

#include <string>

class QAction;
class QComboBox;
class QWidget;

namespace usdcc::usd {

class StageManager;

// Save Stage As (docs/LAYER_EDITOR.md §5): writes `stage`'s root layer to the
// file `path` (layer_actions::saveStageAs()) and goes on in that file, which opens as a
// stage in `stage`'s place; `stage` closes. Along come:
//   - what a session keeps of a stage (session_stages): the selection, the edit target
//     and variant edit, muted layers, which payloads are loaded, the session layer;
//   - every panel showing it, with its own state (the camera, expanded rows, ...:
//     ViewPanel::saveSessionState());
//   - being the current stage.
// Its sublayers' unsaved edits stay, in the same layers. The undo history doesn't come
// along. `path` being its own file is a plain save, and `stage` stays.
// Returns the stage now in use, or null with `error` set; `stage` stays then.
USDCC_USD_UI_API StageRefPtr saveStageAsAndSwitch(StageManager& stageManager, const StageRefPtr& stage,
                                                  const std::string& path, QString* error);
// Whether saveStageAsAndSwitch() is under way: the stage it opens is the old one's
// replacement, not a stage opened by the user (main.cpp gives it no viewport of its own).
USDCC_USD_UI_API bool isSwitchingStage();

// Stages > a stage > Save Stage, and a panel tab's Save Stage: every file layer of `stage`
// with unsaved changes (layer_actions::saveAll()). A toast says how it went: saved (a
// success toast), nothing to save (info), or why a layer couldn't be saved (an error that
// stays until dismissed). Returns whether all were saved.
USDCC_USD_UI_API bool saveStageInteractively(const StageRefPtr& stage, QWidget* parent);
// Whether Save Stage has anything to do: `stage` has a layer with unsaved changes.
USDCC_USD_UI_API bool stageHasUnsavedLayers(const StageRefPtr& stage);
// Stages > a stage > Save Stage As...: asks for a file and saveStageAsAndSwitch()es to it,
// which then goes in File > Open Recent. Returns the stage now in use (null if cancelled or
// it failed, which a message box says).
USDCC_USD_UI_API StageRefPtr saveStageAsInteractively(StageManager& stageManager, const StageRefPtr& stage,
                                                      QWidget* parent);

// The Save Stage key (docs/LAYER_EDITOR.md §5): a Shortcut setting, Ctrl+S by default.
USDCC_USD_UI_API extern const char* const kSaveStageShortcutKey;
// Defines that setting and gives `action` its key, application-wide, so it works in
// floating panels too. Except while the focus widget, or a widget it sits in, has its own
// shortcut for that key (the Layers panel's and the Script Editor's Ctrl+S): `action` then
// has no key, so the two never make it ambiguous and the panel's wins.
USDCC_USD_UI_API void bindSaveStageShortcut(QAction* action);
// Whether `focus` or a widget it sits in (up to its window) has an enabled QShortcut or
// QAction other than `ignore` that `key` would set off there.
USDCC_USD_UI_API bool focusHasOwnShortcut(const QWidget* focus, const QKeySequence& key,
                                          const QAction* ignore = nullptr);
// What the key does: with one stage open, saveStageInteractively() on it; with more, asks
// which (SaveStageDialog) and saves that one. Returns whether a stage was saved (false with
// none open, or on Cancel).
USDCC_USD_UI_API bool saveStageFromShortcut(StageManager& stageManager, QWidget* parent);

// The question saveStageFromShortcut() asks: a dropdown of every open stage (their short
// names, see shortStageNames()), the current stage picked to start with.
class USDCC_USD_UI_API SaveStageDialog : public QDialog {
    Q_OBJECT

public:
    explicit SaveStageDialog(StageManager* stageManager, QWidget* parent = nullptr);
    // The stage picked; null if it has closed meanwhile.
    StageRefPtr stage() const;

private:
    StageManager* m_stageManager;
    QComboBox* m_choice;
};

}  // namespace usdcc::usd
