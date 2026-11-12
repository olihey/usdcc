#include "selection_overlay.h"

#include "usdcc/core/log.h"

#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usdImaging/usdImaging/tokens.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

namespace {
// An outline where the selection is hidden behind other geometry: faint, not left out
// (docs/VIEWPORT_SELECTION.md decision 3).
constexpr float kHiddenOpacity = 0.35f;
// How far behind the viewport's depth a selected pixel may be and still count as the
// one the viewport shows: the same surface drawn twice doesn't land on exactly the same
// depth, and a wire on a surface is a line over the triangles under it. Relative to the
// distance from the camera.
constexpr float kDepthTolerance = 0.003f;

QOpenGLExtraFunctions* gl() { return QOpenGLContext::currentContext()->extraFunctions(); }

// One triangle that covers the viewport, made from gl_VertexID: no vertex buffer.
constexpr const char* kFullScreenVertex = R"(#version 330 core
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Pass 1. Which pixels are the selection's: "strong" ones are drawn at full strength,
// "weak" ones faintly. Outline: strong = selected and visible (the overlay's depth is the
// viewport's), weak = selected at all. Wireframe: strong = a selected edge that is
// visible, no weak ones. Per pixel, the horizontal distance to the nearest of each,
// within `radius` (radius + 1 = none), for pass 2 to finish into distances in 2D.
constexpr const char* kRowsFragment = R"(#version 330 core
uniform sampler2D maskColor;
uniform sampler2D maskDepth;
uniform sampler2D sceneDepth;
uniform int radius;
uniform int wireframe;
uniform float nearClip;
uniform float farClip;
uniform float tolerance;
layout(location = 0) out vec2 result;

float linearDepth(float depth) {
    float z = depth * 2.0 - 1.0;
    return 2.0 * nearClip * farClip / (farClip + nearClip - z * (farClip - nearClip));
}

void classify(ivec2 p, out bool strong, out bool weak) {
    bool selected = texelFetch(maskColor, p, 0).r > 0.5;
    strong = false;
    weak = selected && wireframe == 0;
    if (!selected) return;
    float overlay = linearDepth(texelFetch(maskDepth, p, 0).r);
    float scene = linearDepth(texelFetch(sceneDepth, p, 0).r);
    strong = overlay <= scene * (1.0 + tolerance);
}

void main() {
    ivec2 size = textureSize(maskColor, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    float none = float(radius + 1);
    float strongDistance = none;
    float weakDistance = none;
    for (int dx = -radius; dx <= radius; ++dx) {
        ivec2 q = ivec2(p.x + dx, p.y);
        if (q.x < 0 || q.x >= size.x) continue;
        bool strong, weak;
        classify(q, strong, weak);
        float d = abs(float(dx));
        if (strong) strongDistance = min(strongDistance, d);
        if (weak) weakDistance = min(weakDistance, d);
    }
    result = vec2(strongDistance, weakDistance);
}
)";

// Pass 2. The distance to the nearest strong and weak pixel, from pass 1's rows, and the
// line from those. Outline: a ring `width` wide outside the selection, faint where it
// only reaches hidden pixels. Wireframe: the visible edges, widened to `width`.
constexpr const char* kCompositeFragment = R"(#version 330 core
uniform sampler2D rows;
uniform sampler2D maskColor;
uniform int radius;
uniform int wireframe;
uniform float width;
uniform float hiddenOpacity;
uniform vec4 color;
layout(location = 0) out vec4 fragColor;

void main() {
    ivec2 size = textureSize(rows, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    float none = float(radius + 1);
    float strong2 = 1e9;
    float weak2 = 1e9;
    for (int dy = -radius; dy <= radius; ++dy) {
        ivec2 q = ivec2(p.x, p.y + dy);
        if (q.y < 0 || q.y >= size.y) continue;
        vec2 row = texelFetch(rows, q, 0).rg;
        float dy2 = float(dy * dy);
        if (row.x < none) strong2 = min(strong2, row.x * row.x + dy2);
        if (row.y < none) weak2 = min(weak2, row.y * row.y + dy2);
    }
    float strongDistance = sqrt(strong2);
    float weakDistance = sqrt(weak2);
    float alpha;
    if (wireframe != 0) {
        alpha = clamp((width - 1.0) * 0.5 + 1.0 - strongDistance, 0.0, 1.0);
    } else {
        // Outside the selection only: its own pixels keep their shading.
        float inside = clamp(texelFetch(maskColor, p, 0).r, 0.0, 1.0);
        float visible = clamp(width + 1.0 - strongDistance, 0.0, 1.0);
        float hidden = clamp(width + 1.0 - weakDistance, 0.0, 1.0) * hiddenOpacity;
        alpha = max(visible, hidden) * (1.0 - inside);
    }
    if (alpha <= 0.0) discard;
    fragColor = vec4(color.rgb, color.a * alpha);
}
)";

unsigned int makeTexture(GLenum internalFormat, GLenum format, GLenum type, const QSize& size) {
    auto* f = gl();
    GLuint texture = 0;
    f->glGenTextures(1, &texture);
    f->glBindTexture(GL_TEXTURE_2D, texture);
    f->glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), size.width(), size.height(), 0, format, type,
                    nullptr);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

unsigned int makeFramebuffer(unsigned int color, unsigned int depthStencil) {
    auto* f = gl();
    GLuint fbo = 0;
    f->glGenFramebuffers(1, &fbo);
    f->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (color) f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    if (depthStencil) f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthStencil, 0);
    if (f->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        USDCC_LOG_WARNING("usdcc.viewport", "Selection overlay: a framebuffer is incomplete");
    }
    return fbo;
}
}  // namespace

SelectionOverlay::SelectionOverlay() = default;

SelectionOverlay::~SelectionOverlay() {
    m_engine.reset();
    releaseTargets();
    m_rowsProgram.reset();
    m_compositeProgram.reset();
    m_vao.reset();
}

std::vector<PXR_NS::SdfPath> SelectionOverlay::renderRoots(const PXR_NS::UsdStageRefPtr& stage,
                                                           const std::vector<PXR_NS::SdfPath>& selection) {
    std::vector<PXR_NS::SdfPath> roots;
    if (!stage) return roots;
    bool instanced = false;
    for (const PXR_NS::SdfPath& path : selection) {
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
        if (!prim) continue;
        if (prim.IsInstance() || prim.IsInstanceProxy()) {
            instanced = true;  // its geometry is its prototype's, propagated (below)
        } else {
            roots.push_back(path);
        }
    }
    // A selected prim with native instances under it.
    for (const PXR_NS::UsdPrim& prototype : stage->GetPrototypes()) {
        if (instanced) break;
        for (const PXR_NS::UsdPrim& instance : prototype.GetInstances()) {
            const PXR_NS::SdfPath& at = instance.GetPath();
            if (std::any_of(roots.begin(), roots.end(), [&at](const PXR_NS::SdfPath& root) { return at.HasPrefix(root); })) {
                instanced = true;
                break;
            }
        }
    }
    // All of them: which instance is selected is told apart by the selection itself,
    // which the engine resolves per instance (renderMask()).
    if (instanced) {
        roots.push_back(PXR_NS::SdfPath::AbsoluteRootPath().AppendChild(PXR_NS::UsdImagingTokens->niPropagatedPrototypesScope));
    }
    return roots;
}

void SelectionOverlay::ensureTargets(const QSize& size) {
    if (size == m_size && m_maskFbo) return;
    releaseTargets();
    m_size = size;
    m_maskColor = makeTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, size);
    m_maskDepth = makeTexture(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, size);
    m_maskFbo = makeFramebuffer(m_maskColor, m_maskDepth);
    m_sceneDepth = makeTexture(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, size);
    m_sceneDepthFbo = makeFramebuffer(0, m_sceneDepth);
    m_rows = makeTexture(GL_RG16F, GL_RG, GL_FLOAT, size);
    m_rowsFbo = makeFramebuffer(m_rows, 0);
}

void SelectionOverlay::releaseTargets() {
    if (!QOpenGLContext::currentContext()) return;
    auto* f = gl();
    for (unsigned int* fbo : {&m_maskFbo, &m_sceneDepthFbo, &m_rowsFbo}) {
        if (*fbo) f->glDeleteFramebuffers(1, fbo);
        *fbo = 0;
    }
    for (unsigned int* texture : {&m_maskColor, &m_maskDepth, &m_sceneDepth, &m_rows}) {
        if (*texture) f->glDeleteTextures(1, texture);
        *texture = 0;
    }
    m_size = {};
}

bool SelectionOverlay::ensurePrograms() {
    if (m_programsFailed) return false;
    if (m_rowsProgram) return true;
    auto build = [](const char* fragment) {
        auto program = std::make_unique<QOpenGLShaderProgram>();
        if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, kFullScreenVertex) ||
            !program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !program->link()) {
            USDCC_LOG_WARNING("usdcc.viewport", "Selection overlay: a shader didn't build: {}", program->log().toStdString());
            return std::unique_ptr<QOpenGLShaderProgram>();
        }
        return program;
    };
    m_rowsProgram = build(kRowsFragment);
    m_compositeProgram = build(kCompositeFragment);
    m_vao = std::make_unique<QOpenGLVertexArrayObject>();
    if (!m_rowsProgram || !m_compositeProgram || !m_vao->create()) {
        m_programsFailed = true;  // logged once; the selection then shows no lines
        m_rowsProgram.reset();
        m_compositeProgram.reset();
        return false;
    }
    return true;
}

void SelectionOverlay::renderMask(const Frame& frame) {
    if (!m_engine || frame.stage != m_stage) {
        // A new stage needs a new engine: it populates from one stage only.
        m_engine = std::make_unique<PXR_NS::UsdImagingGLEngine>();
        m_engine->SetRendererPlugin(PXR_NS::TfToken("HdStormRendererPlugin"));
        // Selected white, over everything else drawn black (renderParams.overrideColor).
        m_engine->SetSelectionColor(PXR_NS::GfVec4f(1.0f, 1.0f, 1.0f, 1.0f));
        m_stage = frame.stage;
        m_selection.clear();
    }

    auto* f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_maskFbo);
    f->glViewport(0, 0, frame.size.width(), frame.size.height());
    f->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    f->glClearDepthf(1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_engine->SetRenderBufferSize(PXR_NS::GfVec2i(frame.size.width(), frame.size.height()));
    m_engine->SetRenderViewport(PXR_NS::GfVec4d(0, 0, frame.size.width(), frame.size.height()));
    m_engine->SetCameraState(frame.viewMatrix, frame.projMatrix);
    m_engine->SetLightingState(PXR_NS::GlfSimpleLightVector{}, PXR_NS::GlfSimpleMaterial(),
                               PXR_NS::GfVec4f(0.0f, 0.0f, 0.0f, 1.0f));

    // The viewport's geometry, or the lines don't sit on what it shows.
    PXR_NS::UsdImagingGLRenderParams params;
    params.frame = PXR_NS::UsdTimeCode::Default();
    params.complexity = frame.params.complexity;
    params.showGuides = frame.params.showGuides;
    params.showProxy = frame.params.showProxy;
    params.showRender = frame.params.showRender;
    params.cullStyle = frame.params.cullStyle;
    params.drawMode = frame.style == SelectionStyle::Wireframe ? PXR_NS::UsdImagingGLDrawMode::DRAW_WIREFRAME
                                                               : PXR_NS::UsdImagingGLDrawMode::DRAW_SHADED_FLAT;
    params.enableLighting = false;
    params.enableSceneMaterials = false;
    params.enableSceneLights = false;
    params.highlight = true;
    params.overrideColor = PXR_NS::GfVec4f(0.0f, 0.0f, 0.0f, 1.0f);
    params.clearColor = PXR_NS::GfVec4f(0.0f, 0.0f, 0.0f, 0.0f);

    const std::vector<PXR_NS::SdfPath> roots = renderRoots(frame.stage, frame.selection);
    m_engine->PrepareBatch(frame.stage->GetPseudoRoot(), params);
    // After PrepareBatch(): a selection set before the engine has populated from the stage
    // is lost, and the first frames would show none.
    if (frame.selection != m_selection) {
        m_selection = frame.selection;
        m_engine->SetSelected(PXR_NS::SdfPathVector(m_selection.begin(), m_selection.end()));
    }
    if (!roots.empty()) m_engine->RenderBatch(PXR_NS::SdfPathVector(roots.begin(), roots.end()), params);
}

void SelectionOverlay::composite(const Frame& frame) {
    auto* f = gl();
    const int w = frame.size.width();
    const int h = frame.size.height();

    // The viewport's depth, as a texture: Hydra's composite left it in the target.
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, frame.targetFramebuffer);
    f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_sceneDepthFbo);
    f->glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    const bool wireframe = frame.style == SelectionStyle::Wireframe;
    const float width = static_cast<float>(std::max(1.0, frame.width));
    // How far pass 1 and 2 look: as far as a line reaches, and a pixel more for its edge.
    const int radius = static_cast<int>(std::ceil(wireframe ? (width - 1.0f) * 0.5f : width)) + 1;

    f->glDisable(GL_DEPTH_TEST);
    f->glDepthMask(GL_FALSE);
    f->glDisable(GL_BLEND);
    f->glDisable(GL_CULL_FACE);
    f->glDisable(GL_SCISSOR_TEST);
    f->glViewport(0, 0, w, h);
    m_vao->bind();

    // Pass 1, into m_rows.
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_rowsFbo);
    m_rowsProgram->bind();
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, m_maskColor);
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, m_maskDepth);
    f->glActiveTexture(GL_TEXTURE2);
    f->glBindTexture(GL_TEXTURE_2D, m_sceneDepth);
    m_rowsProgram->setUniformValue("maskColor", 0);
    m_rowsProgram->setUniformValue("maskDepth", 1);
    m_rowsProgram->setUniformValue("sceneDepth", 2);
    m_rowsProgram->setUniformValue("radius", radius);
    m_rowsProgram->setUniformValue("wireframe", wireframe ? 1 : 0);
    m_rowsProgram->setUniformValue("nearClip", static_cast<float>(frame.nearClip));
    m_rowsProgram->setUniformValue("farClip", static_cast<float>(frame.farClip));
    m_rowsProgram->setUniformValue("tolerance", kDepthTolerance);
    f->glDrawArrays(GL_TRIANGLES, 0, 3);

    // Pass 2, blended onto the viewport's frame.
    f->glBindFramebuffer(GL_FRAMEBUFFER, frame.targetFramebuffer);
    f->glEnable(GL_BLEND);
    f->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_compositeProgram->bind();
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, m_rows);
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, m_maskColor);
    m_compositeProgram->setUniformValue("rows", 0);
    m_compositeProgram->setUniformValue("maskColor", 1);
    m_compositeProgram->setUniformValue("radius", radius);
    m_compositeProgram->setUniformValue("wireframe", wireframe ? 1 : 0);
    m_compositeProgram->setUniformValue("width", width);
    m_compositeProgram->setUniformValue("hiddenOpacity", kHiddenOpacity);
    m_compositeProgram->setUniformValue("color", frame.color);
    f->glDrawArrays(GL_TRIANGLES, 0, 3);

    // Back to what the fixed-function drawing after this (the gizmo) expects.
    m_compositeProgram->release();
    m_vao->release();
    for (GLenum unit : {GL_TEXTURE2, GL_TEXTURE1, GL_TEXTURE0}) {
        f->glActiveTexture(unit);
        f->glBindTexture(GL_TEXTURE_2D, 0);
    }
    f->glDisable(GL_BLEND);
    f->glDepthMask(GL_TRUE);
    f->glEnable(GL_DEPTH_TEST);
}

void SelectionOverlay::draw(const Frame& frame) {
    if (!frame.stage || frame.selection.empty() || frame.size.isEmpty() || !QOpenGLContext::currentContext()) return;
    if (!ensurePrograms()) return;
    ensureTargets(frame.size);
    renderMask(frame);
    composite(frame);
    gl()->glBindFramebuffer(GL_FRAMEBUFFER, frame.targetFramebuffer);
}

}  // namespace usdcc::usd
