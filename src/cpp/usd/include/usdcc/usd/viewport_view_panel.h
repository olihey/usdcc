#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <memory>
#include <vector>

class QComboBox;

namespace usdcc::usd {

class StageManager;
class HydraViewportWindow;
class UndoManager;

// ViewPanel subclass that renders a USD stage via Hydra and lets the user
// switch between loaded stages and available render delegates (see
// docs/PLAN.md milestone M3). Actual navigation (orbit/pan/zoom), rendering,
// and tool/gizmo handling live in HydraViewportWindow; this class is the
// ViewPanel/toolbar wrapper around it. Which editing tool is active, and its
// options, are not per panel: they live in a ToolSet shared by every viewport
// and are controlled from the main window's tool rail and options bar (see
// ViewportToolBars).
//
// Deliberately built as part of the usdcc_usd_ui target (see
// src/cpp/usd/CMakeLists.txt) rather than usdcc_ui: it only exists when USD
// is available, and keeping it out of usdcc_usd itself means usdcc.usd's
// pybind11 module doesn't pull in Qt Widgets/ADS as a runtime dependency it
// doesn't otherwise need.
class ViewportViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    // `toolSet` is shared with every other viewport (and the toolbars that
    // drive it); it must not be null. Gizmo drags are recorded as undo steps in
    // `undoManager`'s stack for the viewed stage.
    explicit ViewportViewPanel(StageManager* stageManager, StageRefPtr stage,
                                std::shared_ptr<usdcc::tools::ToolSet> toolSet,
                                std::shared_ptr<UndoManager> undoManager,
                                const QString& title = QStringLiteral("Viewport"), QWidget* parent = nullptr);

    HydraViewportWindow* hydraWindow() const { return m_viewport; }

    // Copies the currently-viewed stage, the current render delegate, and
    // the camera position onto the duplicate — see ViewPanel::duplicate().
    // The duplicate shares this panel's ToolSet.
    ViewPanel* duplicate(QWidget* parent = nullptr) const override;

protected:
    // Adds a "Reset Camera" entry on top of ViewPanel's base "Duplicate" —
    // see ViewPanel::populateContextMenu() for the extension mechanism.
    void populateContextMenu(QMenu* menu) override;

    // Points the viewport at the new stage and restores its selection.
    void onStageChanged(const StageRefPtr& stage) override;

private Q_SLOTS:
    void refreshRendererCombo();
    void onRendererComboChanged(int index);
    void onSelectionRequested(std::vector<PXR_NS::SdfPath> paths);
    void onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths);

private:
    // Passes the stage's tool target (for the selected prim) to the viewport.
    void syncTarget();

    HydraViewportWindow* m_viewport;
    QComboBox* m_rendererCombo;
    std::shared_ptr<usdcc::tools::ToolSet> m_toolSet;
    std::shared_ptr<UndoManager> m_undoManager;
    StageRefPtr m_toolEditStage;  // the stage a gizmo drag's undo session was opened on

    // Kept alive for as long as this panel wants to keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChangedConnection;
    // The viewed stage's change notices, which trigger a redraw.
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<>::Connection m_targetConnection;
};

}  // namespace usdcc::usd
