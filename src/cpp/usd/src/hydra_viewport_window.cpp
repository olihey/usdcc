#include "usdcc/usd/hydra_viewport_window.h"

#include "usdcc/tools/move_tool.h"
#include "usdcc/tools/rotate_tool.h"
#include "usdcc/tools/scale_tool.h"
#include "usdcc/tools/select_tool.h"

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/imaging/hdx/pickTask.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <QExposeEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions_1_1>
#include <QOpenGLVersionFunctionsFactory>
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
    m_tools[static_cast<size_t>(ToolKind::Select)] = std::make_unique<usdcc::tools::SelectTool>();
    m_tools[static_cast<size_t>(ToolKind::Move)] = std::make_unique<usdcc::tools::MoveTool>();
    m_tools[static_cast<size_t>(ToolKind::Rotate)] = std::make_unique<usdcc::tools::RotateTool>();
    m_tools[static_cast<size_t>(ToolKind::Scale)] = std::make_unique<usdcc::tools::ScaleTool>();
}

HydraViewportWindow::~HydraViewportWindow() {
    if (m_context) {
        m_context->makeCurrent(this);
        m_engine.reset();
        m_context->doneCurrent();
    }
}

void HydraViewportWindow::setStage(const PXR_NS::UsdStageRefPtr& stage) {
    if (stage == m_stage) {
        return;
    }
    m_stage = stage;

    // UsdImagingGLEngine::Render()/PrepareBatch() only populates its scene
    // index/delegate from a stage once (see the _isPopulated flag in
    // usdImaging/usdImagingGL/engine.cpp): the first Render() call for a
    // given engine instance calls Populate() on whichever stage owns that
    // call's root prim, and every later Render() call — even with a
    // completely different stage's root — skips population entirely and
    // keeps showing the first stage's content. The only public way to force
    // a fresh population is what SetRendererPlugin() does internally when
    // switching to a genuinely different plugin: tear down and rebuild the
    // whole engine. So that's what happens here whenever the stage actually
    // changes, preserving whichever renderer plugin was already selected.
    if (m_engine) {
        const PXR_NS::TfToken rendererPluginId = m_engine->GetCurrentRendererId();
        m_context->makeCurrent(this);
        m_engine = std::make_unique<PXR_NS::UsdImagingGLEngine>();
        m_engine->SetRendererPlugin(rendererPluginId);
        configureEngine();
    }

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
        configureEngine();

        // Legacy (fixed-function) GL, used only to draw M5's gizmo overlays
        // via simple immediate-mode glBegin/glVertex calls — Storm itself
        // renders through modern, shader-based GL, so this doesn't need to
        // integrate with its pipeline at all, just draw into the same
        // already-cleared/composited framebuffer afterward. Requires the
        // CompatibilityProfile context M3 already established for Storm's
        // own sake (see docs/PLAN.md §6 item 12). Tied to the GL context
        // (not the engine), so unlike configureEngine() this only ever runs
        // once per window, not every time the engine gets recreated.
        m_legacyGl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_1_1>(m_context);
        if (m_legacyGl) {
            m_legacyGl->initializeOpenGLFunctions();
        }

        emit rendererPluginsChanged();
    }
}

void HydraViewportWindow::configureEngine() {
    m_engine->SetSelectionColor(PXR_NS::GfVec4f(1.0f, 0.65f, 0.0f, 1.0f));
    updateEngineSelection();
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

HydraViewportWindow::CameraState HydraViewportWindow::cameraState() const {
    return {m_target, m_distance, m_yaw, m_pitch};
}

void HydraViewportWindow::setCameraState(const CameraState& state) {
    m_target = state.target;
    m_distance = state.distance;
    m_yaw = state.yaw;
    m_pitch = state.pitch;
    renderNow();
}

void HydraViewportWindow::setActiveTool(ToolKind kind) {
    if (usdcc::tools::Tool* tool = activeToolInstance()) {
        tool->mouseRelease();
    }
    m_activeToolKind = kind;
    m_dragging = false;
    renderNow();
}

void HydraViewportWindow::setSelectedPaths(std::vector<PXR_NS::SdfPath> paths) {
    m_selectedPaths = std::move(paths);
    updateEngineSelection();
    renderNow();
}

void HydraViewportWindow::updateEngineSelection() {
    if (m_engine) {
        m_engine->SetSelected(m_selectedPaths);
    }
}

usdcc::tools::Tool* HydraViewportWindow::activeToolInstance() const {
    return m_tools[static_cast<size_t>(m_activeToolKind)].get();
}

PXR_NS::GfVec3d HydraViewportWindow::cameraPosition() const {
    const double yawRad = PXR_NS::GfDegreesToRadians(m_yaw);
    const double pitchRad = PXR_NS::GfDegreesToRadians(m_pitch);
    const double x = m_distance * std::cos(pitchRad) * std::sin(yawRad);
    const double y = m_distance * std::sin(pitchRad);
    const double z = m_distance * std::cos(pitchRad) * std::cos(yawRad);
    return m_target + PXR_NS::GfVec3d(x, y, z);
}

PXR_NS::GfFrustum HydraViewportWindow::currentFrustum() const {
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

    const qreal pixelRatio = devicePixelRatio();
    const int w = std::max(1, static_cast<int>(width() * pixelRatio));
    const int h = std::max(1, static_cast<int>(height() * pixelRatio));

    PXR_NS::GfFrustum frustum;
    frustum.SetPositionAndRotationFromMatrix(cameraToWorld);
    frustum.SetPerspective(kFovY, /*isFovVertical=*/true, static_cast<double>(w) / static_cast<double>(h), kNearClip,
                            kFarClip);
    return frustum;
}

PXR_NS::GfVec2d HydraViewportWindow::ndcFromPixel(const QPointF& pixelPos) const {
    const double w = std::max(1, width());
    const double h = std::max(1, height());
    const double x = 2.0 * pixelPos.x() / w - 1.0;
    const double y = 1.0 - 2.0 * pixelPos.y() / h;
    return PXR_NS::GfVec2d(x, y);
}

PXR_NS::SdfPath HydraViewportWindow::pickPrim(const PXR_NS::GfVec2d& ndcPos) {
    if (!m_engine || !m_stage) {
        return {};
    }
    m_context->makeCurrent(this);

    const qreal pixelRatio = devicePixelRatio();
    const int w = std::max(1, static_cast<int>(width() * pixelRatio));
    const int h = std::max(1, static_cast<int>(height() * pixelRatio));
    const PXR_NS::GfFrustum pickFrustum =
        currentFrustum().ComputeNarrowedFrustum(ndcPos, PXR_NS::GfVec2d(4.0 / w, 4.0 / h));

    PXR_NS::UsdImagingGLRenderParams params;
    params.frame = PXR_NS::UsdTimeCode::Default();

    PXR_NS::UsdImagingGLEngine::PickParams pickParams;
    pickParams.resolveMode = PXR_NS::HdxPickResolveModeTokens->resolveNearestToCamera;

    PXR_NS::UsdImagingGLEngine::IntersectionResultVector results;
    const bool hit = m_engine->TestIntersection(pickParams, pickFrustum.ComputeViewMatrix(),
                                                 pickFrustum.ComputeProjectionMatrix(), m_stage->GetPseudoRoot(),
                                                 params, &results);
    if (!hit || results.empty()) {
        return {};
    }
    return results.front().hitPrimPath;
}

usdcc::tools::ToolContext HydraViewportWindow::buildToolContext() {
    usdcc::tools::ToolContext ctx;
    ctx.stage = m_stage;
    ctx.selection = m_selectedPaths;
    const PXR_NS::GfFrustum frustum = currentFrustum();
    ctx.viewMatrix = frustum.ComputeViewMatrix();
    ctx.projMatrix = frustum.ComputeProjectionMatrix();
    ctx.cameraPos = frustum.GetPosition();
    ctx.pickPrim = [this](const PXR_NS::GfVec2d& ndcPos) { return pickPrim(ndcPos); };
    return ctx;
}

void HydraViewportWindow::applyToolResult(const usdcc::tools::ToolResult& result) {
    if (result.newSelection) {
        m_selectedPaths = *result.newSelection;
        updateEngineSelection();
        emit selectionRequested(m_selectedPaths);
    }
}

void HydraViewportWindow::drawGizmo(const usdcc::tools::GizmoGeometry& geo, const PXR_NS::GfMatrix4d& viewMatrix,
                                     const PXR_NS::GfMatrix4d& projMatrix) {
    if (!m_legacyGl || (geo.lines.empty() && geo.triangles.empty())) {
        return;
    }

    // GfMatrix4d is row-major, row-vector convention (p' = p * M); reading
    // its raw array as OpenGL's column-major, column-vector convention
    // (v' = M * v) is exactly the transpose of that — which represents the
    // identical transform, so no manual transpose is needed here.
    m_legacyGl->glMatrixMode(GL_PROJECTION);
    m_legacyGl->glLoadMatrixd(projMatrix.GetArray());
    m_legacyGl->glMatrixMode(GL_MODELVIEW);
    m_legacyGl->glLoadMatrixd(viewMatrix.GetArray());

    m_legacyGl->glDisable(GL_DEPTH_TEST);
    m_legacyGl->glLineWidth(2.5f);

    m_legacyGl->glBegin(GL_LINES);
    for (const auto& line : geo.lines) {
        m_legacyGl->glColor4f(line.color[0], line.color[1], line.color[2], line.color[3]);
        m_legacyGl->glVertex3d(line.a[0], line.a[1], line.a[2]);
        m_legacyGl->glVertex3d(line.b[0], line.b[1], line.b[2]);
    }
    m_legacyGl->glEnd();

    m_legacyGl->glBegin(GL_TRIANGLES);
    for (const auto& tri : geo.triangles) {
        m_legacyGl->glColor4f(tri.color[0], tri.color[1], tri.color[2], tri.color[3]);
        m_legacyGl->glVertex3d(tri.a[0], tri.a[1], tri.a[2]);
        m_legacyGl->glVertex3d(tri.b[0], tri.b[1], tri.b[2]);
        m_legacyGl->glVertex3d(tri.c[0], tri.c[1], tri.c[2]);
    }
    m_legacyGl->glEnd();

    m_legacyGl->glEnable(GL_DEPTH_TEST);
    m_legacyGl->glMatrixMode(GL_MODELVIEW);
    m_legacyGl->glLoadIdentity();
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

    const usdcc::tools::ToolContext toolCtx = buildToolContext();

    m_engine->SetRenderBufferSize(PXR_NS::GfVec2i(w, h));
    m_engine->SetRenderViewport(PXR_NS::GfVec4d(0, 0, w, h));
    m_engine->SetCameraState(toolCtx.viewMatrix, toolCtx.projMatrix);

    PXR_NS::GlfSimpleLight cameraLight;
    cameraLight.SetPosition(PXR_NS::GfVec4f(static_cast<float>(toolCtx.cameraPos[0]),
                                             static_cast<float>(toolCtx.cameraPos[1]),
                                             static_cast<float>(toolCtx.cameraPos[2]), 1.0f));
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

    if (usdcc::tools::Tool* tool = activeToolInstance()) {
        drawGizmo(tool->buildGizmo(toolCtx), toolCtx.viewMatrix, toolCtx.projMatrix);
    }

    m_context->swapBuffers(this);

    if (!m_engine->IsConverged()) {
        requestUpdate();
    }
}

void HydraViewportWindow::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();

    if (event->button() == Qt::LeftButton) {
        // Alt+drag is the camera-navigation override: it must always orbit,
        // never reach the active tool. Without this, SelectTool::mousePress()
        // (which always reports handled=true, even on empty space, since a
        // click always resolves to "select this" or "select nothing") would
        // permanently latch m_dragging and never fall through to orbiting —
        // unlike Move/Rotate/Scale, whose mousePress() only reports handled
        // when a gizmo handle is actually under the cursor.
        if (!(event->modifiers() & Qt::AltModifier)) {
            if (usdcc::tools::Tool* tool = activeToolInstance()) {
                const usdcc::tools::ToolContext ctx = buildToolContext();
                const PXR_NS::GfVec2d ndc = ndcFromPixel(event->position());
                const PXR_NS::GfRay ray = currentFrustum().ComputePickRay(ndc);
                const usdcc::tools::ToolResult result = tool->mousePress(ctx, ndc, ray);
                applyToolResult(result);
                if (result.handled) {
                    m_dragging = true;
                    renderNow();
                    return;
                }
            }
        }
        m_orbiting = true;
    } else if (event->button() == Qt::MiddleButton) {
        m_panning = true;
    }
}

void HydraViewportWindow::mouseMoveEvent(QMouseEvent* event) {
    const QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();

    if (m_dragging) {
        if (usdcc::tools::Tool* tool = activeToolInstance()) {
            const usdcc::tools::ToolContext ctx = buildToolContext();
            const PXR_NS::GfVec2d ndc = ndcFromPixel(event->position());
            const PXR_NS::GfRay ray = currentFrustum().ComputePickRay(ndc);
            applyToolResult(tool->mouseMove(ctx, ndc, ray));
        }
        renderNow();
    } else if (m_orbiting) {
        m_yaw -= delta.x() * 0.5;
        m_pitch = std::clamp(m_pitch + delta.y() * 0.5, -89.0, 89.0);
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
        if (m_dragging) {
            if (usdcc::tools::Tool* tool = activeToolInstance()) {
                tool->mouseRelease();
            }
            m_dragging = false;
            renderNow();
        }
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
