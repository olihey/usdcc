#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/stage.h"

#include <pxr/usd/usd/stage.h>

#include <QJsonObject>
#include <QStringList>

#include <string>
#include <vector>

namespace usdcc::usd {

class StageManager;

// A stage as a session keeps it (docs/SESSIONS.md §2.1): its file, the selection,
// the edit target and variant edit, muted layers, which payloads are loaded, and the
// session layer's text. Never its unsaved edits.
namespace session_stages {

// Whether it has a file to reopen. One that doesn't (File > Create Stage > In Memory)
// is kept as its text instead, under "inMemory", and made again from it.
USDCC_USD_UI_API bool hasFile(const StageRefPtr& stage);
// The stage's state, under the keys of §3.2's "stages" entries.
USDCC_USD_UI_API QJsonObject capture(const StageManager& stageManager, const StageRefPtr& stage);

// The slow part of reopening, safe on any thread: opens `state`'s file without loading
// any payload, puts its session layer and muted layers back, then loads what its load
// rules say. So a payload that was unloaded is never loaded, even for a moment. Null if
// it can't be opened; `errors` gets USD's messages.
USDCC_USD_UI_API PXR_NS::UsdStageRefPtr open(const QJsonObject& state, std::vector<std::string>* errors = nullptr);

// On the stage manager's thread, once the stage is adopted: the edit target, variant
// edit and selection. What no longer fits the files (a prim or layer that's gone) is
// left out, and said in `warnings` (§5).
USDCC_USD_UI_API void apply(StageManager& stageManager, const StageRefPtr& stage, const QJsonObject& state,
                            QStringList* warnings = nullptr);

}  // namespace session_stages
}  // namespace usdcc::usd
