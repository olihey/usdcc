#include "usdcc/usd/hydra_viewport_window.h"

#include "selection_overlay.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/orbit_camera.h"
#include "usdcc/usd/viewport_display.h"
#include "usdcc/usd/viewport_grid.h"
#include "usdcc/usd/viewport_render_params.h"

#include <QCursor>
#include <QGuiApplication>
#include <QOpenGLExtraFunctions>
#include <QPainter>
#include <QRect>
#include <QScreen>
#include <QTimer>

#include "usdcc/core/log.h"

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/tf/stackTrace.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/imaging/hd/tokens.h>
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
// Hidden Line's depth pass is drawn this much farther, in NDC depth (-1 to 1), so the
// wires in front of a surface win over it.
constexpr double kHiddenLineDepthBias = 0.0005;

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

    m_clock.start();
    // The HUD's prim under the cursor, once it has rested there (docs/VIEWPORT_HUD.md §2.5).
    m_rolloverTimer = new QTimer(this);
    m_rolloverTimer->setSingleShot(true);
    m_rolloverTimer->setInterval(400);
    connect(m_rolloverTimer, &QTimer::timeout, this, &HydraViewportWindow::showRollover);
    // The frame time only means something while frames keep coming.
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    m_idleTimer->setInterval(1500);
    connect(m_idleTimer, &QTimer::timeout, this, [this]() {
        m_frameTimes.clear();
        if (m_shownFrameMs || m_shownFps) {
            m_shownFrameMs.reset();
            m_shownFps.reset();
            refreshHud();
        }
    });
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
    m_selectionOverlay.reset();
    m_engine.reset();
    forgetRendererHalt();
    m_framebuffer.reset();
    if (current && m_hudTexture) m_context->functions()->glDeleteTextures(1, &m_hudTexture);
    m_hudTexture = 0;
    m_hudTextureStale = true;
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
    m_engineFailures = 0;

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
    if (m_engine) rebuildEngine();

    renderNow();
}

void HydraViewportWindow::rebuildEngine() {
    const PXR_NS::TfToken rendererPluginId = m_engine->GetCurrentRendererId();
    m_context->makeCurrent(this);
    m_engine = std::make_unique<PXR_NS::UsdImagingGLEngine>();
    forgetRendererHalt();
    m_engine->SetRendererPlugin(rendererPluginId);
    configureEngine();
}

// OpenUSD's material cache (HdsiLocatorCachingSceneIndex, which UsdImaging puts after its
// stage scene index) keys a shader by its path below the Material, "NodeGraph/tex", but a
// change to a shader clears the entry named after the shader alone, "tex"
// (UsdImagingMaterialAdapter::InvalidateImagingSubprimFromDescendent). Inside a NodeGraph the
// two differ, so a shader removed and made again at the same path, as Undo and Redo of a
// group do, stays cached with its old, expired prim. Storm reads it on the next sync and
// UsdShade throws (tests/shade_imaging_test.cpp). A new engine has no cache, so it reads the
// stage as it is. Done again for each failure, but asks for another frame only after the
// first in a row: an engine that fails straight after a fresh start waits for the next edit
// or camera move rather than spinning.
void HydraViewportWindow::recoverFromEngineError(const char* during, const std::exception& error) {
    ++m_engineFailures;
    if (m_engineFailures == 1) {
        USDCC_LOG_WARNING("usdcc.viewport", "Hydra failed while {}: {}. Starting it afresh.", during, error.what());
    } else {
        USDCC_LOG_DEBUG("usdcc.viewport", "Hydra failed while {} again: {}", during, error.what());
    }
    rebuildEngine();
    if (m_engineFailures == 1) requestUpdate();
}

void HydraViewportWindow::ensureEngine() {
    if (!m_context) {
        m_context = new QOpenGLContext(this);
        m_context->setFormat(requestedFormat());
        if (!m_context->create() || !m_context->makeCurrent(this)) {
            // No GL here (a platform without it, such as tests' offscreen one): draw nothing.
            USDCC_LOG_WARNING("usdcc.viewport", "No OpenGL context for a viewport: it stays empty");
            return;
        }
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
        if (!m_context->isValid() || !m_context->makeCurrent(this)) return;
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
    const QColor& selection = m_display.selectionColor;
    m_engine->SetSelectionColor(PXR_NS::GfVec4f(static_cast<float>(selection.redF()), static_cast<float>(selection.greenF()),
                                                static_cast<float>(selection.blueF()), 1.0f));
    m_engine->SetRendererSetting(PXR_NS::HdRenderSettingsTokens->domeLightCameraVisibility,
                                 PXR_NS::VtValue(m_display.domeLightTextures));
    applyRendererSettings();
    updateEngineSelection();
}

void HydraViewportWindow::setRendererSettings(const PXR_NS::TfToken& pluginId, const RendererSettingValues& values) {
    if (pluginId == m_rendererSettingsPlugin && values == m_rendererSettings) return;
    m_rendererSettingsPlugin = pluginId;
    m_rendererSettings = values;
    applyRendererSettings();
}

void HydraViewportWindow::applyRendererSettings() {
    if (!m_engine || m_rendererSettingsPlugin != m_engine->GetCurrentRendererId()) return;
    bool changed = false;
    for (const auto& [key, value] : m_rendererSettings) {
        if (m_engine->GetRendererSetting(key) == value) continue;
        m_engine->SetRendererSetting(key, value);
        changed = true;
    }
    // Not renderNow(): this also runs inside it, from ensureEngine()'s configureEngine().
    if (changed) requestUpdate();
}

PXR_NS::HdCommandDescriptors HydraViewportWindow::rendererCommands() const {
    return m_engine ? m_engine->GetRendererCommandDescriptors() : PXR_NS::HdCommandDescriptors{};
}

bool HydraViewportWindow::runRendererCommand(const PXR_NS::TfToken& name) {
    if (!m_engine) return false;
    m_context->makeCurrent(this);
    const bool ran = m_engine->InvokeRendererCommand(name);
    if (ran) {
        USDCC_LOG_INFO("usdcc.viewport", "Ran renderer command {} of {}", name.GetString(),
                       m_engine->GetRendererDisplayName(m_engine->GetCurrentRendererId()));
    } else {
        USDCC_LOG_WARNING("usdcc.viewport", "The viewport's renderer has no command {}", name.GetString());
    }
    renderNow();
    return ran;
}

void HydraViewportWindow::setPreviewMode(bool preview) {
    if (preview == m_previewMode) return;
    m_previewMode = preview;
    m_dragging = false;
    renderNow();
}

void HydraViewportWindow::setDomeLightVisible(bool visible) {
    DisplayOptions options = m_display;
    options.domeLightTextures = visible;
    setDisplayOptions(options);
}

void HydraViewportWindow::setPaused(bool paused) {
    if (paused == m_paused) return;
    m_paused = paused;
    renderNow();
}

bool HydraViewportWindow::isOnScreen() const {
    if (!isExposed()) return false;
    const QWindow* top = this;
    while (top->parent()) top = top->parent();
    return !(top->windowStates() & Qt::WindowMinimized);
}

void HydraViewportWindow::watchTopLevel() {
    QWindow* top = this;
    while (top->parent()) top = top->parent();
    if (top == m_topLevel) return;
    disconnect(m_topLevelConnection);
    m_topLevel = top;
    m_topLevelConnection = connect(top, &QWindow::windowStateChanged, this, [this](Qt::WindowState) {
        // Restored, the native child gets no expose event of its own to render on.
        if (isOnScreen()) {
            requestUpdate();
        } else {
            updateRendererActivity();
        }
    });
}

void HydraViewportWindow::updateRendererActivity() {
    if (!m_engine) return;
    const bool run = !m_paused && isOnScreen();
    if (run == (m_rendererHalt == RendererHalt::None)) return;
    const auto rendererName = [this]() { return m_engine->GetRendererDisplayName(m_engine->GetCurrentRendererId()); };
    if (run) {
        const bool resumed = m_rendererHalt == RendererHalt::Paused ? m_engine->ResumeRenderer() : m_engine->RestartRenderer();
        if (resumed) {
            USDCC_LOG_DEBUG("usdcc.viewport", "Viewport renderer {} resumed", rendererName());
        } else {
            USDCC_LOG_WARNING("usdcc.viewport", "Could not resume the viewport's renderer {}", rendererName());
        }
        m_rendererHalt = RendererHalt::None;
        return;
    }
    if (m_engine->IsPauseRendererSupported() && m_engine->PauseRenderer()) {
        m_rendererHalt = RendererHalt::Paused;
    } else if (m_engine->IsStopRendererSupported() && m_engine->StopRenderer()) {
        m_rendererHalt = RendererHalt::Stopped;
    } else {
        return;  // nothing runs between frames (Storm)
    }
    USDCC_LOG_DEBUG("usdcc.viewport", "Viewport renderer {} {} ({})", rendererName(),
                    m_rendererHalt == RendererHalt::Paused ? "paused" : "stopped",
                    m_paused ? "the viewport is paused" : "the viewport is off screen");
}

void HydraViewportWindow::showLastFrame(int width, int height) {
    auto* gl = m_context->functions();
    QOpenGLFramebufferObject::bindDefault();
    gl->glViewport(0, 0, width, height);
    gl->glClearColor(static_cast<float>(m_display.background.redF()), static_cast<float>(m_display.background.greenF()),
                     static_cast<float>(m_display.background.blueF()), 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (m_framebuffer) {
        // Fitted, keeping its shape, as the window may have been resized since.
        const QSize frame = m_framebuffer->size();
        const double scale = std::min(static_cast<double>(width) / frame.width(), static_cast<double>(height) / frame.height());
        const int w = std::max(1, static_cast<int>(frame.width() * scale));
        const int h = std::max(1, static_cast<int>(frame.height() * scale));
        const QRect target((width - w) / 2, (height - h) / 2, w, h);
        QOpenGLFramebufferObject::blitFramebuffer(nullptr, target, m_framebuffer.get(), QRect(QPoint(0, 0), frame),
                                                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    QOpenGLFramebufferObject::bindDefault();
    drawHud(width, height);
    m_context->swapBuffers(this);
}

void HydraViewportWindow::setHudCounts(const HudCounts& counts) {
    if (counts == m_hudCounts) return;
    m_hudCounts = counts;
    refreshHud();
}

HudInput HydraViewportWindow::hudInput() const {
    HudInput input;
    input.size = QSizeF(width(), height());
    input.background = m_display.background;
    input.hasStage = static_cast<bool>(m_stage);
    input.paused = m_paused;
    input.hover = m_hudHover;
    input.cursor = m_cursorPos;

    input.showCamera = m_display.hudCamera;
    // The orbit camera, until the viewport can look through the stage's cameras.
    input.camera = QStringLiteral("Persp");
    const PXR_NS::TfToken renderer = currentRendererPlugin();
    input.renderer = rendererDisplayName(renderer);
    input.shading = shadingModeLabel(effectiveShading(m_display, renderer));
    input.shadingAvailable = renderer.IsEmpty() || rendererDrawsShadingModes(renderer);

    input.showAxes = m_display.hudAxes;
    input.viewMatrix = currentFrustum().ComputeViewMatrix();

    if (m_stage) {
        input.zUp = PXR_NS::UsdGeomGetStageUpAxis(m_stage) == PXR_NS::UsdGeomTokens->z;
        input.upAxis = input.zUp ? QStringLiteral("Z") : QStringLiteral("Y");
        input.metersPerUnit = PXR_NS::UsdGeomGetStageMetersPerUnit(m_stage);
        const PXR_NS::SdfLayerHandle target = m_stage->GetEditTarget().GetLayer();
        if (target && target != m_stage->GetRootLayer()) input.editTarget = QString::fromStdString(target->GetDisplayName());

        input.showStats = m_display.hudStats;
        input.stageCounts = m_hudCounts.stage;
        input.selectedPrims = m_selectedPaths.size();
        if (!m_selectedPaths.empty()) input.selectionCounts = m_hudCounts.selection;
        if (m_selectedPaths.size() == 1) {
            if (const PXR_NS::UsdPrim prim = m_stage->GetPrimAtPath(m_selectedPaths.front())) {
                input.selectionType = QString::fromStdString(prim.GetTypeName().GetString());
            }
        }
    }

    input.showPerformance = m_display.hudPerformance;
    input.frameMs = m_shownFrameMs;
    input.fps = m_shownFps;
    input.converging = m_converging;
    input.percentDone = m_percentDone;

    if (m_display.hudToolReadout && m_dragging) {
        if (const usdcc::tools::Tool* tool = activeToolInstance()) input.toolReadout = QString::fromStdString(tool->readout());
    }
    if (m_display.hudRollover) input.rollover = m_rollover;
    return input;
}

void HydraViewportWindow::drawHud(int width, int height) {
    if (!hudActive() || !m_legacyGl) {
        m_hudLayout = {};
        return;
    }
    const QFont font = QGuiApplication::font();
    HudLayout layout = buildViewportHud(hudInput(), font);
    if (layout != m_hudLayout || m_hudImage.size() != QSize(width, height)) {
        m_hudLayout = std::move(layout);
        m_hudImage = QImage(width, height, QImage::Format_RGBA8888_Premultiplied);
        m_hudImage.setDevicePixelRatio(devicePixelRatio());
        m_hudImage.fill(Qt::transparent);
        QPainter painter(&m_hudImage);
        paintViewportHud(painter, m_hudLayout, font);
        painter.end();
        m_hudTextureStale = true;
    }
    if (m_hudLayout.isEmpty()) return;

    // Whatever Hydra left bound, the HUD needs none of it: no program, no sampler, no
    // unpack buffer, texture unit 0, no depth test, culling, scissor or sRGB conversion.
    auto* gl = m_context->extraFunctions();
    gl->glUseProgram(0);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindSampler(0, 0);
    gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisable(0x8DB9);  // GL_FRAMEBUFFER_SRGB
    gl->glViewport(0, 0, width, height);

    if (!m_hudTexture) {
        gl->glGenTextures(1, &m_hudTexture);
        m_hudTextureStale = true;
    }
    gl->glBindTexture(GL_TEXTURE_2D, m_hudTexture);
    if (m_hudTextureStale) {
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_hudImage.constBits());
        m_hudTextureStale = false;
    }

    // The image, premultiplied, over the whole window: its first row is the top.
    m_legacyGl->glMatrixMode(GL_PROJECTION);
    m_legacyGl->glLoadIdentity();
    m_legacyGl->glMatrixMode(GL_MODELVIEW);
    m_legacyGl->glLoadIdentity();
    m_legacyGl->glEnable(GL_TEXTURE_2D);
    m_legacyGl->glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    gl->glEnable(GL_BLEND);
    gl->glBlendEquation(GL_FUNC_ADD);
    gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    m_legacyGl->glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    m_legacyGl->glBegin(GL_QUADS);
    m_legacyGl->glTexCoord2f(0.0f, 1.0f);
    m_legacyGl->glVertex2f(-1.0f, -1.0f);
    m_legacyGl->glTexCoord2f(1.0f, 1.0f);
    m_legacyGl->glVertex2f(1.0f, -1.0f);
    m_legacyGl->glTexCoord2f(1.0f, 0.0f);
    m_legacyGl->glVertex2f(1.0f, 1.0f);
    m_legacyGl->glTexCoord2f(0.0f, 0.0f);
    m_legacyGl->glVertex2f(-1.0f, 1.0f);
    m_legacyGl->glEnd();
    m_legacyGl->glDisable(GL_TEXTURE_2D);
    gl->glDisable(GL_BLEND);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glEnable(GL_DEPTH_TEST);
}

void HydraViewportWindow::refreshHud() {
    if (!isExposed() || !m_context || !m_framebuffer || m_previewMode) return;
    if (!m_context->makeCurrent(this)) return;
    const qreal pixelRatio = devicePixelRatio();
    const int w = std::max(1, static_cast<int>(width() * pixelRatio));
    const int h = std::max(1, static_cast<int>(height() * pixelRatio));
    if (m_paused) {
        showLastFrame(w, h);  // draws the HUD
        return;
    }
    if (m_framebuffer->size() != QSize(w, h)) {
        requestUpdate();  // resized since the last frame: draw a new one
        return;
    }
    const QRect frame(0, 0, w, h);
    QOpenGLFramebufferObject::blitFramebuffer(nullptr, frame, m_framebuffer.get(), frame);
    QOpenGLFramebufferObject::bindDefault();
    drawHud(w, h);
    m_context->swapBuffers(this);
}

void HydraViewportWindow::recordFrame(double milliseconds) {
    if (!m_display.hudPerformance || !hudActive()) return;
    const qint64 now = m_clock.elapsed();
    m_frameTimes.push_back(now);
    while (!m_frameTimes.empty() && now - m_frameTimes.front() > 1000) m_frameTimes.pop_front();
    // A few times a second, or the numbers can't be read.
    if (!m_shownFrameMs || now - m_shownAt >= 250) {
        m_shownFrameMs = milliseconds;
        if (m_frameTimes.size() >= 3 && m_frameTimes.back() > m_frameTimes.front()) {
            m_shownFps = 1000.0 * static_cast<double>(m_frameTimes.size() - 1) /
                         static_cast<double>(m_frameTimes.back() - m_frameTimes.front());
        } else {
            m_shownFps.reset();
        }
        m_shownAt = now;
    }
    m_idleTimer->start();
}

void HydraViewportWindow::snapToAxis(HudItem item) {
    if (item == HudItem::AxisCenter) {
        m_yaw = kDefaultOrbitYaw;
        m_pitch = kDefaultOrbitPitch;
    } else if (const std::optional<PXR_NS::GfVec3d> direction = hudAxisDirection(item)) {
        // Straight down or up the camera's Y, any yaw looks that way: 0 puts X to the right.
        const OrbitAngles angles = orbitAnglesLookingFrom(*direction, 0.0);
        m_yaw = angles.yaw;
        m_pitch = angles.pitch;
    } else {
        return;
    }
    cameraMoved();
}

void HydraViewportWindow::updateHover(const QPointF& position) {
    const HudItem item = hudActive() ? hudItemAt(m_hudLayout, position) : HudItem::None;
    if (item == m_hudHover) return;
    m_hudHover = item;
    if (item == HudItem::None) {
        unsetCursor();
    } else {
        setCursor(Qt::PointingHandCursor);
    }
    refreshHud();
}

void HydraViewportWindow::showRollover() {
    if (!hudActive() || !m_display.hudRollover || !m_stage || m_hudHover != HudItem::None) return;
    const PXR_NS::SdfPath path = pickPrim(ndcFromPixel(m_cursorPos));
    QString text;
    if (!path.IsEmpty()) {
        text = QString::fromStdString(path.GetString());
        if (const PXR_NS::UsdPrim prim = m_stage->GetPrimAtPath(path); prim && !prim.GetTypeName().IsEmpty()) {
            text += QStringLiteral(" · ") + QString::fromStdString(prim.GetTypeName().GetString());
        }
    }
    if (text == m_rollover) return;
    m_rollover = text;
    // A full frame: the pick drew with the frame's framebuffer bound.
    requestUpdate();
}

std::vector<PXR_NS::TfToken> HydraViewportWindow::rendererPlugins() const {
    return m_engine ? m_engine->GetRendererPlugins() : std::vector<PXR_NS::TfToken>{};
}

QString HydraViewportWindow::rendererDisplayName(const PXR_NS::TfToken& pluginId) const {
    return m_engine ? QString::fromStdString(m_engine->GetRendererDisplayName(pluginId)) : QString();
}

PXR_NS::TfToken HydraViewportWindow::currentRendererPlugin() const {
    return m_engine ? m_engine->GetCurrentRendererId() : m_rendererPluginId;
}

void HydraViewportWindow::setRendererPlugin(const PXR_NS::TfToken& pluginId) {
    if (pluginId.IsEmpty()) {
        return;
    }
    if (!m_engine) {
        // Before the first frame: the engine starts with it (ensureEngine()).
        m_rendererPluginId = pluginId;
        return;
    }
    m_context->makeCurrent(this);
    const PXR_NS::TfToken previous = m_engine->GetCurrentRendererId();
    if (m_engine->SetRendererPlugin(pluginId)) {
        if (m_engine->GetCurrentRendererId() != previous) forgetRendererHalt();  // a new one, running
        applyRendererSettings();  // a new delegate starts from its defaults
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
    const OrbitFraming framing = frameOrbitCamera(box, m_fovY, aspect, m_distance);
    m_target = framing.target;
    m_distance = std::max(kMinDistance, framing.distance);
    cameraMoved();
}

double HydraViewportWindow::verticalFieldOfView() { return kFovY; }

void HydraViewportWindow::setFieldOfView(double degrees) {
    degrees = std::clamp(degrees, 1.0, 179.0);
    if (degrees == m_fovY) return;
    m_fovY = degrees;
    cameraMoved();
}

void HydraViewportWindow::cameraMoved() {
    renderNow();
    Q_EMIT cameraChanged();
}

void HydraViewportWindow::setDisplayOptions(const DisplayOptions& options) {
    if (options == m_display) return;
    // What the engine holds rather than takes per frame.
    const bool reconfigure = options.selectionColor != m_display.selectionColor ||
                             options.domeLightTextures != m_display.domeLightTextures;
    m_display = options;
    if (m_engine && reconfigure) configureEngine();
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
    frustum.SetPerspective(m_fovY, /*isFovVertical=*/true, static_cast<double>(w) / static_cast<double>(h),
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

    // What the frame draws, so what's picked is what's shown (purposes, culling,
    // refinement). Hidden Line picks its surfaces, which hide what's behind them.
    const ShadingMode shading = effectiveShading(m_display, currentRendererPlugin());
    PXR_NS::UsdImagingGLRenderParams params = viewportRenderParams(m_display, shading);
    if (shading == ShadingMode::HiddenLine) params.drawMode = PXR_NS::UsdImagingGLDrawMode::DRAW_GEOM_ONLY;

    PXR_NS::UsdImagingGLEngine::PickParams pickParams;
    pickParams.resolveMode = PXR_NS::HdxPickResolveModeTokens->resolveNearestToCamera;

    PXR_NS::UsdImagingGLEngine::IntersectionResultVector results;
    bindFramebuffer(w, h);  // as for rendering, see renderNow()
    bool hit = false;
    try {
        hit = m_engine->TestIntersection(pickParams, pickFrustum.ComputeViewMatrix(),
                                         pickFrustum.ComputeProjectionMatrix(), m_stage->GetPseudoRoot(), params, &results);
    } catch (const std::exception& error) {
        QOpenGLFramebufferObject::bindDefault();
        recoverFromEngineError("picking", error);  // nothing picked: the next click works
        return {};
    }
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

void HydraViewportWindow::drawSelectionOverlay(const usdcc::tools::ToolContext& ctx, int width, int height) {
    if (m_previewMode || m_display.selectionStyle == SelectionStyle::Solid) {
        m_selectionOverlay.reset();  // its engine holds a copy of the scene: not kept for later
        return;
    }
    if (!m_stage || m_selectedPaths.empty() || !m_framebuffer) return;  // kept: the selection comes back
    if (!m_selectionOverlay) m_selectionOverlay = std::make_unique<SelectionOverlay>();
    SelectionOverlay::Frame frame;
    frame.stage = m_stage;
    frame.selection = m_selectedPaths;
    frame.style = m_display.selectionStyle;
    frame.color = m_display.selectionColor;
    // In UI pixels, scaled as the rest of the app is, then to the screen's.
    frame.width = usdcc::ui::UiScaleController::instance().pixels(m_display.selectionWidth) * devicePixelRatio();
    frame.viewMatrix = ctx.viewMatrix;
    frame.projMatrix = ctx.projMatrix;
    const OrbitClipping clipping = orbitClipping(m_distance);
    frame.nearClip = clipping.nearClip;
    frame.farClip = clipping.farClip;
    frame.size = QSize(width, height);
    frame.targetFramebuffer = m_framebuffer->handle();
    // The same geometry as the frame: purposes, refinement, culling.
    frame.params = viewportRenderParams(m_display, ShadingMode::SmoothShaded);
    m_selectionOverlay->draw(frame);
}

void HydraViewportWindow::drawGrid(const PXR_NS::GfMatrix4d& viewMatrix, const PXR_NS::GfMatrix4d& projMatrix) {
    // No stage, no ground: an empty viewport shows only its background.
    if (!m_legacyGl || !m_display.showGrid || !m_stage) return;
    // On the stage's ground: XY for a Z-up stage. The camera itself stays Y-up.
    const bool zUp = PXR_NS::UsdGeomGetStageUpAxis(m_stage) == PXR_NS::UsdGeomTokens->z;
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
    watchTopLevel();
    renderNow();  // or, no longer exposed, pauses the renderer
}

void HydraViewportWindow::resizeEvent(QResizeEvent*) { cameraMoved(); }  // the aspect ratio

bool HydraViewportWindow::event(QEvent* ev) {
    if (ev->type() == QEvent::UpdateRequest) {
        renderNow();
        return true;
    }
    if (ev->type() == QEvent::ParentWindowChange) {
        watchTopLevel();  // docked, floated, or moved to another window
    }
    if (ev->type() == QEvent::Leave) {
        m_rolloverTimer->stop();
        const bool shown = m_hudHover != HudItem::None || !m_rollover.isEmpty();
        m_hudHover = HudItem::None;
        m_rollover.clear();
        unsetCursor();
        if (shown) refreshHud();
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
    if (!isOnScreen()) {
        updateRendererActivity();  // nobody would see what it renders
        return;
    }

    ensureEngine();
    if (!m_engine || !m_context->makeCurrent(this)) return;
    updateRendererActivity();  // resumed before it renders again, or paused with the viewport

    const qreal pixelRatio = devicePixelRatio();
    const int w = std::max(1, static_cast<int>(width() * pixelRatio));
    const int h = std::max(1, static_cast<int>(height() * pixelRatio));
    if (m_paused) {
        showLastFrame(w, h);
        return;
    }
    QElapsedTimer frameTimer;  // for the HUD's frame time
    frameTimer.start();

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

    // The shading this renderer draws: Storm draws them all, any other Smooth Shaded only.
    const ShadingMode shading = effectiveShading(m_display, m_engine->GetCurrentRendererId());

    // The default lights, in every mode: Storm shades a wire as it would the surface, so
    // without them the line modes' wires are near black (usdview leaves them out there).
    // A preview brings its own lights (a dome, a key), and a camera light would flatten them.
    PXR_NS::GlfSimpleLightVector lights;
    if (!m_previewMode) {
        if (m_display.cameraLight) {
            PXR_NS::GlfSimpleLight cameraLight;
            cameraLight.SetPosition(PXR_NS::GfVec4f(static_cast<float>(toolCtx.cameraPos[0]),
                                                     static_cast<float>(toolCtx.cameraPos[1]),
                                                     static_cast<float>(toolCtx.cameraPos[2]), 1.0f));
            lights.push_back(cameraLight);
        }
        if (m_display.domeLight) {
            // Hydra's packaged map (HdxPackageDefaultDomeLightTexture()). A dome's map has
            // Y up: turned onto a Z-up stage's sky, as usdview does.
            PXR_NS::GlfSimpleLight dome;
            dome.SetIsDomeLight(true);
            if (m_stage && PXR_NS::UsdGeomGetStageUpAxis(m_stage) == PXR_NS::UsdGeomTokens->z) {
                dome.SetTransform(PXR_NS::GfMatrix4d().SetRotate(PXR_NS::GfRotation(PXR_NS::GfVec3d::XAxis(), 90.0)));
            }
            lights.push_back(dome);
        }
    }
    PXR_NS::GlfSimpleMaterial material;
    const PXR_NS::GfVec4f sceneAmbient = m_previewMode ? PXR_NS::GfVec4f(0.0f, 0.0f, 0.0f, 1.0f)
                                                       : PXR_NS::GfVec4f(0.15f, 0.15f, 0.15f, 1.0f);
    m_engine->SetLightingState(lights, material, sceneAmbient);

    PXR_NS::UsdImagingGLRenderParams params = viewportRenderParams(m_display, shading);
    if (m_previewMode) params.enableSceneLights = true;  // a preview's lights are its stage's
    // SetSelected() only feeds the engine the selection; it is drawn solely
    // when the render params ask for it (the flag defaults to off). Only Solid has
    // Hydra fill it: the line styles leave the prims' shading as it is.
    params.highlight = !m_previewMode && m_display.selectionStyle == SelectionStyle::Solid;

    if (m_stage) try {
        if (shading == ShadingMode::HiddenLine) {
            // usdview's Hidden Surface Wireframe: the surfaces first, for their depth only
            // (their color is cleared to the background again), then the edges, which
            // Hydra's composite depth-tests against it: those behind a surface are left out.
            // The surfaces' depth is pushed back a little, or a wire and its own surface tie
            // and the wire comes out dashed. usdview's glPolygonOffset() doesn't reach Hydra's
            // own pipeline; a projection that adds to every depth does (NDC z + bias).
            PXR_NS::UsdImagingGLRenderParams surfaces = params;
            surfaces.drawMode = PXR_NS::UsdImagingGLDrawMode::DRAW_GEOM_ONLY;
            surfaces.highlight = false;
            PXR_NS::GfMatrix4d pushedBack = toolCtx.projMatrix;
            for (int row = 0; row < 4; ++row) pushedBack[row][2] += kHiddenLineDepthBias * pushedBack[row][3];
            m_engine->SetCameraState(toolCtx.viewMatrix, pushedBack);
            m_engine->Render(m_stage->GetPseudoRoot(), surfaces);
            m_engine->SetCameraState(toolCtx.viewMatrix, toolCtx.projMatrix);
            m_framebuffer->bind();
            if (auto* gl = m_context->functions()) {
                gl->glViewport(0, 0, w, h);
                gl->glClearColor(static_cast<float>(m_display.background.redF()),
                                 static_cast<float>(m_display.background.greenF()),
                                 static_cast<float>(m_display.background.blueF()), 1.0f);
                gl->glClear(GL_COLOR_BUFFER_BIT);
            }
        }
        m_engine->Render(m_stage->GetPseudoRoot(), params);
        m_engineFailures = 0;
    } catch (const std::exception& error) {
        QOpenGLFramebufferObject::bindDefault();
        recoverFromEngineError("drawing", error);  // the last frame stays on screen
        return;
    }

    if (!m_previewMode) {
        drawGrid(toolCtx.viewMatrix, toolCtx.projMatrix);
        drawSelectionOverlay(toolCtx, w, h);
        if (usdcc::tools::Tool* tool = activeToolInstance()) {
            drawGizmo(tool->buildGizmo(toolCtx), toolCtx.viewMatrix, toolCtx.projMatrix);
        }
    }

    // How far a progressive renderer has got, for the HUD's performance line.
    m_converging = m_stage && !m_engine->IsConverged();
    m_percentDone.reset();
    if (m_converging && m_display.hudPerformance) {
        const PXR_NS::VtDictionary stats = m_engine->GetRenderStats();
        if (const auto it = stats.find("percentDone"); it != stats.end()) {
            const PXR_NS::VtValue& value = it->second;
            if (value.IsHolding<double>()) m_percentDone = value.UncheckedGet<double>();
            else if (value.IsHolding<float>()) m_percentDone = value.UncheckedGet<float>();
            else if (value.IsHolding<int>()) m_percentDone = value.UncheckedGet<int>();
        }
    }
    if (m_stage) recordFrame(static_cast<double>(frameTimer.nsecsElapsed()) / 1e6);

    const QRect frame(0, 0, w, h);
    QOpenGLFramebufferObject::blitFramebuffer(nullptr, frame, m_framebuffer.get(), frame);
    QOpenGLFramebufferObject::bindDefault();
    drawHud(w, h);
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
    m_cursorPos = event->position();
    Q_EMIT pressed();
    m_rolloverTimer->stop();
    if (!m_rollover.isEmpty()) {
        m_rollover.clear();
        refreshHud();
    }

    // The HUD gets first refusal (docs/VIEWPORT_HUD.md §4): a click on one of its labels or
    // on the axis triad goes nowhere else.
    if (event->button() == Qt::LeftButton && hudActive()) {
        const HudItem item = hudItemAt(m_hudLayout, event->position());
        if (isHudAxis(item)) {
            snapToAxis(item);
            return;
        }
        if (item != HudItem::None) {
            const QRectF rect = hudItemRect(m_hudLayout, item);
            Q_EMIT hudMenuRequested(item, mapToGlobal(QPointF(rect.left(), rect.bottom() + 2.0)).toPoint());
            return;
        }
    }

    if (event->button() == Qt::LeftButton) {
        // Alt+drag is the camera-navigation override: it must always orbit,
        // never reach the active tool. Without this, a tool's mousePress()
        // would always claim the click (SelectTool reports handled=true even
        // on empty space, since a click always resolves to "select this" or
        // "select nothing", and Move/Rotate/Scale are SelectTools that fall
        // back to that when no gizmo handle is under the cursor), so
        // m_dragging would latch and the view could never be orbited.
        if (!(event->modifiers() & Qt::AltModifier) && !m_previewMode) {
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
    m_cursorPos = event->position();

    if (event->buttons() == Qt::NoButton) {
        // Hovering: the HUD's labels and axes light up, and once the cursor rests the
        // prim under it shows.
        updateHover(event->position());
        if (hudActive() && m_display.hudRollover && m_stage) {
            if (!m_rollover.isEmpty()) {
                m_rollover.clear();
                refreshHud();
            }
            m_rolloverTimer->start();
        }
        return;
    }

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
    // Nothing else uses the right button.
    if (event->button() == Qt::RightButton) Q_EMIT contextMenuRequested(event->globalPosition().toPoint());
}

void HydraViewportWindow::wheelEvent(QWheelEvent* event) {
    const double factor = std::pow(0.9, event->angleDelta().y() / 120.0);
    m_distance = std::max(kMinDistance, m_distance * factor);
    cameraMoved();
}

}  // namespace usdcc::usd
