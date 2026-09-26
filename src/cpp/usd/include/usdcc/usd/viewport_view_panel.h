#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <vector>

class QComboBox;

namespace usdcc::usd {

class StageManager;
class HydraViewportWindow;

// ViewPanel subclass that renders a USD stage via Hydra and lets the user
// switch between loaded stages and available render delegates (see
// docs/PLAN.md milestone M3), and switch between the Select/Move/Rotate/
// Scale editing tools (milestone M5). Actual navigation (orbit/pan/zoom),
// rendering, and tool/gizmo handling live in HydraViewportWindow; this class
// is the ViewPanel/toolbar wrapper around it.
//
// Deliberately built as part of the usdcc_usd_ui target (see
// src/cpp/usd/CMakeLists.txt) rather than usdcc_ui: it only exists when USD
// is available, and keeping it out of usdcc_usd itself means usdcc.usd's
// pybind11 module doesn't pull in Qt Widgets/ADS as a runtime dependency it
// doesn't otherwise need.
class ViewportViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    explicit ViewportViewPanel(StageManager* stageManager, StageRefPtr stage,
                                const QString& title = QStringLiteral("Viewport"), QWidget* parent = nullptr);

    HydraViewportWindow* hydraWindow() const { return m_viewport; }

    // Copies the currently-viewed stage, the current render delegate, and
    // the camera position onto the duplicate — see ViewPanel::duplicate().
    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

protected:
    // Adds a "Reset Camera" entry on top of ViewPanel's base "Duplicate" —
    // see ViewPanel::populateContextMenu() for the extension mechanism.
    void populateContextMenu(QMenu* menu) override;

    // Points the viewport at the new stage and restores its selection.
    void onStageChanged(const StageRefPtr& stage) override;

private slots:
    void refreshRendererCombo();
    void onRendererComboChanged(int index);
    void onToolComboChanged(int index);
    void onSelectionRequested(std::vector<PXR_NS::SdfPath> paths);
    void onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths);

private:
    HydraViewportWindow* m_viewport;
    QComboBox* m_rendererCombo;
    QComboBox* m_toolCombo;

    // Kept alive for as long as this panel wants to keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChangedConnection;
};

}  // namespace usdcc::usd
