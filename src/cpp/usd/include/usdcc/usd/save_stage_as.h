#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/stage.h"

#include <QString>

#include <string>

namespace usdcc::usd {

class StageManager;

// File > Save Stage As (docs/LAYER_EDITOR.md §5): writes `stage`'s root layer to the
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

}  // namespace usdcc::usd
