#pragma once

#include "usdcc/ui/view_panel.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <vector>

class QComboBox;
class QTreeView;

namespace usdcc::usd {

class StageManager;
class UsdPrimTreeModel;

// ViewPanel subclass showing a USD stage's prim hierarchy as a tree: rename
// (double-click/F2), toggle active/inactive ("disable" — the row's
// checkbox), select, and reorder siblings (tab's context menu: Move
// Up/Down) — see docs/PLAN.md milestone M4. Selection is written to
// StageManager::setSelectedPaths() so other panels showing the same stage
// (e.g. AttributesViewPanel) stay in sync.
class OutlinerViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    explicit OutlinerViewPanel(StageManager* stageManager, const QString& title = QStringLiteral("Outliner"),
                                QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

protected:
    void populateContextMenu(QMenu* menu) override;

private slots:
    void refreshStageCombo();
    void onStageComboChanged(int index);
    void onTreeSelectionChanged();
    void onStageSelectionChanged(PXR_NS::UsdStageRefPtr stage, std::vector<PXR_NS::SdfPath> paths);
    void onPrimRenamed(const PXR_NS::SdfPath& oldPath, const PXR_NS::SdfPath& newPath);

private:
    void moveSelectedPrim(int direction);  // -1 = up, +1 = down

    StageManager* m_stageManager;
    PXR_NS::UsdStageRefPtr m_stage;
    QComboBox* m_stageCombo;
    QTreeView* m_treeView;
    UsdPrimTreeModel* m_model;
    bool m_updatingSelection = false;
};

}  // namespace usdcc::usd
