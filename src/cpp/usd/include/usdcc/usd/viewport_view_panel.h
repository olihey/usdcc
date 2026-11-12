#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/viewport_display.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QPointer>

#include <memory>
#include <vector>

class QAction;
class QDialog;

namespace usdcc::usd {

class StageManager;
class HydraViewportWindow;

// The Shortcut setting for Fit to View (F by default).
inline constexpr const char* kFitToViewShortcutKey = "Shortcuts/viewport.fitToView";
class UndoManager;

// ViewPanel subclass that renders a USD stage via Hydra and lets the user
// switch between loaded stages and, from the options menu's Renderer
// submenu, the available render delegates (see docs/PLAN.md milestone M3). Actual navigation (orbit/pan/zoom), rendering,
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
class USDCC_USD_UI_API ViewportViewPanel : public usdcc::ui::ViewPanel {
    Q_OBJECT

public:
    // `toolSet` is shared with every other viewport (and the toolbars that
    // drive it); it must not be null. Gizmo drags are recorded as undo steps in
    // `undoManager`'s stack for the viewed stage.
    explicit ViewportViewPanel(StageManager* stageManager, StageRefPtr stage,
                                std::shared_ptr<usdcc::tools::ToolSet> toolSet,
                                std::shared_ptr<UndoManager> undoManager,
                                const QString& title = QStringLiteral("Viewport"), QWidget* parent = nullptr);
    ~ViewportViewPanel() override;

    HydraViewportWindow* hydraWindow() const { return m_viewport; }

    // Fit to View: moves the camera so the selected prims fill the view, or the whole
    // stage when nothing is selected (sceneBounds()). Selected prims without bounds are
    // left out; when none of them has any (or the stage is empty), a warning toast says
    // so and the camera stays. Returns whether it moved the camera.
    bool fitToView();

    // The application-wide Fit to View action, for the active viewport: added to
    // `window`, which owns it, with its key from kFitToViewShortcutKey (defined here).
    static QAction* addFitToViewAction(QWidget* window);

    // The viewport the user last worked in: the last one clicked, or until then
    // the oldest one open. Null when no viewport is open. What follows "the
    // viewport camera" uses it (usdcc.app.active_viewport_camera()).
    static ViewportViewPanel* activeViewport();
    // Fired when activeViewport() changes, its camera moves, or it switches
    // stages. Main thread only.
    static usdcc::core::Signal<>& activeViewportChanged();
    // The renderer of the viewport showing `stage`: the one clicked last, else the oldest.
    // Empty when no viewport shows it. The Material Editor's preview follows it
    // (docs/MATERIAL_EDITOR.md §7.5).
    static PXR_NS::TfToken rendererFor(const StageRefPtr& stage);
    // Fired when a viewport opens or closes, is clicked, or switches stage or renderer:
    // when rendererFor() may say something else. Main thread only.
    static usdcc::core::Signal<>& viewportsChanged();

    // The grid and background (docs/VIEWPORT_GRID.md). A viewport shows the Settings >
    // Viewport defaults, except for what it overrides; a value it doesn't override
    // follows its setting when that changes.
    ViewportDisplayOverrides displayOverrides() const { return m_displayOverrides; }
    void setDisplayOverrides(const ViewportDisplayOverrides& overrides);
    // What it shows: the defaults with the overrides applied.
    HydraViewportWindow::DisplayOptions displayOptions() const;
    // The Display dialog (grid, background, selection) for this viewport (the options menu's entry); one
    // at a time, raised if it's already open.
    QDialog* showDisplayDialog();

    // Copies the currently-viewed stage, the current render delegate, the
    // camera position and the grid and background overrides onto the
    // duplicate — see ViewPanel::duplicate().
    // The duplicate shares this panel's ToolSet.
    ViewPanel* duplicate(QWidget* parent = nullptr) const override;
    // The camera, the renderer and the display overrides (docs/SESSIONS.md §2.4).
    QJsonObject saveSessionState() const override;
    void restoreSessionState(const QJsonObject& state) override;

protected:
    // Adds "Fit to View" and "Reset Camera" entries on top of ViewPanel's base "Duplicate" —
    // see ViewPanel::populateContextMenu() for the extension mechanism.
    void populateContextMenu(QMenu* menu) override;
    // A Renderer submenu listing the render delegates, the current one checked; Show
    // Grid, Selection, Display… and Use Settings Defaults.
    void populateOptionsMenu(QMenu* menu) override;

    // Points the viewport at the new stage and restores its selection.
    void onStageChanged(const StageRefPtr& stage) override;
    // An opened stage goes to one viewport only, which main.cpp picks.
    bool adoptsOpenedStage() const override { return false; }

Q_SIGNALS:
    // displayOptions() changed: an override, or a default it follows.
    void displayChanged();

private Q_SLOTS:
    void onSelectionRequested(std::vector<PXR_NS::SdfPath> paths);
    void onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths);

private:
    // Passes the stage's tool target (for the selected prim) to the viewport.
    void syncTarget();
    // Makes this the active viewport; tells activeViewportChanged() if it wasn't.
    void makeActive();
    // Hands displayOptions() to the viewport and tells displayChanged().
    void applyDisplay();

    HydraViewportWindow* m_viewport;
    std::shared_ptr<usdcc::tools::ToolSet> m_toolSet;
    std::shared_ptr<UndoManager> m_undoManager;
    StageRefPtr m_toolEditStage;  // the stage a gizmo drag's undo session was opened on
    ViewportDisplayOverrides m_displayOverrides;
    QPointer<QDialog> m_displayDialog;

    // Kept alive for as long as this panel wants to keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChangedConnection;
    // The viewed stage's change notices, which trigger a redraw.
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<>::Connection m_targetConnection;
};

}  // namespace usdcc::usd
