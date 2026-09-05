#pragma once

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/usd/common.h>

#include <QPoint>
#include <QWindow>

#include <memory>
#include <vector>

class QOpenGLContext;

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

signals:
    void rendererPluginsChanged();

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
    void renderNow();
    PXR_NS::GfVec3d cameraPosition() const;

    QOpenGLContext* m_context = nullptr;
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> m_engine;
    PXR_NS::UsdStageRefPtr m_stage;

    PXR_NS::GfVec3d m_target{0.0, 0.0, 0.0};
    double m_distance = 10.0;
    double m_yaw = 45.0;
    double m_pitch = -20.0;

    QPoint m_lastMousePos;
    bool m_orbiting = false;
    bool m_panning = false;
};

}  // namespace usdcc::usd
