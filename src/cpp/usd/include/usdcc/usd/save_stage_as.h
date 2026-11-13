#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/stage.h"

#include <QString>

#include <string>

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
// with unsaved changes (layer_actions::saveAll()). A layer that can't be saved says why in
// a message box over `parent`. Returns whether all were saved.
USDCC_USD_UI_API bool saveStageInteractively(const StageRefPtr& stage, QWidget* parent);
// Whether Save Stage has anything to do: `stage` has a layer with unsaved changes.
USDCC_USD_UI_API bool stageHasUnsavedLayers(const StageRefPtr& stage);
// Stages > a stage > Save Stage As...: asks for a file and saveStageAsAndSwitch()es to it,
// which then goes in File > Open Recent. Returns the stage now in use (null if cancelled or
// it failed, which a message box says).
USDCC_USD_UI_API StageRefPtr saveStageAsInteractively(StageManager& stageManager, const StageRefPtr& stage,
                                                      QWidget* parent);

}  // namespace usdcc::usd
