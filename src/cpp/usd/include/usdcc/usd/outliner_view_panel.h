#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <set>
#include <vector>

class QToolButton;
class QTreeView;

namespace usdcc::usd {

class StageManager;
class UsdPrimTreeModel;
class UndoManager;

// ViewPanel subclass showing a USD stage's prim hierarchy as a tree: rename
// (double-click/F2), toggle active/inactive ("disable" — the row's
// checkbox), select, and reorder siblings (tab's context menu: Move
// Up/Down) — see docs/PLAN.md milestone M4. Selection is written to
// StageManager::setSelectedPaths() so other panels showing the same stage
// (e.g. AttributesViewPanel) stay in sync. The selected prims' ancestors are
// tinted, and with "Expand to Selection" (on by default, saved in the
// settings) a selection made anywhere, e.g. by picking in the viewport,
// expands the tree down to it and scrolls it into view.
class OutlinerViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    explicit OutlinerViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undoManager,
                                const QString& title = QStringLiteral("Outliner"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

protected:
    void populateContextMenu(QMenu* menu) override;

    // Points the tree model at the new stage and restores its selection.
    void onStageChanged(const StageRefPtr& stage) override;

private Q_SLOTS:
    void onTreeSelectionChanged();
    void onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths);
    void onPrimRenamed(const PXR_NS::SdfPath& oldPath, const PXR_NS::SdfPath& newPath);

private:
    void moveSelectedPrim(int direction);  // -1 = up, +1 = down
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
    QToolButton* m_expandToSelection;
    bool m_updatingSelection = false;
    std::shared_ptr<UndoManager> m_undoManager;
    bool m_refreshQueued = false;
    // View state carried across a model reset; not across a stage switch,
    // whose paths belong to another stage.
    bool m_keepViewState = true;
    std::set<PXR_NS::SdfPath> m_expandedPaths;
    int m_scrollPosition = 0;

    // Kept alive for as long as this panel wants to keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChangedConnection;
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
};

}  // namespace usdcc::usd
