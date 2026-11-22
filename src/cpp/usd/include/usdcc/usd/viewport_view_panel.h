#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/scene_counts.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/viewport_display.h"

#include <pxr/base/gf/camera.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QColor>
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
class ViewportViewPanel;

// An entry added to every viewport's menus, by a plugin say (docs/PLUGINS.md): at the end
// of its options menu and of its tab's menu, in the order added.
struct ViewportMenuEntry {
    std::string id;  // adding one again replaces it
    QString label;   // "Group/Action" makes a submenu
    std::function<void(ViewportViewPanel*)> run;
    // Checkable, and checked when this says so; null: a plain entry.
    std::function<bool(ViewportViewPanel*)> checked;
    // Left out of the menu when false; may be null.
    std::function<bool(ViewportViewPanel*)> applies;
    // Shown but greyed out when false; may be null.
    std::function<bool(ViewportViewPanel*)> enabled;
};

class USDCC_USD_UI_API ViewportMenuEntries {
public:
    static ViewportMenuEntries& instance();
    void add(ViewportMenuEntry entry);
    void remove(const std::string& id);
    void clear() { m_entries.clear(); }
    const std::vector<ViewportMenuEntry>& entries() const { return m_entries; }
    // Adds the entries that apply to `viewport` to the end of `menu`, after a separator.
    void populate(QMenu* menu, ViewportViewPanel* viewport) const;

private:
    std::vector<ViewportMenuEntry> m_entries;
};

// The Shortcut setting for Fit to View (F by default).
inline constexpr const char* kFitToViewShortcutKey = "Shortcuts/viewport.fitToView";
// The Shortcut setting for Fit All Views (Shift+F by default, docs/WORKSPACES.md §3.5.7).
inline constexpr const char* kFitAllViewsShortcutKey = "Shortcuts/viewport.fitAllViews";
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
    // Through a stage camera, what a drag does (docs/VIEWPORT_CAMERA.md §2): "detach", "drive"
    // or "locked". This viewport's own choice, over Settings' default.
    void setCameraNavigation(const std::string& mode);

    // A border in `color` around the 3D view, to set this viewport apart: a broadcast
    // marks the one it sends (docs/VIEWPORT_BROADCAST.md §2.1). The view shrinks by the
    // border while it's shown. An invalid colour (the default) is none.
    void setBorderColor(const QColor& color);
    QColor borderColor() const;

    // Fit to View: moves the camera so the selected prims fill the view, or the whole
    // stage when nothing is selected (sceneBounds()). Selected prims without bounds are
    // left out; when none of them has any (or the stage is empty), a warning toast says
    // so and the camera stays. Returns whether it moved the camera.
    bool fitToView();

    // Fit All Views (docs/WORKSPACES.md §3.5.7): fits every viewport on screen (open, its
    // tab in front), each keeping its view. `what`: "selection" (else the whole stage),
    // or "stage".
    static void fitAllViews(const QString& what = QStringLiteral("selection"));
    // The application-wide Fit All Views action, as addFitToViewAction(); its key from
    // kFitAllViewsShortcutKey.
    static QAction* addFitAllViewsAction(QWidget* window);

    // Reset Camera: the default perspective view; an axis view stays that view, at the
    // origin. Detached from a stage camera, it looks through that camera again first
    // (docs/VIEWPORT_CAMERA.md §2.1).
    void resetCamera();

    // Looking through the stage's cameras (docs/VIEWPORT_CAMERA.md). `path`: one of the
    // stage's UsdGeomCamera prims, or empty for the orbit camera, where it was. False for a
    // path that isn't a camera of the stage.
    bool lookThrough(const PXR_NS::SdfPath& path);
    // Back to Camera (§2.1): looks through the camera the orbit camera last left. False
    // when there's none.
    bool backToCamera();
    // Copy View to Camera (§2.1): sets the camera the orbit camera left to the view, one
    // undo step in the edit target, then looks through it. False, with a toast saying why,
    // when it can't (whyNotSetCamera()).
    bool copyViewToCamera();
    // New Camera from View (§2.1): defines a camera seeing the view at the stage's root (in
    // variant edit mode, under the variant's prim), one undo step, then looks through it.
    // Its path; empty when it couldn't, with a toast.
    PXR_NS::SdfPath newCameraFromView();
    // Where a Look Through on `stage`'s camera goes: the active viewport when it shows the
    // stage, else the oldest open one that does. Null for none.
    static ViewportViewPanel* viewportFor(const StageRefPtr& stage);

    // Workspaces (docs/WORKSPACES.md §3.5): a workspace entry's camera, the view as a
    // workspace names it, and fitting without a toast. A fit asked for before the
    // viewport has a size happens once it has one.
    void applyWorkspaceCamera(const QJsonObject& camera) override;
    QString workspaceView() const override;
    QJsonObject workspaceCamera() const override;
    bool fitView(const QString& what) override;

    // Whether this viewport may close: every close guard agrees (addCloseGuard()). Asked
    // by its tab's close button, the tab menu's Close, closing its dock area or floating
    // window, and a workspace switch. Deleting the panel (closing its stage) doesn't ask.
    bool canClose() override;
    // Something a plugin keeps going in a viewport, which closing would end: the guard
    // says whether `viewport` may close, and may ask the user (a broadcast does,
    // docs/VIEWPORT_BROADCAST.md §2.1). An id for removeCloseGuard(). Main thread only.
    using CloseGuard = std::function<bool(ViewportViewPanel* viewport)>;
    static int addCloseGuard(CloseGuard guard);
    static void removeCloseGuard(int id);

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
    // Fired with a viewport whose renderer settings changed: its renderer, the values that
    // renderer gets (an override, or a default it follows), or its overrides of any
    // renderer. Only for a real change, unlike rendererSettingsChanged(). Main thread only;
    // the "viewport_renderer_settings_changed" Python event.
    static usdcc::core::Signal<ViewportViewPanel*>& anyRendererSettingsChanged();

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
    // Switches the renderer, as the Renderer menu does; before the first frame, the one it
    // starts with (HydraViewportWindow::setRendererPlugin()). Tells anyRendererSettingsChanged()
    // either way.
    void setRenderer(const PXR_NS::TfToken& pluginId);
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

    // While busy (docs/PANEL_BUSY.md §4): the viewport's last frame, which it has even when
    // covered, in place of a screen grab.
    QImage busySnapshot(QWidget* container) override;

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
    // Fits now: "selection" (else the stage) or "stage". `quiet`: no toast when there's
    // nothing to fit.
    bool fitNow(const QString& what, bool quiet);
    // A fit asked for before the viewport could do it, once it can.
    void runPendingFit();
    // Makes this the active viewport; tells activeViewportChanged() if it wasn't.
    void makeActive();
    // Hands displayOptions() to the viewport and tells displayChanged().
    void applyDisplay();
    // Changes some overrides, leaving the others as they are.
    void editOverrides(const std::function<void(ViewportDisplayOverrides&)>& change);
    // Hands the current renderer's rendererSettingValues() to the viewport and tells
    // rendererSettingsChanged().
    void applyRendererSettings();
    // The Renderer, Shading and Camera entries, for the options menu and the HUD's.
    void addRendererEntries(QMenu* menu);
    void addShadingEntries(QMenu* menu);
    void addCameraEntries(QMenu* menu);
    // The workspace camera's "path" (docs/WORKSPACES.md §3.5.5): looks through it, once the
    // stage has it. Whether it does.
    bool lookThroughSpecCamera();
    // The orbit camera's view as `like` would see it (its aperture's shape, its clipping).
    PXR_NS::GfCamera viewAsCamera(const PXR_NS::GfCamera& like) const;

    // The HUD's counts (docs/VIEWPORT_HUD.md §2.3), a slice at a time on the main thread,
    // as the stage is edited there. `reset` drops the numbers shown until new ones come.
    bool countsWanted() const;
    void recountStage(bool reset);
    void recountSelection(bool reset);
    void runCounts();
    void publishCounts();

    HydraViewportWindow* m_viewport;
    QWidget* m_viewportFrame = nullptr;  // Around the 3D view's container: its border
    int m_id;
    // The workspace camera it was made with (§3.5.1): a following view refits by it.
    QJsonObject m_cameraSpec;
    QString m_pendingFit;
    std::shared_ptr<usdcc::tools::ToolSet> m_toolSet;
    std::shared_ptr<UndoManager> m_undoManager;
    std::string m_driveProblem;  // why the drive in progress can't show; empty while it can
    StageRefPtr m_toolEditStage;  // the stage a gizmo drag's undo session was opened on
    ViewportDisplayOverrides m_displayOverrides;
    QPointer<QDialog> m_displayDialog;
    RendererOverrides m_rendererOverrides;
    QPointer<QDialog> m_rendererSettingsDialog;
    // What anyRendererSettingsChanged() last told, to tell only a real change.
    struct RendererState {
        PXR_NS::TfToken pluginId;
        RendererSettingValues values;
        RendererOverrides overrides;
        bool operator==(const RendererState&) const = default;
    };
    RendererState m_toldRendererState;

    // Kept alive for as long as this panel wants to keep receiving
    // StageManager's (Qt-free) notifications — see usdcc::core::Signal.
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionChangedConnection;
    // The viewed stage's change notices, which trigger a redraw.
    usdcc::core::Signal<const StageChanges&>::Connection m_stageChangedConnection;
    usdcc::core::Signal<>::Connection m_targetConnection;
    usdcc::core::Signal<>::Connection m_editTargetConnection;
    usdcc::core::Signal<double>::Connection m_timeConnection;  // the stage's time, to the viewport

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
