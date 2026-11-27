#pragma once

// Prims as USDA text, for Send to… (docs/PEERS.md §10.3): the selected prims, as the
// stage composes them, written out; and text like that pasted into another stage.

#include "usdcc/core/export.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>

#include <QString>
#include <QStringList>

#include <vector>

namespace usdcc::usd {

class UndoManager;

namespace share {

// The prims at `paths`, with everything under them, flattened as the stage composes them,
// as one USDA layer. A prim under another of `paths` comes with that one. Each sits at its
// own path, under `over`s for its parents. Empty, with why in `error`, for no valid path.
USDCC_USD_UI_API QString primsAsUsda(const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths,
                                     QString* error = nullptr);

// Copies the prims `usda` defines at its top into `stage`'s edit target, each at its own
// path (parents made as `over`s when missing), as one undo step. A prim the stage has at a
// path already gets the next free name ("Hero_1"). Returns the paths pasted to; empty, with
// why in `error`, when nothing could be.
USDCC_USD_UI_API std::vector<PXR_NS::SdfPath> pasteUsda(const StageRefPtr& stage, UndoManager& undo,
                                                       const QString& usda, QString* error = nullptr);

}  // namespace share
}  // namespace usdcc::usd
