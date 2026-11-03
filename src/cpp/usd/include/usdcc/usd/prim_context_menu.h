#pragma once

#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>

#include <QString>

#include <functional>
#include <vector>

class QMenu;
class QWidget;

namespace usdcc::usd {

class UndoManager;

// What the Outliner's prim menu acts on (docs/OUTLINER_CONTEXT_MENU.md).
struct PrimMenuContext {
    UndoManager* undo = nullptr;
    StageRefPtr stage;
    // The selection, which Active, Visibility, Load and Copy Path act on.
    std::vector<PXR_NS::SdfPath> paths;
    // The right-clicked prim, which the title, Variants and Prim Info are for.
    PXR_NS::SdfPath clicked;
    QWidget* dialogParent = nullptr;
    // Move Up (-1) / Move Down (+1); left out of the menu when not set.
    std::function<void(int direction)> move;
    // After Load or Unload, which can remove selected prims.
    std::function<void()> loadChanged;
};

// How Composition > an arc > Open as Stage opens a file (the app sets it: open
// it with StageOpener, or bring it forward if it's open already). Without one
// the entry is disabled.
void setOpenStageHandler(std::function<void(const QString& path)> handler);
// Opens `path` through that handler; false (and nothing done) without one.
bool openStageFile(const QString& path);

// Fills the menu: the prim's name and type, Active, Visibility, Load/Unload
// Payload, Variants, Composition (the clicked prim's arcs, read-only, with
// Open as Stage and Copy; compositionArcs()),
// Move Up/Down, Copy Path and Prim Info. Entries that don't apply to the
// prims are left out; on an instance proxy the edits are disabled, with a
// tooltip saying why. Each edit is one undo step (prim_actions.h).
void populatePrimMenu(QMenu* menu, const PrimMenuContext& context);

// A non-modal dialog with prim_actions::info(), and a Copy button.
QWidget* showPrimInfo(const PXR_NS::UsdPrim& prim, QWidget* parent);

}  // namespace usdcc::usd
