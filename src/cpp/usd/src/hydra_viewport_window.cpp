#include "usdcc/usd/hydra_viewport_window.h"

#include "usdcc/usd/orbit_camera.h"
#include "usdcc/usd/viewport_grid.h"

#include <QCursor>
#include <QRect>
#include <QScreen>

#include "usdcc/core/log.h"

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/tf/stackTrace.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/imaging/hdx/pickTask.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <QExposeEvent>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QOpenGLContext>
#include <QOpenGLDebugLogger>
#include <QOpenGLFramebufferObject>
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
constexpr double kMinDistance = 0.01;

// The grid's colours: light lines on a dark background, dark ones on a light one, so the
// grid shows whatever the background color is. The axes take the gizmo's axis colours.
struct GridColors {
    PXR_NS::GfVec4f minor, major, axisX, axisOther;
};
GridColors gridColors(const QColor& background, bool zUp) {
    const float ink = background.lightnessF() < 0.5f ? 1.0f : 0.0f;
    const PXR_NS::GfVec4f x(0.9f, 0.3f, 0.3f, 0.8f), y(0.4f, 0.8f, 0.3f, 0.8f), z(0.3f, 0.5f, 0.95f, 0.8f);
    return {PXR_NS::GfVec4f(ink, ink, ink, 0.12f), PXR_NS::GfVec4f(ink, ink, ink, 0.28f), x, zUp ? y : z};
}
}  // namespace

HydraViewportWindow::HydraViewportWindow(QWindow* parent) : QWindow(parent) {
    setSurfaceType(QWindow::OpenGLSurface);
    // Until a shared set is installed (see setToolSet()), use a private one so
    // the viewport always has working tools.
    setToolSet(std::make_shared<usdcc::tools::ToolSet>());
}

HydraViewportWindow::~HydraViewportWindow() {
    // Normally already done when the surface went (see event()); this covers
    // a window whose surface outlives it.
    releaseEngine();
}

void HydraViewportWindow::releaseEngine() {
    if (!m_engine) {
        return;
    }
    // The engine frees its GL objects (buffers, shaders, textures) as it goes,
    // which needs the context current on a surface that still exists.
    // Without one every free fails and Hydra logs a GL error for each.
    const bool current = m_context && m_context->makeCurrent(this);
    if (!current) {
        USDCC_LOG_WARNING("usdcc.viewport", "Releasing the Hydra engine without a current GL context");
    }
    m_rendererPluginId = m_engine->GetCurrentRendererId();
    m_engine.reset();
    m_framebuffer.reset();
    if (current) {
        m_context->doneCurrent();
    }
}

void HydraViewportWindow::setStage(const PXR_NS::UsdStageRefPtr& stage) {
    if (stage == m_stage) {
        return;
    }
    m_stage = stage;
    m_firstFrameRendered = false;  // firstFrameRendered() is per stage

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
        m_context->makeCurrent(this);
        const QSurfaceFormat format = m_context->format();
        const auto glString = [this](GLenum name) {
            const GLubyte* text = m_context->functions()->glGetString(name);
            return text ? reinterpret_cast<const char*>(text) : "?";
        };
        USDCC_LOG_INFO("usdcc.viewport", "OpenGL {}.{} {} profile{}, {} ({})", format.majorVersion(),
                       format.minorVersion(),
                       format.profile() == QSurfaceFormat::CompatibilityProfile ? "compatibility"
                       : format.profile() == QSurfaceFormat::CoreProfile        ? "core"
                                                                                : "no",
                       format.testOption(QSurfaceFormat::DebugContext) ? ", debug" : "",
                       glString(GL_RENDERER), glString(GL_VERSION));
        if (format.testOption(QSurfaceFormat::DebugContext)) {
            startGlDebugLog();
        }
    }
    if (!m_engine) {
        m_context->makeCurrent(this);
        m_engine = std::make_unique<PXR_NS::UsdImagingGLEngine>();
        if (!m_rendererPluginId.IsEmpty()) {
            m_engine->SetRendererPlugin(m_rendererPluginId);  // the one in use before releaseEngine()
        }
        configureEngine();
        USDCC_LOG_INFO("usdcc.viewport", "Viewport renderer: {}",
                       m_engine->GetRendererDisplayName(m_engine->GetCurrentRendererId()));

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

        Q_EMIT rendererPluginsChanged();
    }
}

QImage HydraViewportWindow::grabFrame() {
    if (!m_context || !m_framebuffer || !m_firstFrameRendered || !m_context->makeCurrent(this)) return {};
    QImage image = m_framebuffer->toImage();
    m_context->doneCurrent();
    return image;
}

void HydraViewportWindow::bindFramebuffer(int width, int height) {
    if (!m_framebuffer || m_framebuffer->size() != QSize(width, height)) {
        m_framebuffer.reset();  // before its replacement exists: one at a time in GPU memory
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);  // Storm composites with a depth test
        format.setInternalTextureFormat(GL_RGBA8);
        m_framebuffer = std::make_unique<QOpenGLFramebufferObject>(QSize(width, height), format);
    }
    m_framebuffer->bind();
}

void HydraViewportWindow::startGlDebugLog() {
    auto* logger = new QOpenGLDebugLogger(this);
    if (!logger->initialize()) {  // needs the context current, and KHR_debug
        USDCC_LOG_WARNING("usdcc.viewport", "GL debug output unavailable (no KHR_debug)");
        delete logger;
        return;
    }
    // Synchronous: the driver reports during the failing call, so the stack
    // shows who made it (Hydra's own checks only see the error later).
    connect(logger, &QOpenGLDebugLogger::messageLogged, this, [](const QOpenGLDebugMessage& message) {
        if (message.type() != QOpenGLDebugMessage::ErrorType) {
            return;
        }
        usdcc::core::LogRecord record;
        record.level = usdcc::core::LogLevel::Error;
        record.source = "usdcc";
        record.category = "usdcc.viewport.gl";
        record.message = message.message().toStdString();
        record.details = PXR_NS::TfGetStackTrace();
        usdcc::core::LogHub::instance().post(std::move(record));
    });
    logger->startLogging(QOpenGLDebugLogger::SynchronousLogging);
    USDCC_LOG_INFO("usdcc.viewport", "GL debug output on (USDCC_GL_DEBUG)");
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
    if (m_engine->SetRendererPlugin(pluginId)) {
        USDCC_LOG_INFO("usdcc.viewport", "Viewport renderer: {}", m_engine->GetRendererDisplayName(pluginId));
        Q_EMIT rendererChanged();
    } else {
        USDCC_LOG_WARNING("usdcc.viewport", "Could not switch the viewport to renderer {}", pluginId.GetString());
    }
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
    cameraMoved();
}

void HydraViewportWindow::frameBox(const PXR_NS::GfRange3d& box) {
    const double aspect = static_cast<double>(std::max(1, width())) / static_cast<double>(std::max(1, height()));
    const OrbitFraming framing = frameOrbitCamera(box, kFovY, aspect, m_distance);
    m_target = framing.target;
    m_distance = std::max(kMinDistance, framing.distance);
    cameraMoved();
}

double HydraViewportWindow::verticalFieldOfView() { return kFovY; }

void HydraViewportWindow::cameraMoved() {
    renderNow();
    Q_EMIT cameraChanged();
}

void HydraViewportWindow::setDisplayOptions(const DisplayOptions& options) {
    if (options == m_display) return;
    m_display = options;
    renderNow();
}

void HydraViewportWindow::setToolSet(std::shared_ptr<usdcc::tools::ToolSet> toolSet) {
    m_toolSet = std::move(toolSet);
    m_dragging = false;
    // The active tool's gizmo (and how it looks, e.g. Move's Space) is drawn by
    // this viewport, so redraw whenever the shared tools change. The
    // Connections drop out automatically when this window is destroyed.
    m_activeToolConnection = m_toolSet->activeToolChanged.connect([this]() {
        m_dragging = false;
        finishToolEdit();
        renderNow();
    });
    m_optionsConnection = m_toolSet->optionsChanged.connect([this]() { renderNow(); });
    renderNow();
}

void HydraViewportWindow::setSelectedPaths(std::vector<PXR_NS::SdfPath> paths) {
    m_selectedPaths = std::move(paths);
    updateEngineSelection();
    renderNow();
}

void HydraViewportWindow::setTargetXformOp(const PXR_NS::TfToken& opName) {
    if (opName == m_targetOp) {
        return;
    }
    m_targetOp = opName;
    renderNow();  // the gizmo moves to (or greys out for) the new target
}

void HydraViewportWindow::updateEngineSelection() {
    if (m_engine) {
        m_engine->SetSelected(m_selectedPaths);
    }
}

usdcc::tools::Tool* HydraViewportWindow::activeToolInstance() const { return &m_toolSet->activeTool(); }

PXR_NS::GfVec3d HydraViewportWindow::cameraPosition() const {
    return orbitCameraAxes(m_target, m_distance, m_yaw, m_pitch).eye;
}

PXR_NS::GfFrustum HydraViewportWindow::currentFrustum() const {
    const auto [eye, forward, right, up] = orbitCameraAxes(m_target, m_distance, m_yaw, m_pitch);

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
    const OrbitClipping clipping = orbitClipping(m_distance);
    frustum.SetPerspective(kFovY, /*isFovVertical=*/true, static_cast<double>(w) / static_cast<double>(h),
                           clipping.nearClip, clipping.farClip);
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
    bindFramebuffer(w, h);  // as for rendering, see renderNow()
    const bool hit = m_engine->TestIntersection(pickParams, pickFrustum.ComputeViewMatrix(),
                                                 pickFrustum.ComputeProjectionMatrix(), m_stage->GetPseudoRoot(),
                                                 params, &results);
    QOpenGLFramebufferObject::bindDefault();
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
    ctx.targetOpName = m_targetOp;
    return ctx;
}

void HydraViewportWindow::applyToolResult(const usdcc::tools::ToolResult& result) {
    if (result.newSelection) {
        m_selectedPaths = *result.newSelection;
        updateEngineSelection();
        Q_EMIT selectionRequested(m_selectedPaths);
    }
}

void HydraViewportWindow::finishToolEdit() {
    if (m_toolEditActive) {
        m_toolEditActive = false;
        Q_EMIT toolEditFinished();
    }
}

void HydraViewportWindow::drawGrid(const PXR_NS::GfMatrix4d& viewMatrix, const PXR_NS::GfMatrix4d& projMatrix) {
    if (!m_legacyGl || !m_display.showGrid) return;
    // On the stage's ground: XY for a Z-up stage. The camera itself stays Y-up.
    const bool zUp = m_stage && PXR_NS::UsdGeomGetStageUpAxis(m_stage) == PXR_NS::UsdGeomTokens->z;
    const std::vector<GridLine> lines = buildViewportGrid(m_display.gridSize, m_display.gridStep, zUp);
    if (lines.empty()) return;
    const GridColors colors = gridColors(m_display.background, zUp);

    // As drawGizmo() explains, GfMatrix4d's array is already what GL expects.
    m_legacyGl->glMatrixMode(GL_PROJECTION);
    m_legacyGl->glLoadMatrixd(projMatrix.GetArray());
    m_legacyGl->glMatrixMode(GL_MODELVIEW);
    m_legacyGl->glLoadMatrixd(viewMatrix.GetArray());

    // Behind the stage's geometry: Hydra's composite left its depth in our framebuffer
    // (HgiInteropOpenGL writes it), so a depth test hides the grid where something is
    // in front of it. The grid itself writes no depth, so its lines don't fight.
    m_legacyGl->glEnable(GL_DEPTH_TEST);
    m_legacyGl->glDepthFunc(GL_LEQUAL);
    m_legacyGl->glDepthMask(GL_FALSE);
    m_legacyGl->glEnable(GL_BLEND);
    m_legacyGl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_legacyGl->glLineWidth(1.0f);

    m_legacyGl->glBegin(GL_LINES);
    for (const GridLine& line : lines) {
        const PXR_NS::GfVec4f& c = line.kind == GridLineKind::Minor   ? colors.minor
                                   : line.kind == GridLineKind::Major ? colors.major
                                   : line.kind == GridLineKind::AxisX ? colors.axisX
                                                                       : colors.axisOther;
        m_legacyGl->glColor4f(c[0], c[1], c[2], c[3]);
        m_legacyGl->glVertex3d(line.a[0], line.a[1], line.a[2]);
        m_legacyGl->glVertex3d(line.b[0], line.b[1], line.b[2]);
    }
    m_legacyGl->glEnd();

    m_legacyGl->glDisable(GL_BLEND);
    m_legacyGl->glDepthMask(GL_TRUE);
    m_legacyGl->glDepthFunc(GL_LESS);
    m_legacyGl->glMatrixMode(GL_MODELVIEW);
    m_legacyGl->glLoadIdentity();
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

void HydraViewportWindow::resizeEvent(QResizeEvent*) { cameraMoved(); }  // the aspect ratio

bool HydraViewportWindow::event(QEvent* ev) {
    if (ev->type() == QEvent::UpdateRequest) {
        renderNow();
        return true;
    }
    // The native surface goes before this object does (the window container
    // and the main window destroy it on close), and with it the last chance to
    // make the context current: release the engine now. It is recreated, with
    // the same renderer, if the window gets a surface and is shown again.
    if (ev->type() == QEvent::PlatformSurface &&
        static_cast<QPlatformSurfaceEvent*>(ev)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
        if (m_engine) {
            USDCC_LOG_DEBUG("usdcc.viewport", "Native surface going away: releasing the Hydra engine");
        }
        releaseEngine();
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

    // Storm draws into our own framebuffer, not the window's default one: some
    // of its passes ask the bound framebuffer about GL_COLOR_ATTACHMENT0 (the
    // transparency resolve sizes its buffers that way, HdxOitResolveTask),
    // which is GL_INVALID_ENUM on the default framebuffer. Hydra then reports
    // that leftover error from unrelated places. Copied to the window below.
    bindFramebuffer(w, h);

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
        gl->glClearColor(static_cast<float>(m_display.background.redF()), static_cast<float>(m_display.background.greenF()),
                         static_cast<float>(m_display.background.blueF()), 1.0f);
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
    params.clearColor = PXR_NS::GfVec4f(static_cast<float>(m_display.background.redF()),
                                        static_cast<float>(m_display.background.greenF()),
                                        static_cast<float>(m_display.background.blueF()), 1.0f);
    params.frame = PXR_NS::UsdTimeCode::Default();
    params.enableLighting = true;
    // SetSelected() only feeds the engine the selection; it is drawn solely
    // when the render params ask for it (the flag defaults to off).
    params.highlight = true;

    if (m_stage) {
        m_engine->Render(m_stage->GetPseudoRoot(), params);
    }

    drawGrid(toolCtx.viewMatrix, toolCtx.projMatrix);

    if (usdcc::tools::Tool* tool = activeToolInstance()) {
        drawGizmo(tool->buildGizmo(toolCtx), toolCtx.viewMatrix, toolCtx.projMatrix);
    }

    const QRect frame(0, 0, w, h);
    QOpenGLFramebufferObject::blitFramebuffer(nullptr, frame, m_framebuffer.get(), frame);
    QOpenGLFramebufferObject::bindDefault();
    m_context->swapBuffers(this);

    if (m_stage && !m_firstFrameRendered) {
        m_firstFrameRendered = true;
        Q_EMIT firstFrameRendered();
    }

    if (!m_engine->IsConverged()) {
        requestUpdate();
    }
}

void HydraViewportWindow::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
    Q_EMIT pressed();

    if (event->button() == Qt::LeftButton) {
        // Alt+drag is the camera-navigation override: it must always orbit,
        // never reach the active tool. Without this, a tool's mousePress()
        // would always claim the click (SelectTool reports handled=true even
        // on empty space, since a click always resolves to "select this" or
        // "select nothing", and Move/Rotate/Scale are SelectTools that fall
        // back to that when no gizmo handle is under the cursor), so
        // m_dragging would latch and the view could never be orbited.
        if (!(event->modifiers() & Qt::AltModifier)) {
            if (usdcc::tools::Tool* tool = activeToolInstance()) {
                const usdcc::tools::ToolContext ctx = buildToolContext();
                const PXR_NS::GfVec2d ndc = ndcFromPixel(event->position());
                const PXR_NS::GfRay ray = currentFrustum().ComputePickRay(ndc);
                const usdcc::tools::ToolResult result = tool->mousePress(ctx, ndc, ray);
                applyToolResult(result);
                if (result.startsEdit && !m_toolEditActive) {
                    m_toolEditActive = true;
                    Q_EMIT toolEditStarted(QString::fromStdString(tool->name()));
                }
                if (result.handled) {
                    m_dragging = true;
                    renderNow();
                    return;
                }
            }
        }
        m_orbiting = true;
        // For the whole drag, so that crossing a pole during it doesn't reverse it.
        m_orbitReversed = orbitIsUpsideDown(m_pitch);
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
        // Moving the cursor can leave an event from before it behind, which isn't the
        // mouse moving half a screen.
        const QScreen* onScreen = screen();
        const QRect bounds = onScreen ? onScreen->geometry() : QRect();
        const bool stale = onScreen && (std::abs(delta.x()) > bounds.width() / 2 || std::abs(delta.y()) > bounds.height() / 2);
        if (!stale) {
            m_yaw = wrapDegrees(m_yaw - (m_orbitReversed ? -1.0 : 1.0) * delta.x() * 0.5);
            m_pitch = wrapDegrees(m_pitch + delta.y() * 0.5);
        }
        // At the screen's edge, on from the other side, so a drag never runs out.
        if (onScreen) {
            const QPoint global = event->globalPosition().toPoint();
            if (const auto wrapped = wrapCursorAtEdges(global.x(), global.y(), bounds.left(), bounds.top(), bounds.right(), bounds.bottom())) {
                const QPoint to(wrapped->x, wrapped->y);
                QCursor::setPos(screen(), to);
                m_lastMousePos = mapFromGlobal(to);
            }
        }
        cameraMoved();
    } else if (m_panning) {
        const auto [eye, forward, right, up] = orbitCameraAxes(m_target, m_distance, m_yaw, m_pitch);
        const double panScale = m_distance * 0.0015;
        m_target += right * (-delta.x() * panScale) + up * (delta.y() * panScale);
        cameraMoved();
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
            finishToolEdit();
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
    cameraMoved();
}

}  // namespace usdcc::usd
