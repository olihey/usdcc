#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <vector>

class QComboBox;
class QTableWidget;

namespace usdcc::usd {

class StageManager;

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
    explicit AttributesViewPanel(StageManager* stageManager, const QString& title = QStringLiteral("Attributes"),
                                  QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

private slots:
    void refreshStageCombo();
    void onStageComboChanged(int index);
    void onStageSelectionChanged(PXR_NS::UsdStageRefPtr stage, std::vector<PXR_NS::SdfPath> paths);
    void onCellChanged(int row, int column);

private:
    void refreshAttributes();

    StageManager* m_stageManager;
    PXR_NS::UsdStageRefPtr m_stage;
    PXR_NS::SdfPath m_primPath;
    QComboBox* m_stageCombo;
    QTableWidget* m_table;
    bool m_updatingTable = false;

    // Kept alive for as long as this panel wants to keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<PXR_NS::UsdStageRefPtr>::Connection m_stageOpenedConnection;
    usdcc::core::Signal<PXR_NS::UsdStageRefPtr>::Connection m_stageClosedConnection;
    usdcc::core::Signal<PXR_NS::UsdStageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChangedConnection;
};

}  // namespace usdcc::usd
