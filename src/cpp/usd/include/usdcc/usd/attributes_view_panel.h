#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <vector>

class QTableWidget;

namespace usdcc::usd {

class StageManager;
class UndoManager;

// ViewPanel subclass showing/editing the attributes of whichever prim is
// currently selected (via StageManager::selectedPaths()) on this panel's
// chosen stage — see docs/PLAN.md milestone M4. Shows the first prim when
// multiple are selected.
//
// Value editing covers common scalar types (bool/int/float/double/
// string/token) and Gf vector types (Float2/3/4, Double2/3/4, Int2/3/4,
// and role variants sharing those C++ types, e.g. Color3f/Vector3f/Point3f)
// via a "(x, y, z)"-style text format matching how USD itself displays
// them. Other types (arrays, matrices, asset paths, etc.) are shown
// read-only for now — see docs/PLAN.md §6 for the known gap.
class AttributesViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    explicit AttributesViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undoManager,
                                  const QString& title = QStringLiteral("Attributes"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

protected:
    // Resets the selected-prim path and refreshes the table for the new stage.
    void onStageChanged(const StageRefPtr& stage) override;

private Q_SLOTS:
    void onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths);
    void onCellChanged(int row, int column);

private:
    void refreshAttributes();
    void onStageObjectsChanged(const StageChanges& changes);

    PXR_NS::SdfPath m_primPath;
    QTableWidget* m_table;
    bool m_updatingTable = false;
    bool m_refreshQueued = false;
    std::shared_ptr<UndoManager> m_undoManager;

    // Kept alive for as long as this panel wants to keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChangedConnection;
    // The shown stage's change notices (re-made whenever the stage changes), so
    // values follow gizmo drags, undo/redo and edits made elsewhere.
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
};

}  // namespace usdcc::usd
