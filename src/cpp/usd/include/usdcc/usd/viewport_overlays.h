#pragma once

// What viewports draw over the render besides the grid, the selection and the tools
// (docs/PRIMVAR_VIEWER.md §9.2 point 7): world-space lines and triangles, each overlay switched
// on or off per viewport in its options menu (Overlays). The app's first is the primvar elements
// picked in the Primvars panel; plugins add theirs.

#include "usdcc/core/export.h"

#include "usdcc/tools/tool.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/timeCode.h>

#include <QObject>
#include <QString>

#include <functional>
#include <string>
#include <vector>

namespace usdcc::usd {

// What an overlay is drawn for: one viewport's frame.
struct ViewportOverlayContext {
    const Stage* stage = nullptr;  // the viewport's, with its per-stage state; null for none
    PXR_NS::UsdStageRefPtr usdStage;
    PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default();
    std::vector<PXR_NS::SdfPath> selection;
};

struct ViewportOverlay {
    std::string id;  // stable: viewports and sessions remember which are off by it
    QString label;   // in the Overlays menu
    // Called for every frame: keep it quick, and cache what's slow.
    std::function<usdcc::tools::GizmoGeometry(const ViewportOverlayContext&)> build;
};

// Main thread.
class USDCC_USD_UI_API ViewportOverlays : public QObject {
    Q_OBJECT

public:
    static ViewportOverlays& instance();

    // One with the id of another replaces it.
    void add(ViewportOverlay overlay);
    // False if there was none.
    bool remove(const std::string& id);
    const std::vector<ViewportOverlay>& overlays() const { return m_overlays; }
    const ViewportOverlay* find(const std::string& id) const;
    // What an overlay draws changed without anything viewports watch: draw again.
    void requestRedraw() { Q_EMIT redrawRequested(); }

Q_SIGNALS:
    void changed();
    void redrawRequested();

private:
    ViewportOverlays();
    std::vector<ViewportOverlay> m_overlays;
};

// The built-in overlay's id and what it draws: the stage's primvar elements (Stage::primvarElements())
// in the selection colour and the one hovered in a light blue, as crosses at points and instances,
// a face's outline and fill, a mark inside the face at a corner, a curve's line. At most
// kMaxElements of each.
inline constexpr const char* kPrimvarElementsOverlay = "usdcc.primvarElements";
inline constexpr std::size_t kMaxOverlayElements = 20000;
USDCC_USD_UI_API usdcc::tools::GizmoGeometry buildPrimvarElementsOverlay(const PXR_NS::UsdStageRefPtr& stage,
                                                                         const PrimvarElements& selected,
                                                                         const PrimvarElements& hovered,
                                                                         PXR_NS::UsdTimeCode time);

}  // namespace usdcc::usd
