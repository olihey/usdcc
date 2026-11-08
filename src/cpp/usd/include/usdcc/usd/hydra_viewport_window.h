#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/orbit_camera.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QPoint>
#include <QWindow>

#include <memory>
#include <vector>

class QOpenGLContext;
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

    // Valid once the GL context has initialized; rendererPluginsChanged()
    // fires at that point.
    std::vector<PXR_NS::TfToken> rendererPlugins() const;
    QString rendererDisplayName(const PXR_NS::TfToken& pluginId) const;
    PXR_NS::TfToken currentRendererPlugin() const;
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
    // The vertical field of view frustum() uses, in degrees.
    static double verticalFieldOfView();

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

Q_SIGNALS:
    void rendererPluginsChanged();
    // setRendererPlugin() switched it.
    void rendererChanged();

    // Any mouse button pressed in the viewport. Its panel takes this as
    // focus: a native window's focus doesn't reach ADS's focus tracking.
    void pressed();

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
    void drawGizmo(const usdcc::tools::GizmoGeometry& geo, const PXR_NS::GfMatrix4d& viewMatrix,
                   const PXR_NS::GfMatrix4d& projMatrix);
    usdcc::tools::Tool* activeToolInstance() const;

    QOpenGLContext* m_context = nullptr;
    QOpenGLFunctions_1_1* m_legacyGl = nullptr;
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> m_engine;
    PXR_NS::TfToken m_rendererPluginId;  // kept across releaseEngine()
    // What Storm and the gizmo draw into; copied to the window each frame.
    std::unique_ptr<QOpenGLFramebufferObject> m_framebuffer;
    PXR_NS::UsdStageRefPtr m_stage;

    PXR_NS::GfVec3d m_target{0.0, 0.0, 0.0};
    double m_distance = kDefaultOrbitDistance;
    double m_yaw = kDefaultOrbitYaw;
    double m_pitch = kDefaultOrbitPitch;

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
};

}  // namespace usdcc::usd
