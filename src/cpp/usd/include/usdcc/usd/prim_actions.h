#pragma once

#include "usdcc/usd/composition_arcs.h"
#include "usdcc/usd/stage.h"

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>

#include <QString>

#include <utility>
#include <vector>

namespace usdcc::usd {

class UndoManager;

// The Outliner's prim edits (docs/OUTLINER_CONTEXT_MENU.md §3), each one undo
// step on `undo` for all of `paths`, named for what it did ("Deactivate 3
// Prims"). They return whether they changed anything. Instance proxies are
// skipped: they can't be edited (isEditable()).
namespace prim_actions {

bool isEditable(const PXR_NS::UsdPrim& prim);

bool setActive(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths,
               bool active);
// UsdGeomImageable::MakeVisible()/MakeInvisible(): making a prim visible also
// authors on its ancestors and their other children when an ancestor is
// invisible, so it really shows.
bool setVisible(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths,
                bool visible);
// Makes `paths` visible and every other imageable prim beside them, or
// beside one of their ancestors, invisible.
bool showOnly(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths);
// Clears the visibility authored in the edit target: back to inherited.
bool resetVisibility(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths);

bool setVariant(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                const std::string& variantSet, const std::string& variant);
bool clearVariant(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                  const std::string& variantSet);

// With UsdNamespaceEditor. False for an invalid or unchanged name.
bool rename(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& name);

// Payloads: whether anything at or under `path` is loadable, and whether some
// of it is unloaded / loaded.
bool hasPayloads(const StageRefPtr& stage, const PXR_NS::SdfPath& path);
bool canLoad(const StageRefPtr& stage, const PXR_NS::SdfPath& path);
bool canUnload(const StageRefPtr& stage, const PXR_NS::SdfPath& path);
// Load with descendants, or unload. Not a layer edit: the step pushed on the
// stage's undo stack puts the stage's load rules back.
bool load(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths);
bool unload(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths);

// Composition arcs (docs/COMPOSITION_ARCS.md §4), in the stage's edit target.
// Add puts a reference or payload (`assetPath`, `target` empty = the default
// prim; an empty `assetPath` with a `target` is an internal one), an inherit
// or a specialize (`target`) at the front of the prepend list, as USD does.
// Change and remove act on the entry `arc` describes (one of
// compositionArcs()' editable ones): change keeps its place in the list op
// and anything else it carries (custom data); remove takes it out of the
// edit target's lists. Variants have setVariant().
bool addArc(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, ArcInfo::Type type,
            const std::string& assetPath, const PXR_NS::SdfPath& target);
bool changeArc(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const ArcInfo& arc,
               const ArcInfo& changed);
bool removeArc(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const ArcInfo& arc);

// Prim Info (§4): label and value rows, and the same as text to copy.
std::vector<std::pair<QString, QString>> info(const PXR_NS::UsdPrim& prim);
QString infoText(const PXR_NS::UsdPrim& prim);

}  // namespace prim_actions
}  // namespace usdcc::usd
