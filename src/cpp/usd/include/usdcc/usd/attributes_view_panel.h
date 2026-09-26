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
    explicit AttributesViewPanel(StageManager* stageManager, StageRefPtr stage,
                                  const QString& title = QStringLiteral("Attributes"), QWidget* parent = nullptr);

    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

protected:
    // Resets the selected-prim path and refreshes the table for the new stage.
    void onStageChanged(const StageRefPtr& stage) override;

private slots:
    void onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths);
    void onCellChanged(int row, int column);

private:
    void refreshAttributes();

    PXR_NS::SdfPath m_primPath;
    QTableWidget* m_table;
    bool m_updatingTable = false;

    // Kept alive for as long as this panel wants to keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChangedConnection;
};

}  // namespace usdcc::usd
