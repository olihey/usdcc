#pragma once

// Presence on a shared stage (docs/SHARED_STAGE.md §8): where the others are. Each
// member's selection, camera, current frame and what it drags, as SharedStages hears
// them, for the panels that show them: the Outliner's dots, the viewport's outlines and
// frustums, and Follow. Also this instance's own view, which the viewports report and
// SharedStages sends. Shown only; nothing of it changes the stage.

#include "usdcc/core/export.h"
#include "usdcc/peers/types.h"
#include "usdcc/tools/tool.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <map>
#include <optional>
#include <vector>

namespace usdcc::usd::share {

// Where a viewport looks from.
struct USDCC_USD_UI_API PeerView {
    PXR_NS::GfMatrix4d cameraToWorld{1.0};
    double fovY = 45.0;            // degrees, perspective
    bool orthographic = false;
    double orthoSize = 10.0;       // the view's height, orthographic
    double aspect = 1.5;
    double nearClip = 0.1, farClip = 1000.0;
    double distance = 10.0;        // to what it orbits about

    bool operator==(const PeerView&) const = default;
    PXR_NS::GfFrustum frustum() const;
    static PeerView fromFrustum(const PXR_NS::GfFrustum& frustum, double distance);
    QJsonObject toJson() const;
    static std::optional<PeerView> fromJson(const QJsonObject& json);
};

struct USDCC_USD_UI_API PeerPresence {
    usdcc::peers::PeerId id;
    QString name;
    QColor color;
    std::vector<PXR_NS::SdfPath> selection;
    std::optional<PeerView> view;
    std::optional<double> time;
    PXR_NS::SdfPath dragging;  // a prim in a gizmo now; empty for none
};

class USDCC_USD_UI_API PresenceHub : public QObject {
    Q_OBJECT

public:
    static PresenceHub& instance();

    // This instance's: the last view a viewport showing `stage` drew. Emits
    // localViewChanged() when it moved.
    void reportView(const PXR_NS::UsdStage* stage, const PeerView& view);
    std::optional<PeerView> localView(const PXR_NS::UsdStage* stage) const;

    // The others on `stage`, as SharedStages last heard; empty when not shared.
    void setPeers(const PXR_NS::UsdStage* stage, std::vector<PeerPresence> peers);
    const std::vector<PeerPresence>& peers(const PXR_NS::UsdStage* stage) const;
    void clear(const PXR_NS::UsdStage* stage);
    // The colors of those who have `path` selected.
    std::vector<QColor> colorsSelecting(const PXR_NS::UsdStage* stage, const PXR_NS::SdfPath& path) const;

    // Follow (§8): the viewports showing `stage` look through `peer`'s view, and the
    // stage's time is the peer's, until the user moves the view (stopFollowing()).
    void follow(const PXR_NS::UsdStage* stage, const usdcc::peers::PeerId& peer);
    void stopFollowing(const PXR_NS::UsdStage* stage);
    usdcc::peers::PeerId following(const PXR_NS::UsdStage* stage) const;
    // The followed peer's view and time, if there is one to follow.
    std::optional<PeerView> followedView(const PXR_NS::UsdStage* stage) const;

Q_SIGNALS:
    void localViewChanged(const PXR_NS::UsdStage* stage);
    void peersChanged(const PXR_NS::UsdStage* stage);
    void followChanged(const PXR_NS::UsdStage* stage);

private:
    std::map<const PXR_NS::UsdStage*, PeerView> m_local;
    std::map<const PXR_NS::UsdStage*, std::vector<PeerPresence>> m_peers;
    std::map<const PXR_NS::UsdStage*, usdcc::peers::PeerId> m_following;
};

// The lines a viewport draws for the others (§8): a box around each prim a peer has
// selected and the frustum it looks through, in the peer's color. `skip` is a peer not
// to draw the frustum of (the one followed: its frustum is the view).
USDCC_USD_UI_API usdcc::tools::GizmoGeometry buildPresenceOverlay(const PXR_NS::UsdStageRefPtr& stage,
                                                                  const std::vector<PeerPresence>& peers,
                                                                  PXR_NS::UsdTimeCode time,
                                                                  const usdcc::peers::PeerId& skip = {});

}  // namespace usdcc::usd::share
