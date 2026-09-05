#pragma once

#include "usdcc/tools/tool.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QPoint>
#include <QWindow>

#include <array>
#include <memory>
#include <vector>

class QOpenGLContext;
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
class HydraViewportWindow : public QWindow {
    Q_OBJECT

public:
    explicit HydraViewportWindow(QWindow* parent = nullptr);
    ~HydraViewportWindow() override;

    void setStage(const PXR_NS::UsdStageRefPtr& stage);

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
        double distance = 10.0;
        double yaw = 45.0;
        double pitch = -20.0;
    };
    CameraState cameraState() const;
    void setCameraState(const CameraState& state);

    enum class ToolKind { Select, Move, Rotate, Scale };
    ToolKind activeTool() const { return m_activeToolKind; }
    void setActiveTool(ToolKind kind);

    // Called by ViewportViewPanel when StageManager's selection changes for
    // the stage this viewport is currently showing, so the gizmo tools and
    // the engine's highlight both stay in sync with the outliner/attributes
    // panels (see docs/PLAN.md milestone M4).
    void setSelectedPaths(std::vector<PXR_NS::SdfPath> paths);

signals:
    void rendererPluginsChanged();

    // Emitted when a tool (currently just SelectTool) changes the selection
    // itself, so ViewportViewPanel can push it into StageManager and keep
    // every other panel showing this stage in sync.
    void selectionRequested(std::vector<PXR_NS::SdfPath> paths);

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
    void renderNow();
    PXR_NS::GfVec3d cameraPosition() const;
    PXR_NS::GfFrustum currentFrustum() const;

    usdcc::tools::ToolContext buildToolContext();
    PXR_NS::GfVec2d ndcFromPixel(const QPointF& pixelPos) const;
    PXR_NS::SdfPath pickPrim(const PXR_NS::GfVec2d& ndcPos);
    void applyToolResult(const usdcc::tools::ToolResult& result);
    void updateEngineSelection();
    void drawGizmo(const usdcc::tools::GizmoGeometry& geo, const PXR_NS::GfMatrix4d& viewMatrix,
                   const PXR_NS::GfMatrix4d& projMatrix);
    usdcc::tools::Tool* activeToolInstance() const;

    QOpenGLContext* m_context = nullptr;
    QOpenGLFunctions_1_1* m_legacyGl = nullptr;
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> m_engine;
    PXR_NS::UsdStageRefPtr m_stage;

    PXR_NS::GfVec3d m_target{0.0, 0.0, 0.0};
    double m_distance = 10.0;
    double m_yaw = 45.0;
    double m_pitch = -20.0;

    QPoint m_lastMousePos;
    bool m_orbiting = false;
    bool m_panning = false;
    bool m_dragging = false;

    ToolKind m_activeToolKind = ToolKind::Select;
    std::array<std::unique_ptr<usdcc::tools::Tool>, 4> m_tools;
    std::vector<PXR_NS::SdfPath> m_selectedPaths;
};

}  // namespace usdcc::usd
