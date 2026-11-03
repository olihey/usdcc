#pragma once

#include "usdcc/core/signal.h"

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <memory>
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
    // `usdStage` must already be registered in UsdUtilsStageCache::Get()
    // (StageManager::openStage() does this before constructing a Stage) —
    // cacheId() is looked up once here, at construction, rather than on
    // every call.
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
    // After usdcc saved one or more of the stage's layers (layer_actions):
    // their unsaved-changes marks change without a change notice.
    usdcc::core::Signal<> layersSaved;

private:
    class ChangeListener;
    void validateTarget();

    PXR_NS::UsdStageRefPtr m_usdStage;
    long m_cacheId;
    std::vector<PXR_NS::SdfPath> m_selectedPaths;
    PXR_NS::SdfPath m_targetPrim;
    PXR_NS::TfToken m_targetOp;
    std::unique_ptr<ChangeListener> m_changeListener;  // after `changed`, which it fires
};

using StageRefPtr = std::shared_ptr<Stage>;

}  // namespace usdcc::usd
