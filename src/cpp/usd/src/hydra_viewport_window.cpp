#include "usdcc/usd/hydra_viewport_window.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <QExposeEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

namespace {
constexpr double kFovY = 45.0;
constexpr double kNearClip = 0.1;
constexpr double kFarClip = 10000.0;
constexpr double kMinDistance = 0.01;
}  // namespace

HydraViewportWindow::HydraViewportWindow(QWindow* parent) : QWindow(parent) {
    setSurfaceType(QWindow::OpenGLSurface);
}

HydraViewportWindow::~HydraViewportWindow() {
    if (m_context) {
        m_context->makeCurrent(this);
        m_engine.reset();
        m_context->doneCurrent();
    }
}

void HydraViewportWindow::setStage(const PXR_NS::UsdStageRefPtr& stage) {
    m_stage = stage;
    renderNow();
}

void HydraViewportWindow::ensureEngine() {
    if (!m_context) {
        m_context = new QOpenGLContext(this);
        m_context->setFormat(requestedFormat());
        m_context->create();
    }
    if (!m_engine) {
        m_context->makeCurrent(this);
        m_engine = std::make_unique<PXR_NS::UsdImagingGLEngine>();
        emit rendererPluginsChanged();
    }
}

std::vector<PXR_NS::TfToken> HydraViewportWindow::rendererPlugins() const {
    return m_engine ? m_engine->GetRendererPlugins() : std::vector<PXR_NS::TfToken>{};
}

QString HydraViewportWindow::rendererDisplayName(const PXR_NS::TfToken& pluginId) const {
    return m_engine ? QString::fromStdString(m_engine->GetRendererDisplayName(pluginId)) : QString();
}

PXR_NS::TfToken HydraViewportWindow::currentRendererPlugin() const {
    return m_engine ? m_engine->GetCurrentRendererId() : PXR_NS::TfToken();
}

void HydraViewportWindow::setRendererPlugin(const PXR_NS::TfToken& pluginId) {
    if (!m_engine || pluginId.IsEmpty()) {
        return;
    }
    m_context->makeCurrent(this);
    m_engine->SetRendererPlugin(pluginId);
    renderNow();
}

PXR_NS::GfVec3d HydraViewportWindow::cameraPosition() const {
    const double yawRad = PXR_NS::GfDegreesToRadians(m_yaw);
    const double pitchRad = PXR_NS::GfDegreesToRadians(m_pitch);
    const double x = m_distance * std::cos(pitchRad) * std::sin(yawRad);
    const double y = m_distance * std::sin(pitchRad);
    const double z = m_distance * std::cos(pitchRad) * std::cos(yawRad);
    return m_target + PXR_NS::GfVec3d(x, y, z);
}

void HydraViewportWindow::exposeEvent(QExposeEvent*) {
    if (isExposed()) {
        renderNow();
    }
}

void HydraViewportWindow::resizeEvent(QResizeEvent*) { renderNow(); }

bool HydraViewportWindow::event(QEvent* ev) {
    if (ev->type() == QEvent::UpdateRequest) {
        renderNow();
        return true;
    }
    return QWindow::event(ev);
}

void HydraViewportWindow::renderNow() {
    if (!isExposed()) {
        return;
    }

    ensureEngine();
    m_context->makeCurrent(this);

    const qreal pixelRatio = devicePixelRatio();
    const int w = std::max(1, static_cast<int>(width() * pixelRatio));
    const int h = std::max(1, static_cast<int>(height() * pixelRatio));

    // QOpenGLWidget sets this automatically from its FBO size before each
    // paintGL(); a raw QWindow + manually-created QOpenGLContext has no such
    // helper; a stale/zero GL_VIEWPORT wasn't automatically fixed up.
    //
    // HgiInteropOpenGL::CompositeToInterop (Hydra's step that blits its
    // internally-rendered color+depth onto whatever framebuffer is bound)
    // explicitly composites "over the application's framebuffer contents"
    // using premultiplied-alpha blending plus a GL_LEQUAL depth test against
    // *our* depth buffer — i.e. it assumes we already cleared color and
    // depth before calling Render(). Without this, the depth test compares
    // Hydra's real depth against whatever undefined memory the window's
    // fresh/reused depth buffer happened to contain, which can silently
    // reject the composite for exactly the pixels covered by geometry
    // (rendering pure black there) while unrelated background-only draws
    // still show through.
    if (auto* gl = m_context->functions()) {
        gl->glViewport(0, 0, w, h);
        gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        gl->glClearDepthf(1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    const PXR_NS::GfVec3d eye = cameraPosition();
    const PXR_NS::GfVec3d forward = (m_target - eye).GetNormalized();
    PXR_NS::GfVec3d right = PXR_NS::GfCross(forward, PXR_NS::GfVec3d(0.0, 1.0, 0.0));
    if (right.GetLength() < 1e-6) {
        right = PXR_NS::GfVec3d(1.0, 0.0, 0.0);
    } else {
        right.Normalize();
    }
    const PXR_NS::GfVec3d up = PXR_NS::GfCross(right, forward);

    PXR_NS::GfMatrix4d cameraToWorld(1.0);
    cameraToWorld.SetRow(0, PXR_NS::GfVec4d(right[0], right[1], right[2], 0.0));
    cameraToWorld.SetRow(1, PXR_NS::GfVec4d(up[0], up[1], up[2], 0.0));
    cameraToWorld.SetRow(2, PXR_NS::GfVec4d(-forward[0], -forward[1], -forward[2], 0.0));
    cameraToWorld.SetRow(3, PXR_NS::GfVec4d(eye[0], eye[1], eye[2], 1.0));
    const PXR_NS::GfMatrix4d viewMatrix = cameraToWorld.GetInverse();

    PXR_NS::GfFrustum frustum;
    frustum.SetPerspective(kFovY, /*isFovVertical=*/true, static_cast<double>(w) / static_cast<double>(h), kNearClip,
                            kFarClip);
    const PXR_NS::GfMatrix4d projMatrix = frustum.ComputeProjectionMatrix();

    m_engine->SetRenderBufferSize(PXR_NS::GfVec2i(w, h));
    m_engine->SetRenderViewport(PXR_NS::GfVec4d(0, 0, w, h));
    m_engine->SetCameraState(viewMatrix, projMatrix);

    PXR_NS::GlfSimpleLight cameraLight;
    cameraLight.SetPosition(PXR_NS::GfVec4f(static_cast<float>(eye[0]), static_cast<float>(eye[1]),
                                             static_cast<float>(eye[2]), 1.0f));
    PXR_NS::GlfSimpleLightVector lights{cameraLight};
    PXR_NS::GlfSimpleMaterial material;
    const PXR_NS::GfVec4f sceneAmbient(0.15f, 0.15f, 0.15f, 1.0f);
    m_engine->SetLightingState(lights, material, sceneAmbient);

    PXR_NS::UsdImagingGLRenderParams params;
    params.clearColor = PXR_NS::GfVec4f(0.24f, 0.24f, 0.26f, 1.0f);
    params.frame = PXR_NS::UsdTimeCode::Default();
    params.enableLighting = true;

    if (m_stage) {
        m_engine->Render(m_stage->GetPseudoRoot(), params);
    }

    m_context->swapBuffers(this);

    if (!m_engine->IsConverged()) {
        requestUpdate();
    }
}

void HydraViewportWindow::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
    if (event->button() == Qt::LeftButton) {
        m_orbiting = true;
    } else if (event->button() == Qt::MiddleButton) {
        m_panning = true;
    }
}

void HydraViewportWindow::mouseMoveEvent(QMouseEvent* event) {
    const QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();

    if (m_orbiting) {
        m_yaw -= delta.x() * 0.5;
        m_pitch = std::clamp(m_pitch - delta.y() * 0.5, -89.0, 89.0);
        renderNow();
    } else if (m_panning) {
        const PXR_NS::GfVec3d eye = cameraPosition();
        const PXR_NS::GfVec3d forward = (m_target - eye).GetNormalized();
        PXR_NS::GfVec3d right = PXR_NS::GfCross(forward, PXR_NS::GfVec3d(0.0, 1.0, 0.0));
        right.Normalize();
        const PXR_NS::GfVec3d up = PXR_NS::GfCross(right, forward);
        const double panScale = m_distance * 0.0015;
        m_target += right * (-delta.x() * panScale) + up * (delta.y() * panScale);
        renderNow();
    }
}

void HydraViewportWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_orbiting = false;
    }
    if (event->button() == Qt::MiddleButton) {
        m_panning = false;
    }
}

void HydraViewportWindow::wheelEvent(QWheelEvent* event) {
    const double factor = std::pow(0.9, event->angleDelta().y() / 120.0);
    m_distance = std::max(kMinDistance, m_distance * factor);
    renderNow();
}

}  // namespace usdcc::usd
