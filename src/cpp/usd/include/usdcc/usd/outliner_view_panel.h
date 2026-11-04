#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <set>
#include <vector>

class QAction;
class QTreeView;

namespace usdcc::usd {

class StageManager;
class UsdPrimTreeModel;
class UndoManager;

// ViewPanel subclass showing a USD stage's prim hierarchy as a tree: rename
// (double-click/F2), toggle active/inactive ("disable" — the row's
// checkbox), select — see docs/PLAN.md milestone M4 — and a right-click menu
// on the rows (prim_context_menu.h, docs/OUTLINER_CONTEXT_MENU.md): Active,
// Visibility, Load/Unload Payload, Variants, Move Up/Down, Copy Path, Prim
// Info. Every edit, the checkbox and rename included, is an undo step. Selection is written to
// StageManager::setSelectedPaths() so other panels showing the same stage
// (e.g. AttributesViewPanel) stay in sync. The selected prims' ancestors are
// tinted, and with "Expand to Selection" (in the options menu; on by default,
// saved in the settings) a selection made anywhere, e.g. by picking in the viewport,
// expands the tree down to it and scrolls it into view.
class OutlinerViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    explicit OutlinerViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undoManager,
                                const QString& title = QStringLiteral("Outliner"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

protected:
    void populateOptionsMenu(QMenu* menu) override;

    // Points the tree model at the new stage and restores its selection.
    void onStageChanged(const StageRefPtr& stage) override;

private Q_SLOTS:
    void onTreeSelectionChanged();
    void onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths);
    void onPrimRenamed(const PXR_NS::SdfPath& oldPath, const PXR_NS::SdfPath& newPath);

private:
    void moveSelectedPrim(int direction);  // -1 = up, +1 = down
    // The rows' context menu, for the selection (the row under `pos` is
    // selected first if it isn't).
    void showPrimMenu(const QPoint& pos);
    // After Unload: drops selected prims that are gone with it.
    void dropMissingSelection();
    // Rebuilds the tree when the stage's prim hierarchy may have changed (a
    // prim path resynced: added, removed, renamed, reordered, (de)activated,
    // or one of those undone). Value-only edits — attribute changes, gizmo
    // drags — leave the tree alone. Coalesced into one rebuild per event loop
    // turn, since a single edit can send several notices.
    void onStageObjectsChanged(const StageChanges& changes);
    // Around every model reset: remember which prims are expanded (and the
    // scroll position), then expand them again and reselect, so a rebuild
    // doesn't collapse the tree.
    void saveViewState();
    void restoreViewState();
    // Expands every ancestor of the selected prims and scrolls the first one
    // into view.
    void expandToSelection(const std::vector<PXR_NS::SdfPath>& paths);

    QTreeView* m_treeView;
    UsdPrimTreeModel* m_model;
    QAction* m_expandToSelection;
    bool m_updatingSelection = false;
    std::shared_ptr<UndoManager> m_undoManager;
    bool m_refreshQueued = false;
    PXR_NS::SdfPath m_revealAfterRefresh;  // a prim just created
    // View state carried across a model reset; not across a stage switch,
    // whose paths belong to another stage.
    bool m_keepViewState = true;
    std::set<PXR_NS::SdfPath> m_expandedPaths;
    int m_scrollPosition = 0;

    // Kept alive for as long as this panel wants to keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChangedConnection;
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<const PXR_NS::SdfPath&>::Connection m_primCreatedConnection;
};

}  // namespace usdcc::usd
