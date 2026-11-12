#pragma once

// The viewport's heads-up display (docs/VIEWPORT_HUD.md): what it shows where, worked out
// from plain inputs (buildViewportHud()), painted with a QPainter (paintViewportHud()),
// and which of its items is under a point (hudItemAt()). No GL: HydraViewportWindow
// paints the layout into an image and draws that over the frame.

#include "usdcc/usd/scene_counts.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

class QPainter;

namespace usdcc::usd {

// What can be clicked or hovered in the HUD.
enum class HudItem {
    None,
    Camera,      // top left: the camera's name; a menu
    Renderer,    // the renderer; the Renderer menu
    Shading,     // the shading mode; the Shading menu
    EditTarget,  // bottom left, when it isn't the root layer; the stage's layers
    AxisPosX,    // the axis triad's ends: the view from that side
    AxisNegX,
    AxisPosY,
    AxisNegY,
    AxisPosZ,
    AxisNegZ,
    AxisCenter,  // the triad's centre: the default view
};

bool isHudAxis(HudItem item);
// The world direction an axis end points to ((1, 0, 0) for AxisPosX): the camera goes
// there, looking back at its target. Nothing for the others.
std::optional<PXR_NS::GfVec3d> hudAxisDirection(HudItem item);
// What the view from an axis end is called on a stage with that up axis: "Top" for the up
// axis, "Front" for +Z on a Y-up stage and -Y on a Z-up one, and so on; "Default View"
// for the centre.
QString hudAxisViewName(HudItem item, bool zUp);

// "1,204" up to 99,999, then "312K", "2.1M", "1.3B".
QString formatHudCount(std::uint64_t count);
// A stage's metersPerUnit as a unit: "cm" (0.01), "m", "mm", "km", "in", "ft"; otherwise
// "0.5 m".
QString formatMetersPerUnit(double metersPerUnit);

// Everything the HUD shows; the window fills it in for each frame.
struct HudInput {
    QSizeF size;  // the window, in logical pixels
    QColor background{0x1c, 0x1c, 0x1e};  // the HUD draws light ink on a dark one and dark on a light one
    bool hasStage = false;
    bool paused = false;

    // Top left: how it's drawn.
    bool showCamera = true;
    QString camera;
    QString renderer;
    QString shading;
    bool shadingAvailable = true;  // greyed when the renderer draws Smooth Shaded only

    // Top right: the axis triad, turning with the camera.
    bool showAxes = true;
    PXR_NS::GfMatrix4d viewMatrix{1.0};
    bool zUp = false;

    // Bottom left.
    QString upAxis;            // "Y" or "Z"
    double metersPerUnit = 0;  // 0: not shown
    QString editTarget;        // its name; empty when it's the root layer

    bool showStats = false;
    std::optional<SceneCounts> stageCounts;      // nothing yet: "Counting..."
    std::optional<SceneCounts> selectionCounts;  // only while something is selected
    std::size_t selectedPrims = 0;
    QString selectionType;  // the type of the one selected prim; empty with more

    // Bottom right.
    bool showPerformance = false;
    std::optional<double> frameMs;
    std::optional<double> fps;
    bool converging = false;
    std::optional<double> percentDone;  // 0-100, from the renderer when it says

    // Transient.
    QString toolReadout;  // bottom centre, during a gizmo drag
    QString rollover;     // the prim under the cursor, beside it
    QPointF cursor;

    HudItem hover = HudItem::None;
};

// One piece of text, where it goes, and how.
struct HudText {
    QString text;
    QRectF rect;  // logical pixels
    QColor color;
    HudItem item = HudItem::None;  // clickable when not None
    bool badge = false;            // on a plate of its own (Paused, the readout, the rollover)
    bool plated = false;           // on its corner's plate (HudLayout::plates)
    bool hovered = false;
    bool operator==(const HudText&) const = default;
};

// One end of the axis triad.
struct HudAxisEnd {
    HudItem item = HudItem::None;
    QPointF end;    // logical pixels
    double depth = 0;  // towards the viewer is larger: drawn last, hit first
    QColor color;
    QString label;  // "X" on the positive end, empty on the negative one
    bool hovered = false;
    bool operator==(const HudAxisEnd&) const = default;
};

struct HudLayout {
    QSizeF size;
    QColor ink;     // the color of text that isn't on a plate (the empty viewport's hint)
    QColor shadow;  // behind it, so it reads on anything
    std::vector<HudText> texts;
    // Semi-transparent black, behind each corner's text (top left, bottom left, bottom
    // right, and the view name under the triad), so it reads on anything.
    std::vector<QRectF> plates;
    // Empty when the triad isn't shown.
    std::vector<HudAxisEnd> axes;  // back to front
    QPointF axesCenter;
    double axesRadius = 0;
    bool centerHovered = false;

    bool isEmpty() const { return texts.empty() && axes.empty(); }
    bool operator==(const HudLayout&) const = default;
};

// The radius of the triad's ends and centre, in logical pixels.
inline constexpr double kHudAxisEndRadius = 8.0;
inline constexpr double kHudAxisCenterRadius = 5.0;

// Where everything goes, in `font`. Needs a QGuiApplication (for the font's metrics).
HudLayout buildViewportHud(const HudInput& input, const QFont& font);

// The clickable item at `point`, or None: the triad's ends front first, then its centre,
// then the labels.
HudItem hudItemAt(const HudLayout& layout, const QPointF& point);
// Its rect, for a menu to open under it; empty for None and the triad.
QRectF hudItemRect(const HudLayout& layout, HudItem item);

// Paints `layout` in `font`, in logical pixels, on a transparent device.
void paintViewportHud(QPainter& painter, const HudLayout& layout, const QFont& font);

}  // namespace usdcc::usd
