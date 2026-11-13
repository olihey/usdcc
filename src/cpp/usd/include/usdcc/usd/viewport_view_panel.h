#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/scene_counts.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/viewport_display.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QPointer>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QDialog;
class QTimer;

namespace usdcc::usd {

class StageManager;
class HydraViewportWindow;

// The Shortcut setting for Fit to View (F by default).
inline constexpr const char* kFitToViewShortcutKey = "Shortcuts/viewport.fitToView";
// The Shortcut setting for Toggle HUD (none by default).
inline constexpr const char* kToggleHudShortcutKey = "Shortcuts/viewport.toggleHud";
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
    // The same for Toggle HUD: turns the active viewport's HUD on or off (its own
    // override), with its key from kToggleHudShortcutKey.
    static QAction* addToggleHudAction(QWidget* window);

    // The menu a HUD label opens (docs/VIEWPORT_HUD.md §3), filled in: Camera (Persp, Fit
    // to View, Reset Camera), Renderer, Shading, or EditTarget (the stage's layers, the
    // edit target checked). The caller shows it.
    void populateHudMenu(QMenu* menu, HudItem item);

    // The viewport the user last worked in: the last one clicked, or until then
    // the oldest one open. Null when no viewport is open. What follows "the
    // viewport camera" uses it (usdcc.app.active_viewport_camera()).
    static ViewportViewPanel* activeViewport();
    // Every open viewport, oldest first.
    static std::vector<ViewportViewPanel*> openViewports();
    // A number for this viewport, never given to another in this run: how a script refers
    // to it (usdcc.app.Viewport). withId() finds it, or null once it's closed.
    int id() const { return m_id; }
    static ViewportViewPanel* withId(int id);
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

    // The render delegates' own settings (docs/VIEWPORT_DISPLAY.md §7): a viewport uses
    // Settings > Renderers' values, except for what it overrides, per renderer. Overrides
    // of a renderer that isn't in use are kept for when it is.
    using RendererOverrides = std::map<PXR_NS::TfToken, RendererSettingValues>;  // by plugin
    const RendererOverrides& rendererOverrides() const { return m_rendererOverrides; }
    void setRendererOverrides(const RendererOverrides& overrides);
    // Sets one override, or drops it (an empty `value`).
    void setRendererOverride(const PXR_NS::TfToken& pluginId, const PXR_NS::TfToken& key,
                             const std::optional<PXR_NS::VtValue>& value);
    // What `pluginId`'s delegate gets here: the defaults with the overrides applied.
    RendererSettingValues rendererSettingValues(const PXR_NS::TfToken& pluginId) const;
    // The Renderer Settings dialog for the current renderer (the Renderer menu's entry);
    // one at a time, raised if it's already open.
    QDialog* showRendererSettingsDialog();

    // Copies the currently-viewed stage, the current render delegate, the
    // camera position, and the display and renderer settings overrides onto the
    // duplicate — see ViewPanel::duplicate().
    // The duplicate shares this panel's ToolSet.
    ViewPanel* duplicate(QWidget* parent = nullptr) const override;
    // The camera, the renderer, and the display and renderer settings overrides
    // (docs/SESSIONS.md §2.4).
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
    // rendererSettingValues() may say something else: an override, a default, or the
    // renderer changed.
    void rendererSettingsChanged();

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
    // Changes some overrides, leaving the others as they are.
    void editOverrides(const std::function<void(ViewportDisplayOverrides&)>& change);
    // Hands the current renderer's rendererSettingValues() to the viewport and tells
    // rendererSettingsChanged().
    void applyRendererSettings();
    // The Renderer and Shading entries, for the options menu and the HUD's.
    void addRendererEntries(QMenu* menu);
    void addShadingEntries(QMenu* menu);

    // The HUD's counts (docs/VIEWPORT_HUD.md §2.3), a slice at a time on the main thread,
    // as the stage is edited there. `reset` drops the numbers shown until new ones come.
    bool countsWanted() const;
    void recountStage(bool reset);
    void recountSelection(bool reset);
    void runCounts();
    void publishCounts();

    HydraViewportWindow* m_viewport;
    int m_id;
    std::shared_ptr<usdcc::tools::ToolSet> m_toolSet;
    std::shared_ptr<UndoManager> m_undoManager;
    StageRefPtr m_toolEditStage;  // the stage a gizmo drag's undo session was opened on
    ViewportDisplayOverrides m_displayOverrides;
    QPointer<QDialog> m_displayDialog;
    RendererOverrides m_rendererOverrides;
    QPointer<QDialog> m_rendererSettingsDialog;

    // Kept alive for as long as this panel wants to keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChangedConnection;
    // The viewed stage's change notices, which trigger a redraw.
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<>::Connection m_targetConnection;
    usdcc::core::Signal<>::Connection m_editTargetConnection;

    QTimer* m_countTimer = nullptr;
    std::unique_ptr<SceneCountJob> m_stageCountJob;
    std::unique_ptr<SceneCountJob> m_selectionCountJob;
    int m_stageCountSlices = 0;  // how many slices the job under way has had
    int m_selectionCountSlices = 0;
    HydraViewportWindow::HudCounts m_counts;
    // What was counted with: a change to either counts again.
    bool m_countsOn = false;
    PXR_NS::TfTokenVector m_countedPurposes;
};

}  // namespace usdcc::usd
