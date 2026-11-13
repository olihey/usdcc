#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/orbit_camera.h"
#include "usdcc/usd/viewport_hud.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QColor>
#include <QElapsedTimer>
#include <QPoint>
#include <QImage>
#include <QWindow>

#include <deque>
#include <memory>
#include <optional>
#include <vector>

class QOpenGLContext;
class QTimer;
class QOpenGLFramebufferObject;
class QOpenGLFunctions_1_1;

// A plain `namespace PXR_NS { class UsdImagingGLEngine; }` here would declare
// a phantom, unrelated class: PXR_NS (aliased to `pxr`) merely re-exposes the
// real, internally-versioned namespace via a using-directive (see pxr/pxr.h)
// rather than being that namespace itself, so a forward declaration must open
// the real namespace with these macros instead — exactly how USD's own
// headers (e.g. usd/common.h) forward-declare classes.
PXR_NAMESPACE_OPEN_SCOPE
class UsdImagingGLEngine;
PXR_NAMESPACE_CLOSE_SCOPE

namespace usdcc::usd {

class SelectionOverlay;

// How the viewport draws what's selected (docs/VIEWPORT_SELECTION.md).
enum class SelectionStyle {
    Solid,      // filled with the selection color
    Outline,    // a line around the selection's silhouette
    Wireframe,  // the selection's edges
};

// How the viewport shades the stage (docs/VIEWPORT_DISPLAY.md §3.1). Only Storm draws
// them; any other renderer is always Smooth Shaded.
enum class ShadingMode {
    SmoothShaded,       // DRAW_SHADED_SMOOTH
    FlatShaded,         // DRAW_SHADED_FLAT
    WireframeOnShaded,  // DRAW_WIREFRAME_ON_SURFACE
    Wireframe,          // DRAW_WIREFRAME
    HiddenLine,         // the edges in front only: a depth pass, then DRAW_WIREFRAME
};

// How finely subdivision surfaces are refined (params.complexity 1.0-1.3).
enum class Complexity { Low, Medium, High, VeryHigh };

// Renders a USD stage via Hydra directly into a native QWindow surface, with
// its own QOpenGLContext calling swapBuffers() itself — rather than
// QOpenGLWidget's approach of rendering into an offscreen FBO that Qt then
// composites into the widget backing store. See docs/PLAN.md milestone M3
// (§6 item 12): the QOpenGLWidget version produced corrupted pixel output
// (correct geometry/camera, wrong per-pixel color) sharing a GL context with
// Hydra's Storm/Embree render delegates that way; embedding via QWindow,
// wrapped into the widget tree with QWidget::createWindowContainer(), avoids
// that Qt-widget-compositor layer entirely, matching how usdview and other
// Hydra-in-Qt integrations typically embed the viewport.
//
// Also owns the M5 editing tools (Select/Move/Rotate/Scale — see
// docs/PLAN.md milestone M5): the active tool gets first refusal on left
// mouse events (to pick a prim or grab a gizmo handle), and only when it
// declines does the existing orbit-camera handling kick in, so gizmo
// interaction never fights with camera navigation.
class USDCC_USD_UI_API HydraViewportWindow : public QWindow {
    Q_OBJECT

public:
    explicit HydraViewportWindow(QWindow* parent = nullptr);
    ~HydraViewportWindow() override;

    void setStage(const PXR_NS::UsdStageRefPtr& stage);

    // Whether firstFrameRendered() has already fired for the current stage —
    // a viewport that was already on screen draws it inside setStage().
    bool hasRenderedFirstFrame() const { return m_firstFrameRendered; }
    // The last frame drawn, at the window's size in pixels; null before the first. For a
    // session's preview where the screen can't be captured (docs/SESSIONS.md §6.2).
    QImage grabFrame();

    // Valid once the GL context has initialized; rendererPluginsChanged()
    // fires at that point.
    std::vector<PXR_NS::TfToken> rendererPlugins() const;
    QString rendererDisplayName(const PXR_NS::TfToken& pluginId) const;
    PXR_NS::TfToken currentRendererPlugin() const;
    // Before the first frame, the renderer it starts with.
    void setRendererPlugin(const PXR_NS::TfToken& pluginId);

    // The orbit/pan/zoom camera's state. Exposed so a caller duplicating a
    // ViewportViewPanel (see ViewPanel::duplicate()) can make the copy start
    // out looking at the same thing as the original, rather than resetting
    // to the default view.
    struct CameraState {
        PXR_NS::GfVec3d target{0.0, 0.0, 0.0};
        double distance = kDefaultOrbitDistance;
        double yaw = kDefaultOrbitYaw;
        double pitch = kDefaultOrbitPitch;  // above the ground, looking down
    };
    CameraState cameraState() const;
    void setCameraState(const CameraState& state);

    // Fit to View: moves the camera, keeping the way it looks, so that `box` (world
    // space) fills the view. See frameOrbitCamera().
    void frameBox(const PXR_NS::GfRange3d& box);

    // The camera this viewport renders with, as a frustum: position and
    // orientation (ComputeViewInverse() is the camera-to-world matrix), the
    // vertical field of view, the window's aspect ratio and the clipping range.
    // What an external renderer needs to frame the same view (usdcc.app's
    // active_viewport_camera(), docs/VRAY_RENDER.md).
    PXR_NS::GfFrustum frustum() const { return currentFrustum(); }
    // The vertical field of view a viewport starts with, in degrees.
    static double verticalFieldOfView();
    // The one frustum() uses: verticalFieldOfView() unless set, as the Material Editor's
    // preview does to look through a shape's camera. Clamped to (0, 180).
    double fieldOfView() const { return m_fovY; }
    void setFieldOfView(double degrees);

    // What the viewport draws besides the stage: the ground grid and the background
    // color (docs/VIEWPORT_GRID.md), and how the selection looks (docs/VIEWPORT_SELECTION.md). ViewportViewPanel sets them from the Settings
    // defaults and the panel's own overrides; this window just draws what it's given.
    struct DisplayOptions {
        bool showGrid = true;
        double gridSize = 20.0;  // the grid's width, in scene units, centred on the origin
        double gridStep = 1.0;   // between neighbouring lines
        // The dark theme's panel surface, #1c1c1e (see ui/resources/theme/dark.qss): the
        // design's viewports sit on the same colour as the panels around them.
        QColor background = QColor(0x1c, 0x1c, 0x1e);
        // How the selection is drawn (docs/VIEWPORT_SELECTION.md): the style, its color
        // (opaque), and the width of the line styles' lines, in UI pixels (1-8).
        SelectionStyle selectionStyle = SelectionStyle::Solid;
        QColor selectionColor = QColor(0xff, 0xa6, 0x00);
        int selectionWidth = 2;
        // How Hydra draws the stage (docs/VIEWPORT_DISPLAY.md): shading, refinement, which
        // purposes show, the stage's materials, culling, and the dome light's map behind.
        ShadingMode shading = ShadingMode::SmoothShaded;
        Complexity complexity = Complexity::Low;
        bool showGuides = false;
        bool showProxy = true;
        bool showRender = false;
        bool sceneMaterials = true;
        bool cullBackfaces = false;
        bool domeLightTextures = true;
        // The lights: the stage's own, a light at the camera, and Hydra's default dome.
        // The default ones only in the shaded modes.
        bool sceneLights = true;
        bool cameraLight = true;
        bool domeLight = false;
        // The heads-up display (docs/VIEWPORT_HUD.md): on at all, and each of its parts.
        // Units, edit target, Paused and the empty-viewport hint come with `hud` itself.
        bool hud = true;
        bool hudCamera = true;       // top left: camera, renderer, shading
        bool hudAxes = true;         // top right: the axis triad
        bool hudStats = false;       // bottom left: stage and selection counts
        bool hudPerformance = false; // bottom right: frame time, convergence
        bool hudToolReadout = true;  // what a gizmo drag is doing
        bool hudRollover = false;    // the prim under the cursor
        bool operator==(const DisplayOptions&) const = default;
    };
    // What a renderer that isn't Storm can draw of `shading`: Smooth Shaded only.
    static bool rendererDrawsShadingModes(const PXR_NS::TfToken& pluginId);
    DisplayOptions displayOptions() const { return m_display; }
    // Redraws when anything changed.
    void setDisplayOptions(const DisplayOptions& options);

    // The editing tools this viewport draws gizmos for and sends mouse events
    // to. Normally one ToolSet is shared by every viewport and driven by the
    // main window's tool rail and options bar (see ViewportToolBars); the
    // viewport redraws whenever the set's active tool or options change. A
    // viewport that is never given one uses a private set.
    void setToolSet(std::shared_ptr<usdcc::tools::ToolSet> toolSet);

    // Called by ViewportViewPanel when StageManager's selection changes for
    // the stage this viewport is currently showing, so the gizmo tools and
    // the engine's highlight both stay in sync with the outliner/attributes
    // panels (see docs/PLAN.md milestone M4).
    void setSelectedPaths(std::vector<PXR_NS::SdfPath> paths);

    // The transform op the tools should act on for the selected prim (empty =
    // Auto); passed to them as ToolContext::targetOpName. See Stage::targetXformOp().
    void setTargetXformOp(const PXR_NS::TfToken& opName);

    // The Material Editor's preview (docs/MATERIAL_EDITOR.md §7.5): no tools, gizmos, grid
    // or highlight, the left button orbits, and only the stage's own lights, as it brings
    // them. Set before the first frame.
    void setPreviewMode(bool preview);
    bool isPreviewMode() const { return m_previewMode; }
    // Whether a dome light's map is drawn behind the scene, for renderers that can: the
    // display options' domeLightTextures.
    void setDomeLightVisible(bool visible);
    bool isDomeLightVisible() const { return m_display.domeLightTextures; }
    // Paused, nothing renders: the last frame stays, fitted to the window, until resumed,
    // which renders at once with whatever changed meanwhile.
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }

    // The heads-up display (docs/VIEWPORT_HUD.md), drawn over the frame while the display
    // options' `hud` is on. Not in grabFrame(): a session's preview shows the scene only.
    //
    // The counts it shows (displayOptions().hudStats): ViewportViewPanel counts them, as it
    // sees the stage's change notices. Nothing: still counting.
    struct HudCounts {
        std::optional<SceneCounts> stage;
        std::optional<SceneCounts> selection;
        bool operator==(const HudCounts&) const = default;
    };
    void setHudCounts(const HudCounts& counts);
    // What the HUD would show now, and where it put it last frame (empty before one).
    HudInput hudInput() const;
    const HudLayout& hudLayout() const { return m_hudLayout; }
    // Draws the last frame again with the HUD as it is now: for what changes only the HUD.
    void refreshHud();
    // Clicking an axis end of the triad: looks along that axis (keeping target and
    // distance), or for its centre, takes the default yaw and pitch.
    void snapToAxis(HudItem item);

Q_SIGNALS:
    // A HUD label was clicked (Camera, Renderer, Shading, EditTarget): its menu belongs
    // under `globalPos`.
    void hudMenuRequested(usdcc::usd::HudItem item, QPoint globalPos);

    void rendererPluginsChanged();
    // setRendererPlugin() switched it.
    void rendererChanged();

    // Any mouse button pressed in the viewport. Its panel takes this as
    // focus: a native window's focus doesn't reach ADS's focus tracking.
    void pressed();

    // The right mouse button was released over the viewport: a context menu belongs at
    // `globalPos`. A native window gets no QContextMenuEvent its container would see.
    void contextMenuRequested(QPoint globalPos);

    // Emitted once per stage, after its first frame has been rendered. That
    // first Render() is when Hydra populates its scene from the stage, so it is
    // where a big stage's viewport actually stalls the UI.
    void firstFrameRendered();

    // Emitted when a tool (currently just SelectTool) changes the selection
    // itself, so ViewportViewPanel can push it into StageManager and keep
    // every other panel showing this stage in sync.
    void selectionRequested(std::vector<PXR_NS::SdfPath> paths);

    // Bracket a tool drag that edits the stage (see ToolResult::startsEdit):
    // started on the press that grabbed a gizmo handle, finished on release (or
    // when the drag is cut short, e.g. by switching tools). ViewportViewPanel
    // turns the pair into one undo step. `description` is the tool's name.
    void toolEditStarted(const QString& description);
    void toolEditFinished();

    // The camera moved (orbit, pan, zoom, setCameraState()) or the window's
    // aspect ratio changed: frustum() is different now.
    void cameraChanged();

protected:
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void ensureEngine();
    void configureEngine();
    // Destroys the engine with the context current, so its GL objects are
    // freed cleanly, and remembers its renderer for ensureEngine().
    void releaseEngine();
    // Paused: the last frame again, fitted to the window, on the background.
    void showLastFrame(int width, int height);
    // With a debug context (USDCC_GL_DEBUG=1, see main.cpp): logs each GL
    // error as the driver reports it, with the stack of the call.
    void startGlDebugLog();
    // Binds m_framebuffer, (re)made at `width` x `height` device pixels.
    void bindFramebuffer(int width, int height);
    void renderNow();
    // After the camera changed: redraw and tell cameraChanged()'s listeners.
    void cameraMoved();
    PXR_NS::GfVec3d cameraPosition() const;
    PXR_NS::GfFrustum currentFrustum() const;

    usdcc::tools::ToolContext buildToolContext();
    PXR_NS::GfVec2d ndcFromPixel(const QPointF& pixelPos) const;
    PXR_NS::SdfPath pickPrim(const PXR_NS::GfVec2d& ndcPos);
    void applyToolResult(const usdcc::tools::ToolResult& result);
    void finishToolEdit();
    void updateEngineSelection();
    // Outline and Wireframe (docs/VIEWPORT_SELECTION.md): the selection's lines over the
    // frame, from a second engine (SelectionOverlay). Nothing for Solid, which Hydra draws.
    void drawSelectionOverlay(const usdcc::tools::ToolContext& ctx, int width, int height);
    // The ground grid, depth-tested against what Hydra drew.
    void drawGrid(const PXR_NS::GfMatrix4d& viewMatrix, const PXR_NS::GfMatrix4d& projMatrix);
    void drawGizmo(const usdcc::tools::GizmoGeometry& geo, const PXR_NS::GfMatrix4d& viewMatrix,
                   const PXR_NS::GfMatrix4d& projMatrix);
    usdcc::tools::Tool* activeToolInstance() const;

    // The HUD (docs/VIEWPORT_HUD.md §4): laid out, painted into m_hudImage when the layout
    // changed, and drawn as one textured quad over the default framebuffer.
    bool hudActive() const { return m_display.hud && !m_previewMode; }
    void drawHud(int width, int height);
    // After a frame: its time, for the performance line.
    void recordFrame(double milliseconds);
    void updateHover(const QPointF& position);
    void showRollover();

    QOpenGLContext* m_context = nullptr;
    QOpenGLFunctions_1_1* m_legacyGl = nullptr;
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> m_engine;
    // While a line style is in use (drawSelectionOverlay()); freed with the engine.
    std::unique_ptr<SelectionOverlay> m_selectionOverlay;
    PXR_NS::TfToken m_rendererPluginId;  // kept across releaseEngine()
    // What Storm and the gizmo draw into; copied to the window each frame.
    std::unique_ptr<QOpenGLFramebufferObject> m_framebuffer;
    PXR_NS::UsdStageRefPtr m_stage;

    PXR_NS::GfVec3d m_target{0.0, 0.0, 0.0};
    double m_distance = kDefaultOrbitDistance;
    double m_yaw = kDefaultOrbitYaw;
    double m_pitch = kDefaultOrbitPitch;
    double m_fovY = verticalFieldOfView();

    QPoint m_lastMousePos;
    bool m_orbiting = false;
    bool m_orbitReversed = false;  // this drag began upside down, so turns the other way
    bool m_panning = false;
    bool m_dragging = false;
    bool m_toolEditActive = false;  // between toolEditStarted() and toolEditFinished()
    bool m_firstFrameRendered = false;

    std::shared_ptr<usdcc::tools::ToolSet> m_toolSet;
    // Redraw triggers; dropped automatically when this window goes away.
    usdcc::core::Signal<>::Connection m_activeToolConnection;
    usdcc::core::Signal<>::Connection m_optionsConnection;
    std::vector<PXR_NS::SdfPath> m_selectedPaths;
    PXR_NS::TfToken m_targetOp;
    DisplayOptions m_display;
    bool m_previewMode = false;
    bool m_paused = false;

    HudLayout m_hudLayout;
    QImage m_hudImage;
    unsigned int m_hudTexture = 0;  // GL name; freed with the engine, made again when needed
    bool m_hudTextureStale = true;
    HudItem m_hudHover = HudItem::None;
    HudCounts m_hudCounts;
    QPointF m_cursorPos;
    QString m_rollover;
    QTimer* m_rolloverTimer = nullptr;  // the cursor has rested
    QTimer* m_idleTimer = nullptr;      // no frame for a while: the frame time goes
    QElapsedTimer m_clock;
    std::deque<qint64> m_frameTimes;  // when the last second's frames were drawn
    std::optional<double> m_shownFrameMs;
    std::optional<double> m_shownFps;
    qint64 m_shownAt = 0;
    bool m_converging = false;
    std::optional<double> m_percentDone;
};

}  // namespace usdcc::usd
