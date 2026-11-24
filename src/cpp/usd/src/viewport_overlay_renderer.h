#pragma once

// What the viewport draws over Hydra's frame itself: the grid, the tools' gizmos and
// the HUD's image. Lines, triangles and one textured quad through two small shaders,
// which run in a Core profile too: macOS has no other with shaders newer than GLSL 1.20
// (docs/MACOS.md), and no lines wider than a pixel in it, so lines are quads here.
// Private to HydraViewportWindow, which owns one per window.

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4f.h>

#include <QSize>

#include <memory>
#include <vector>

class QOpenGLBuffer;
class QOpenGLShaderProgram;
class QOpenGLVertexArrayObject;

namespace usdcc::usd {

class ViewportOverlayRenderer {
public:
    // Needs the viewport's GL context current, here and in every call below, the
    // destructor included.
    ViewportOverlayRenderer();
    ~ViewportOverlayRenderer();
    ViewportOverlayRenderer(const ViewportOverlayRenderer&) = delete;
    ViewportOverlayRenderer& operator=(const ViewportOverlayRenderer&) = delete;

    // Starts a batch of lines and triangles given in world space, seen through
    // `viewMatrix`. They are moved into view space here, in double precision, so a
    // scene far from the origin keeps its gizmos steady.
    void begin(const PXR_NS::GfMatrix4d& viewMatrix);
    void line(const PXR_NS::GfVec3d& a, const PXR_NS::GfVec3d& b, const PXR_NS::GfVec4f& color);
    void triangle(const PXR_NS::GfVec3d& a, const PXR_NS::GfVec3d& b, const PXR_NS::GfVec3d& c,
                  const PXR_NS::GfVec4f& color);
    // Draws the batch into the bound framebuffer: the lines, `lineWidth` device pixels
    // wide, then the triangles. Depth test, depth writes and blending are the caller's,
    // as set; `viewport` is the framebuffer's size in device pixels.
    void end(const PXR_NS::GfMatrix4d& projMatrix, const QSize& viewport, float lineWidth);

    // Draws the 2D texture `texture` over the whole bound framebuffer, its first row at
    // the top. Blending is the caller's.
    void drawImage(unsigned int texture);

private:
    struct Vertex {
        float position[3];
        float other[3];  // a line's other end; a triangle's corner again
        float color[4];
        float side;      // which side of the line this corner is on: -1 or 1; 0 for a triangle
    };

    bool ensurePrograms();
    void draw(const std::vector<Vertex>& vertices);
    void push(std::vector<Vertex>& into, const PXR_NS::GfVec3d& position, const PXR_NS::GfVec3d& other,
              const PXR_NS::GfVec4f& color, float side);

    PXR_NS::GfMatrix4d m_viewMatrix{1.0};
    std::vector<Vertex> m_lines, m_triangles;

    std::unique_ptr<QOpenGLShaderProgram> m_geometryProgram, m_imageProgram;
    std::unique_ptr<QOpenGLVertexArrayObject> m_geometryVao, m_imageVao;
    std::unique_ptr<QOpenGLBuffer> m_vertexBuffer;
    bool m_programsFailed = false;
};

}  // namespace usdcc::usd
