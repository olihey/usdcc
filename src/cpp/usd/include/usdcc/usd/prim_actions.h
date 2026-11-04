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

// False for an instance proxy, and for a prim a variant edit target doesn't reach:
// USD would write its edits to the plain layer, silently (docs/VARIANT_EDITOR.md §2).
bool isEditable(const PXR_NS::UsdPrim& prim);
// UsdNamespaceEditor refuses an edit target that maps paths, so rename doesn't work
// inside a variant.
bool canRename(const PXR_NS::UsdPrim& prim);

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

// Under a variant edit target a selection goes in the plain layer: inside the variant
// it couldn't change what composes. For the set being edited, it switches to editing
// that variant instead, and clearing is refused.
bool setVariant(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                const std::string& variantSet, const std::string& variant);
bool clearVariant(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                  const std::string& variantSet);

// With UsdNamespaceEditor. False for an invalid or unchanged name.
bool rename(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& name);

// Creating a prim (docs/CREATE_MENU.md §3). The target is where a new prim goes for
// this selection: under the one selected prim, else at the root. During variant edit
// mode, a selection the variant doesn't reach means the prim whose variant it is.
// `problem` says why not, when it can't go there; then createPrim() refuses it too.
struct CreateTarget {
    PXR_NS::SdfPath parent;
    QString problem;
};
CreateTarget createTarget(const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& selection);
// Empty if a prim can be created under `parent`, else why not.
QString whyNotCreateUnder(const PXR_NS::UsdPrim& parent);
// `type` is a concrete schema type name, or empty for a prim with no type. One undo
// step. It's named after its type, made unique among the parent's children, as
// DefinePrim on a name in use would change that prim's type. Returns its path, empty
// if refused, and announces it on Stage::primCreated.
PXR_NS::SdfPath createPrim(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& parent, const std::string& type);

// Payloads: whether anything at or under `path` is loadable, and whether some
// of it is unloaded / loaded.
bool hasPayloads(const StageRefPtr& stage, const PXR_NS::SdfPath& path);
bool canLoad(const StageRefPtr& stage, const PXR_NS::SdfPath& path);
bool canUnload(const StageRefPtr& stage, const PXR_NS::SdfPath& path);
// Load with descendants, or unload. Not a layer edit: the step pushed on the
// stage's undo stack puts the stage's load rules back.
bool load(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths);
bool unload(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths);

// Applied API schemas (docs/APPLIED_SCHEMAS.md), in the stage's edit target.
// `schema` is a name like "PhysicsMassAPI" or "CollectionAPI:ferns". Removing
// refuses a built-in one, where USD would report success and change nothing.
// With `cleanUp` it also removes, in the same undo step, the properties
// orphanedProperties() says the edit target has. Applying with `addAttributes`
// also authors, in the same undo step, what schemaAttributesToAdd() lists, each
// with its fallback value (declared only, if it has none).
bool applySchema(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& schema,
                 bool addAttributes = false);
bool removeSchema(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& schema,
                  bool cleanUp);

// Applies `schema` and sets a relationship's target, in one undo step (adding
// MaterialBindingAPI with its material).
bool applySchemaWithTarget(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                           const std::string& schema, const PXR_NS::TfToken& relationship, const PXR_NS::SdfPath& target);

// Relationship targets (docs/RELATIONSHIPS.md), in the stage's edit target. Adding
// creates the relationship if needed; with `replace` the target becomes its only
// one. Removing the last one removes the relationship's spec too, when the edit
// target holds its only opinion, rather than leaving an empty delete list behind.
bool addRelationshipTarget(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                           const PXR_NS::TfToken& relationship, const PXR_NS::SdfPath& target, bool replace);
bool removeRelationshipTarget(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                              const PXR_NS::TfToken& relationship, const PXR_NS::SdfPath& target);

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
