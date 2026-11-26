#include "usdcc/usd/viewport_overlays.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/curves.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <algorithm>
#include <utility>

namespace usdcc::usd {

using namespace PXR_NS;
using usdcc::tools::GizmoGeometry;

namespace {
const GfVec4f kSelectedColor(1.0f, 0.65f, 0.0f, 1.0f);  // the viewport's selection orange
const GfVec4f kHoveredColor(0.35f, 0.85f, 1.0f, 1.0f);
constexpr float kFillAlpha = 0.3f;

// Draws one set of elements of one prim.
class Drawer {
public:
    Drawer(GizmoGeometry& geometry, const UsdPrim& prim, UsdTimeCode time, GfVec4f color)
        : m_geometry(geometry), m_prim(prim), m_time(time), m_color(color) {
        m_toWorld = UsdGeomXformable(prim) ? UsdGeomXformable(prim).ComputeLocalToWorldTransform(time) : GfMatrix4d(1);
    }

    void draw(const PrimvarElements& elements) {
        if (elements.domain == "instances") {
            VtVec3fArray positions;
            if (UsdGeomPointInstancer(m_prim).GetPositionsAttr().Get(&positions, m_time)) crosses(positions, elements.indices);
            return;
        }
        VtVec3fArray points;
        if (!UsdGeomPointBased(m_prim).GetPointsAttr().Get(&points, m_time) || points.empty()) return;
        sizeFrom(points);
        if (elements.domain == "points") {
            crosses(points, elements.indices);
        } else if (elements.domain == "faces" || elements.domain == "face corners") {
            const UsdGeomMesh mesh(m_prim);
            VtIntArray counts, indices;
            if (!mesh || !mesh.GetFaceVertexCountsAttr().Get(&counts, m_time) || !mesh.GetFaceVertexIndicesAttr().Get(&indices, m_time)) return;
            if (elements.domain == "faces") {
                faces(points, counts, indices, elements.indices);
            } else {
                corners(points, counts, indices, elements.indices);
            }
        } else if (elements.domain == "curves") {
            VtIntArray counts;
            if (UsdGeomCurves(m_prim).GetCurveVertexCountsAttr().Get(&counts, m_time)) curves(points, counts, elements.indices);
        }
    }

private:
    GfVec3d world(const GfVec3f& point) const { return m_toWorld.Transform(GfVec3d(point)); }

    // Marks a hundredth of the prim's size across, so they read at any scale.
    void sizeFrom(const VtVec3fArray& points) {
        GfRange3d range;
        for (const GfVec3f& point : points) range.UnionWith(GfVec3d(point));
        m_mark = std::max(range.GetSize().GetLength() * 0.012, 1e-6);
    }

    void line(const GfVec3d& a, const GfVec3d& b) { m_geometry.lines.push_back({a, b, m_color}); }

    void cross(const GfVec3d& local) {
        const double h = m_mark;
        for (const GfVec3d& axis : {GfVec3d(h, 0, 0), GfVec3d(0, h, 0), GfVec3d(0, 0, h)}) {
            line(m_toWorld.Transform(local - axis), m_toWorld.Transform(local + axis));
        }
    }

    void crosses(const VtVec3fArray& points, const std::vector<std::size_t>& which) {
        if (m_mark == 0) sizeFrom(points);
        std::size_t drawn = 0;
        for (const std::size_t i : which) {
            if (i >= points.size()) continue;
            cross(GfVec3d(points[i]));
            if (++drawn == kMaxOverlayElements) break;
        }
    }

    // Where each face starts in faceVertexIndices.
    static std::vector<std::size_t> starts(const VtIntArray& counts) {
        std::vector<std::size_t> result;
        result.reserve(counts.size());
        std::size_t at = 0;
        for (const int count : counts) {
            result.push_back(at);
            at += std::size_t(std::max(count, 0));
        }
        return result;
    }

    static bool valid(const VtVec3fArray& points, const VtIntArray& indices, std::size_t start, int count) {
        if (start + std::size_t(count) > indices.size()) return false;
        for (int i = 0; i < count; ++i) {
            if (indices[start + i] < 0 || std::size_t(indices[start + i]) >= points.size()) return false;
        }
        return true;
    }

    void faces(const VtVec3fArray& points, const VtIntArray& counts, const VtIntArray& indices, const std::vector<std::size_t>& which) {
        const std::vector<std::size_t> start = starts(counts);
        const GfVec4f fill(m_color[0], m_color[1], m_color[2], kFillAlpha);
        std::size_t drawn = 0;
        for (const std::size_t face : which) {
            if (face >= counts.size() || counts[face] < 3 || !valid(points, indices, start[face], counts[face])) continue;
            const int n = counts[face];
            const auto corner = [&](int i) { return world(points[indices[start[face] + std::size_t(i)]]); };
            for (int i = 0; i < n; ++i) line(corner(i), corner((i + 1) % n));
            for (int i = 1; i + 1 < n; ++i) m_geometry.triangles.push_back({corner(0), corner(i), corner(i + 1), fill});
            if (++drawn == kMaxOverlayElements) break;
        }
    }

    // A corner: a mark a quarter of the way from its point to its face's middle, and a line to it.
    void corners(const VtVec3fArray& points, const VtIntArray& counts, const VtIntArray& indices, const std::vector<std::size_t>& which) {
        const std::vector<std::size_t> start = starts(counts);
        std::size_t drawn = 0;
        for (const std::size_t cornerIndex : which) {
            const auto after = std::upper_bound(start.begin(), start.end(), cornerIndex);
            if (after == start.begin()) continue;
            const std::size_t face = std::size_t(after - start.begin()) - 1;
            if (!valid(points, indices, start[face], counts[face]) || cornerIndex >= start[face] + std::size_t(counts[face])) continue;
            GfVec3d middle(0);
            for (int i = 0; i < counts[face]; ++i) middle += GfVec3d(points[indices[start[face] + std::size_t(i)]]);
            middle /= double(counts[face]);
            const GfVec3d point(points[indices[cornerIndex]]);
            const GfVec3d mark = point + (middle - point) * 0.25;
            line(m_toWorld.Transform(point), m_toWorld.Transform(mark));
            cross(mark);
            if (++drawn == kMaxOverlayElements) break;
        }
    }

    void curves(const VtVec3fArray& points, const VtIntArray& counts, const std::vector<std::size_t>& which) {
        const std::vector<std::size_t> start = starts(counts);
        std::size_t drawn = 0;
        for (const std::size_t curve : which) {
            if (curve >= counts.size() || start[curve] + std::size_t(std::max(counts[curve], 0)) > points.size()) continue;
            for (int i = 1; i < counts[curve]; ++i) line(world(points[start[curve] + std::size_t(i) - 1]), world(points[start[curve] + std::size_t(i)]));
            if (++drawn == kMaxOverlayElements) break;
        }
    }

    GizmoGeometry& m_geometry;
    UsdPrim m_prim;
    UsdTimeCode m_time;
    GfVec4f m_color;
    GfMatrix4d m_toWorld;
    double m_mark = 0;
};

void drawElements(GizmoGeometry& geometry, const UsdStageRefPtr& stage, const PrimvarElements& elements, UsdTimeCode time,
                  GfVec4f color) {
    if (elements.empty() || !stage) return;
    const UsdPrim prim = stage->GetPrimAtPath(elements.prim);
    if (!prim) return;
    Drawer(geometry, prim, time, color).draw(elements);
}
}  // namespace

GizmoGeometry buildPrimvarElementsOverlay(const UsdStageRefPtr& stage, const PrimvarElements& selected,
                                          const PrimvarElements& hovered, UsdTimeCode time) {
    GizmoGeometry geometry;
    drawElements(geometry, stage, selected, time, kSelectedColor);
    drawElements(geometry, stage, hovered, time, kHoveredColor);
    return geometry;
}

ViewportOverlays& ViewportOverlays::instance() {
    static ViewportOverlays* const overlays = new ViewportOverlays;  // outlives every viewport
    return *overlays;
}

ViewportOverlays::ViewportOverlays() {
    m_overlays.push_back({kPrimvarElementsOverlay, tr("Primvar Elements"), [](const ViewportOverlayContext& context) {
                              if (!context.stage) return GizmoGeometry();
                              return buildPrimvarElementsOverlay(context.usdStage, context.stage->primvarElements(),
                                                                 context.stage->primvarElementHover(), context.time);
                          }});
}

void ViewportOverlays::add(ViewportOverlay overlay) {
    const auto it = std::find_if(m_overlays.begin(), m_overlays.end(), [&overlay](const ViewportOverlay& other) { return other.id == overlay.id; });
    if (it != m_overlays.end()) {
        *it = std::move(overlay);
    } else {
        m_overlays.push_back(std::move(overlay));
    }
    Q_EMIT changed();
}

bool ViewportOverlays::remove(const std::string& id) {
    if (std::erase_if(m_overlays, [&id](const ViewportOverlay& overlay) { return overlay.id == id; }) == 0) return false;
    Q_EMIT changed();
    return true;
}

const ViewportOverlay* ViewportOverlays::find(const std::string& id) const {
    const auto it = std::find_if(m_overlays.begin(), m_overlays.end(), [&id](const ViewportOverlay& overlay) { return overlay.id == id; });
    return it == m_overlays.end() ? nullptr : &*it;
}

}  // namespace usdcc::usd
