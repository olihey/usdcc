#include "usdcc/usd/share/presence.h"

#include <pxr/base/gf/range3d.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QJsonArray>

#include <algorithm>
#include <array>

using namespace PXR_NS;

namespace usdcc::usd::share {

GfFrustum PeerView::frustum() const {
    GfFrustum f;
    f.SetPositionAndRotationFromMatrix(cameraToWorld);
    if (orthographic) {
        const double halfHeight = orthoSize / 2.0;
        const double halfWidth = halfHeight * aspect;
        f.SetOrthographic(-halfWidth, halfWidth, -halfHeight, halfHeight, nearClip, farClip);
    } else {
        f.SetPerspective(fovY, true, aspect, nearClip, farClip);
    }
    return f;
}

PeerView PeerView::fromFrustum(const GfFrustum& frustum, double distance) {
    PeerView view;
    view.cameraToWorld = frustum.ComputeViewInverse();
    view.orthographic = frustum.GetProjectionType() == GfFrustum::Orthographic;
    const GfRange2d window = frustum.GetWindow();
    view.aspect = window.GetSize()[1] > 0 ? window.GetSize()[0] / window.GetSize()[1] : 1.0;
    if (view.orthographic) {
        view.orthoSize = window.GetSize()[1];
    } else {
        double fov = 45.0, aspect = 1.0, nearClip = 0.1, farClip = 1000.0;
        frustum.GetPerspective(true, &fov, &aspect, &nearClip, &farClip);
        view.fovY = fov;
    }
    view.nearClip = frustum.GetNearFar().GetMin();
    view.farClip = frustum.GetNearFar().GetMax();
    view.distance = distance;
    return view;
}

QJsonObject PeerView::toJson() const {
    QJsonArray matrix;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) matrix.append(cameraToWorld[r][c]);
    }
    return {{QStringLiteral("matrix"), matrix},       {QStringLiteral("fov"), fovY},
            {QStringLiteral("ortho"), orthographic},  {QStringLiteral("orthoSize"), orthoSize},
            {QStringLiteral("aspect"), aspect},       {QStringLiteral("near"), nearClip},
            {QStringLiteral("far"), farClip},         {QStringLiteral("distance"), distance}};
}

std::optional<PeerView> PeerView::fromJson(const QJsonObject& json) {
    const QJsonArray matrix = json.value(QLatin1String("matrix")).toArray();
    if (matrix.size() != 16) return std::nullopt;
    PeerView view;
    for (int i = 0; i < 16; ++i) view.cameraToWorld[i / 4][i % 4] = matrix[i].toDouble();
    view.fovY = std::clamp(json.value(QLatin1String("fov")).toDouble(45.0), 1.0, 179.0);
    view.orthographic = json.value(QLatin1String("ortho")).toBool();
    view.orthoSize = std::max(1e-6, json.value(QLatin1String("orthoSize")).toDouble(10.0));
    view.aspect = std::clamp(json.value(QLatin1String("aspect")).toDouble(1.5), 0.05, 20.0);
    view.nearClip = std::max(1e-6, json.value(QLatin1String("near")).toDouble(0.1));
    view.farClip = std::max(view.nearClip * 2, json.value(QLatin1String("far")).toDouble(1000.0));
    view.distance = std::max(1e-6, json.value(QLatin1String("distance")).toDouble(10.0));
    return view;
}

// ---------------------------------------------------------------------------------

PresenceHub& PresenceHub::instance() {
    static PresenceHub hub;
    return hub;
}

void PresenceHub::reportView(const UsdStage* stage, const PeerView& view) {
    if (!stage) return;
    const auto it = m_local.find(stage);
    if (it != m_local.end() && it->second == view) return;
    m_local[stage] = view;
    Q_EMIT localViewChanged(stage);
}

std::optional<PeerView> PresenceHub::localView(const UsdStage* stage) const {
    const auto it = m_local.find(stage);
    return it == m_local.end() ? std::nullopt : std::optional<PeerView>(it->second);
}

void PresenceHub::setPeers(const UsdStage* stage, std::vector<PeerPresence> peers) {
    m_peers[stage] = std::move(peers);
    Q_EMIT peersChanged(stage);
}

const std::vector<PeerPresence>& PresenceHub::peers(const UsdStage* stage) const {
    static const std::vector<PeerPresence> none;
    const auto it = m_peers.find(stage);
    return it == m_peers.end() ? none : it->second;
}

void PresenceHub::clear(const UsdStage* stage) {
    const bool had = m_peers.erase(stage) > 0;
    m_local.erase(stage);
    const bool followed = m_following.erase(stage) > 0;
    if (had) Q_EMIT peersChanged(stage);
    if (followed) Q_EMIT followChanged(stage);
}

std::vector<QColor> PresenceHub::colorsSelecting(const UsdStage* stage, const SdfPath& path) const {
    std::vector<QColor> colors;
    for (const PeerPresence& peer : peers(stage)) {
        if (std::find(peer.selection.begin(), peer.selection.end(), path) != peer.selection.end()) {
            colors.push_back(peer.color);
        }
    }
    return colors;
}

void PresenceHub::follow(const UsdStage* stage, const usdcc::peers::PeerId& peer) {
    if (!stage) return;
    m_following[stage] = peer;
    Q_EMIT followChanged(stage);
}

void PresenceHub::stopFollowing(const UsdStage* stage) {
    if (m_following.erase(stage) > 0) Q_EMIT followChanged(stage);
}

usdcc::peers::PeerId PresenceHub::following(const UsdStage* stage) const {
    const auto it = m_following.find(stage);
    return it == m_following.end() ? usdcc::peers::PeerId() : it->second;
}

std::optional<PeerView> PresenceHub::followedView(const UsdStage* stage) const {
    const usdcc::peers::PeerId id = following(stage);
    if (id.isEmpty()) return std::nullopt;
    for (const PeerPresence& peer : peers(stage)) {
        if (peer.id == id) return peer.view;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------

namespace {

void addBox(usdcc::tools::GizmoGeometry& geometry, const GfBBox3d& box, const GfVec4f& color) {
    const GfRange3d range = box.GetRange();
    if (range.IsEmpty()) return;
    const GfMatrix4d& matrix = box.GetMatrix();
    std::array<GfVec3d, 8> corners;
    for (int i = 0; i < 8; ++i) corners[size_t(i)] = matrix.Transform(range.GetCorner(size_t(i)));
    // GfRange3d's corners: bit 0 x, bit 1 y, bit 2 z.
    static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                          {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : kEdges) geometry.lines.push_back({corners[size_t(edge[0])], corners[size_t(edge[1])], color});
}

void addFrustum(usdcc::tools::GizmoGeometry& geometry, const PeerView& view, const GfVec4f& color) {
    // A small one: what the camera sees out to a little way past where it stands.
    PeerView shown = view;
    shown.nearClip = std::max(1e-3, view.distance * 0.02);
    shown.farClip = std::max(shown.nearClip * 2.0, view.distance * 0.15);
    const std::vector<GfVec3d> c = shown.frustum().ComputeCorners();  // near: lb rb lt rt, then far
    if (c.size() != 8) return;
    static constexpr int kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                          {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : kEdges) geometry.lines.push_back({c[size_t(edge[0])], c[size_t(edge[1])], color});
    // Which way is up: a small triangle over the far end.
    const GfVec3d top = (c[6] + c[7]) / 2.0 + (c[6] - c[4]) * 0.25;
    geometry.lines.push_back({c[6], top, color});
    geometry.lines.push_back({top, c[7], color});
}

}  // namespace

usdcc::tools::GizmoGeometry buildPresenceOverlay(const UsdStageRefPtr& stage, const std::vector<PeerPresence>& peers,
                                                 UsdTimeCode time, const usdcc::peers::PeerId& skip) {
    usdcc::tools::GizmoGeometry geometry;
    if (!stage || peers.empty()) return geometry;
    UsdGeomBBoxCache bboxes(time, {UsdGeomTokens->default_, UsdGeomTokens->render, UsdGeomTokens->proxy}, true);
    for (const PeerPresence& peer : peers) {
        const QColor c = peer.color.isValid() ? peer.color : QColor(255, 200, 60);
        const GfVec4f color(float(c.redF()), float(c.greenF()), float(c.blueF()), 1.0f);
        size_t shown = 0;
        for (const SdfPath& path : peer.selection) {
            if (++shown > 64) break;  // a big selection: the first are enough to see where
            const UsdPrim prim = stage->GetPrimAtPath(path);
            if (!prim || prim.IsPseudoRoot()) continue;
            addBox(geometry, bboxes.ComputeWorldBound(prim), color);
        }
        if (peer.view && peer.id != skip) addFrustum(geometry, *peer.view, color);
    }
    return geometry;
}

}  // namespace usdcc::usd::share
