#pragma once

#include "usdcc/core/signal.h"

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/prim.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace usdcc::usd {

// What one UsdNotice::ObjectsChanged reported, in plain data (see Stage::changed).
struct StageChanges {
    // Prims/properties whose composition or existence may have changed —
    // anything under these paths must be re-read from scratch.
    std::vector<PXR_NS::SdfPath> resyncedPaths;
    // Properties (or prims) where only a value or metadata changed.
    std::vector<PXR_NS::SdfPath> changedInfoOnlyPaths;

    // True if the prim at `primPath`, or any of its properties, changed in any way.
    bool affectsPrim(const PXR_NS::SdfPath& primPath) const;
    // True if the prim itself, one of its ancestors, or one of its properties
    // was resynced — i.e. the set of properties to show may be different now.
    bool resyncsPrim(const PXR_NS::SdfPath& primPath) const;
};

// Prims that changed path, old then new (docs/OUTLINER_REPARENT.md §3.8).
using PrimMoves = std::vector<std::pair<PXR_NS::SdfPath, PXR_NS::SdfPath>>;
// `path` after those moves: itself, or under a moved prim's new path.
PXR_NS::SdfPath pathAfterMoves(const PXR_NS::SdfPath& path, const PrimMoves& moves);

// Which variant edits are going into (docs/VARIANT_EDITOR.md §5).
struct VariantEdit {
    PXR_NS::SdfPath prim;
    std::string set;
    std::string variant;
};

// Whether the stage's edit target can author on `prim`. Always, unless it's a variant
// edit target and the prim is outside the variant's prim: USD would then write to the
// plain layer and report success (docs/VARIANT_EDITOR.md §2).
bool editTargetReaches(const PXR_NS::UsdPrim& prim);

// Removes the prim spec at `specPath` from `layer` if it's a plain `over` that says
// nothing, then its parents likewise: what's left when its last opinion goes. It stops
// at a variant, whose emptiness means something, and at anything with content.
void removeEmptyOvers(const PXR_NS::SdfLayerHandle& layer, PXR_NS::SdfPath specPath);

// Wraps a UsdStageRefPtr together with usdcc-specific, per-stage state that
// doesn't belong on USD's own Stage object — currently just the prim
// selection (see docs/PLAN.md milestone M4/§6 item 21), but the point of
// this class is to have a single place to grow that: e.g. a per-stage
// render setting would live here too, rather than in another StageManager-
// side map keyed by cache id.
//
// Always held as a StageRefPtr (shared_ptr<Stage>), never a bare Stage*:
// StageManager owns the canonical list, but — exactly like a
// UsdStageRefPtr itself — anyone holding a copy (e.g. a ViewPanel's "which
// stage am I showing" member) keeps both this wrapper and the underlying
// UsdStage alive even after StageManager::closeStage() removes it from the
// manager's own list. See StageManager::closeStage()'s comment.
class Stage {
public:
    // A stage StageManager opens is already registered in
    // UsdUtilsStageCache::Get() (StageManager::openStage() does this before
    // constructing a Stage) — cacheId() is looked up once here, at
    // construction, rather than on every call. One that isn't (a Show USDA
    // preview's own stage) has an invalid cacheId(), and never reaches what
    // reads it.
    explicit Stage(PXR_NS::UsdStageRefPtr usdStage);
    ~Stage();
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    const PXR_NS::UsdStageRefPtr& usdStage() const { return m_usdStage; }

    // Fired for every UsdNotice::ObjectsChanged the stage sends, whoever made
    // the edit (a panel, a gizmo, undo/redo, a script), synchronously on the
    // editing thread. Lets panels update live instead of polling.
    usdcc::core::Signal<const StageChanges&> changed;

    // The UsdUtilsStageCache::Get() id this stage was registered under when
    // opened (computed once, at construction) — see
    // StageManager::stageCacheIds() for why this crosses into Python
    // instead of the stage itself.
    long cacheId() const { return m_cacheId; }

    const std::vector<PXR_NS::SdfPath>& selectedPaths() const { return m_selectedPaths; }
    void setSelectedPaths(std::vector<PXR_NS::SdfPath> paths) { m_selectedPaths = std::move(paths); }

    // The transform op the Move/Rotate/Scale tools act on (docs/ATTRIBUTE_EDITOR.md
    // §7.1): an op name on a prim, e.g. xformOp:scale:second on /World/Box.
    // Empty means Auto — each tool picks the first op of its own type. It is
    // cleared automatically when the op leaves the prim's xformOpOrder (or the
    // prim goes away), and by StageManager when the selection moves to
    // another prim.
    const PXR_NS::SdfPath& targetPrim() const { return m_targetPrim; }
    const PXR_NS::TfToken& targetXformOp() const { return m_targetOp; }
    // The target op if `primPath` is the target prim, else empty (Auto).
    PXR_NS::TfToken targetXformOpFor(const PXR_NS::SdfPath& primPath) const;
    void setTargetXformOp(const PXR_NS::SdfPath& primPath, const PXR_NS::TfToken& opName);
    void clearTargetXformOp() { setTargetXformOp(PXR_NS::SdfPath(), PXR_NS::TfToken()); }
    usdcc::core::Signal<> targetXformOpChanged;

    // The layer edits go to: UsdStage's edit target (docs/LAYER_EDITOR.md §3),
    // the root layer until something changes it.
    PXR_NS::SdfLayerHandle editTargetLayer() const;
    // Makes `layer` the edit target. False, and nothing changed, for a layer
    // that isn't in the stage's layer stack (session layers included) or is
    // muted.
    bool setEditTargetLayer(const PXR_NS::SdfLayerHandle& layer);
    // After the edit target changed, however it was changed (this, or
    // UsdStage::SetEditTarget() from a script).
    usdcc::core::Signal<> editTargetChanged;

    // Variant edit mode (docs/VARIANT_EDITOR.md §5): edits go into a variant, in the
    // layer that was the edit target, through the tools that always edit. The variant
    // is selected in the session layer, so the mode itself leaves the files alone.
    // Entering another variant of the same set switches to it; another set while in
    // the mode is refused (false), as is a variant the set doesn't have.
    bool enterVariantEdit(const PXR_NS::SdfPath& prim, const std::string& set, const std::string& variant);
    // Back to the plain layer, with the session layer as it was. Anything else that
    // changes the edit target leaves the mode as well.
    void exitVariantEdit();
    const std::optional<VariantEdit>& variantEdit() const { return m_variantEdit; }
    bool isEditingVariantSet(const PXR_NS::SdfPath& prim, const std::string& set) const;
    // After the mode was entered or left, or moved to another variant.
    usdcc::core::Signal<> variantEditChanged;

    // After the Create menu made a prim: the Outliner shows it, however far down it is.
    usdcc::core::Signal<const PXR_NS::SdfPath&> primCreated;
    // After prims changed path: a rename or a move, and undoing or redoing one. What
    // holds a path follows it: the selection (StageManager) and the Outliner's rows.
    usdcc::core::Signal<const PrimMoves&> primsMoved;
    // After usdcc saved one or more of the stage's layers (layer_actions):
    // their unsaved-changes marks change without a change notice.
    usdcc::core::Signal<> layersSaved;

private:
    class ChangeListener;
    void validateTarget();
    void leaveVariantEdit(bool resetTarget);

    PXR_NS::UsdStageRefPtr m_usdStage;
    long m_cacheId;
    std::vector<PXR_NS::SdfPath> m_selectedPaths;
    PXR_NS::SdfPath m_targetPrim;
    PXR_NS::TfToken m_targetOp;
    std::optional<VariantEdit> m_variantEdit;
    PXR_NS::SdfLayerHandle m_variantEditLayer;
    std::optional<std::string> m_sessionSelectionBefore;  // the set's, in the session layer
    bool m_changingVariantTarget = false;
    std::unique_ptr<ChangeListener> m_changeListener;  // after `changed`, which it fires
};

using StageRefPtr = std::shared_ptr<Stage>;

}  // namespace usdcc::usd
