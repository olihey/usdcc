#include "viewport_overlay_renderer.h"

#include "usdcc/core/log.h"

#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

#include <cstddef>

namespace usdcc::usd {

namespace {

// Lines and triangles, already in view space. A line's corner knows the line's other
// end, and moves half the width sideways from the line on the screen, so the two
// triangles of a line make a quad that many pixels wide. A triangle's corner has
// side 0 and stays where it is.
constexpr const char* kGeometryVertex = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aOther;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aSide;
uniform mat4 uProjection;
uniform vec2 uHalfViewport;  // in pixels
uniform float uLineWidth;    // in pixels
out vec4 vColor;
void main() {
    vec4 here = uProjection * vec4(aPosition, 1.0);
    vColor = aColor;
    if (aSide == 0.0) {
        gl_Position = here;
        return;
    }
    vec4 there = uProjection * vec4(aOther, 1.0);
    // An end behind the eye has no place on the screen: the line is cut off just in
    // front of the eye first, as clipping would cut it.
    const float kInFront = 1e-5;
    if (here.w < kInFront && there.w >= kInFront) {
        here = mix(here, there, (kInFront - here.w) / (there.w - here.w));
    } else if (there.w < kInFront && here.w >= kInFront) {
        there = mix(there, here, (kInFront - there.w) / (here.w - there.w));
    }
    vec2 along = there.xy / there.w * uHalfViewport - here.xy / here.w * uHalfViewport;
    float length_ = length(along);
    along = length_ > 1e-6 ? along / length_ : vec2(1.0, 0.0);
    vec2 sideways = vec2(-along.y, along.x) * aSide * uLineWidth * 0.5;
    gl_Position = vec4(here.xy + sideways / uHalfViewport * here.w, here.z, here.w);
}
)";

constexpr const char* kGeometryFragment = R"(#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";

// One triangle that covers the framebuffer; the image's first row is the top one.
constexpr const char* kImageVertex = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = vec2(corner.x, 1.0 - corner.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kImageFragment = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uImage;
out vec4 fragColor;
void main() {
    fragColor = texture(uImage, vUv);
}
)";

std::unique_ptr<QOpenGLShaderProgram> buildProgram(const char* vertex, const char* fragment) {
    auto program = std::make_unique<QOpenGLShaderProgram>();
    if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
        !program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !program->link()) {
        USDCC_LOG_WARNING("usdcc.viewport", "Viewport overlays: a shader didn't build: {}", program->log().toStdString());
        return {};
    }
    return program;
}

}  // namespace

ViewportOverlayRenderer::ViewportOverlayRenderer() = default;

ViewportOverlayRenderer::~ViewportOverlayRenderer() = default;

bool ViewportOverlayRenderer::ensurePrograms() {
    if (m_programsFailed) return false;
    if (m_geometryProgram) return true;
    m_geometryProgram = buildProgram(kGeometryVertex, kGeometryFragment);
    m_imageProgram = buildProgram(kImageVertex, kImageFragment);
    m_geometryVao = std::make_unique<QOpenGLVertexArrayObject>();
    m_imageVao = std::make_unique<QOpenGLVertexArrayObject>();
    m_vertexBuffer = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
    if (!m_geometryProgram || !m_imageProgram || !m_geometryVao->create() || !m_imageVao->create() ||
        !m_vertexBuffer->create()) {
        m_programsFailed = true;  // logged once; the viewport then has no grid, gizmos or HUD
        USDCC_LOG_WARNING("usdcc.viewport", "Viewport overlays are off: the grid, gizmos and HUD aren't drawn");
        m_geometryProgram.reset();
        m_imageProgram.reset();
        m_geometryVao.reset();
        m_imageVao.reset();
        m_vertexBuffer.reset();
        return false;
    }
    m_vertexBuffer->setUsagePattern(QOpenGLBuffer::StreamDraw);

    auto* gl = QOpenGLContext::currentContext()->extraFunctions();
    m_geometryVao->bind();
    m_vertexBuffer->bind();
    const auto attribute = [gl](unsigned int index, int size, std::size_t offset) {
        gl->glEnableVertexAttribArray(index);
        gl->glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offset));
    };
    attribute(0, 3, offsetof(Vertex, position));
    attribute(1, 3, offsetof(Vertex, other));
    attribute(2, 4, offsetof(Vertex, color));
    attribute(3, 1, offsetof(Vertex, side));
    m_geometryVao->release();
    m_vertexBuffer->release();
    return true;
}

void ViewportOverlayRenderer::begin(const PXR_NS::GfMatrix4d& viewMatrix) {
    m_viewMatrix = viewMatrix;
    m_lines.clear();
    m_triangles.clear();
}

void ViewportOverlayRenderer::push(std::vector<Vertex>& into, const PXR_NS::GfVec3d& position,
                                   const PXR_NS::GfVec3d& other, const PXR_NS::GfVec4f& color, float side) {
    into.push_back({{static_cast<float>(position[0]), static_cast<float>(position[1]), static_cast<float>(position[2])},
                    {static_cast<float>(other[0]), static_cast<float>(other[1]), static_cast<float>(other[2])},
                    {color[0], color[1], color[2], color[3]},
                    side});
}

void ViewportOverlayRenderer::line(const PXR_NS::GfVec3d& a, const PXR_NS::GfVec3d& b, const PXR_NS::GfVec4f& color) {
    const PXR_NS::GfVec3d viewA = m_viewMatrix.Transform(a);
    const PXR_NS::GfVec3d viewB = m_viewMatrix.Transform(b);
    // Seen from b, a's sideways points the other way: the signs there are swapped, so
    // that both ends' corners are on the same two sides of the line.
    push(m_lines, viewA, viewB, color, -1.0f);
    push(m_lines, viewA, viewB, color, 1.0f);
    push(m_lines, viewB, viewA, color, 1.0f);
    push(m_lines, viewB, viewA, color, 1.0f);
    push(m_lines, viewA, viewB, color, 1.0f);
    push(m_lines, viewB, viewA, color, -1.0f);
}

void ViewportOverlayRenderer::triangle(const PXR_NS::GfVec3d& a, const PXR_NS::GfVec3d& b, const PXR_NS::GfVec3d& c,
                                       const PXR_NS::GfVec4f& color) {
    for (const PXR_NS::GfVec3d* corner : {&a, &b, &c}) {
        const PXR_NS::GfVec3d view = m_viewMatrix.Transform(*corner);
        push(m_triangles, view, view, color, 0.0f);
    }
}

void ViewportOverlayRenderer::draw(const std::vector<Vertex>& vertices) {
    if (vertices.empty()) return;
    m_vertexBuffer->allocate(vertices.data(), static_cast<int>(vertices.size() * sizeof(Vertex)));
    QOpenGLContext::currentContext()->extraFunctions()->glDrawArrays(GL_TRIANGLES, 0, static_cast<int>(vertices.size()));
}

void ViewportOverlayRenderer::end(const PXR_NS::GfMatrix4d& projMatrix, const QSize& viewport, float lineWidth) {
    if ((m_lines.empty() && m_triangles.empty()) || viewport.isEmpty() || !ensurePrograms()) return;
    auto* gl = QOpenGLContext::currentContext()->extraFunctions();

    // GfMatrix4d is row-major with row vectors (p' = p * M). Read as OpenGL's
    // column-major array for column vectors (v' = M * v), its numbers are the
    // transpose of that, which is the same transform: no transpose of our own.
    float projection[16];
    const double* source = projMatrix.GetArray();
    for (int i = 0; i < 16; ++i) projection[i] = static_cast<float>(source[i]);

    m_geometryProgram->bind();
    gl->glUniformMatrix4fv(m_geometryProgram->uniformLocation("uProjection"), 1, GL_FALSE, projection);
    m_geometryProgram->setUniformValue("uHalfViewport", viewport.width() * 0.5f, viewport.height() * 0.5f);
    m_geometryProgram->setUniformValue("uLineWidth", lineWidth);
    m_geometryVao->bind();
    m_vertexBuffer->bind();

    // A line's quad faces wherever the line happens to run: never culled.
    const bool culling = gl->glIsEnabled(GL_CULL_FACE);
    if (culling) gl->glDisable(GL_CULL_FACE);
    draw(m_lines);
    if (culling) gl->glEnable(GL_CULL_FACE);
    draw(m_triangles);

    m_vertexBuffer->release();
    m_geometryVao->release();
    m_geometryProgram->release();
    m_lines.clear();
    m_triangles.clear();
}

void ViewportOverlayRenderer::drawImage(unsigned int texture) {
    if (!texture || !ensurePrograms()) return;
    auto* gl = QOpenGLContext::currentContext()->extraFunctions();
    m_imageProgram->bind();
    m_imageProgram->setUniformValue("uImage", 0);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    m_imageVao->bind();
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    m_imageVao->release();
    m_imageProgram->release();
}

}  // namespace usdcc::usd
