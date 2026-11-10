#pragma once

#include <pxr/base/gf/vec3d.h>

#include <vector>

namespace usdcc::usd {

// The viewport's ground grid (docs/VIEWPORT_GRID.md), as lines: Qt- and GL-free, for tests.
//
// The grid is a square `size` wide, centred on the origin, on the ground plane — XZ for a
// Y-up stage, XY for a Z-up one — with a line every `step`. Every tenth line is a major
// one; the two lines through the origin are the axes.
enum class GridLineKind {
    Minor,
    Major,
    AxisX,      // along the X axis (y = z = 0)
    AxisOther,  // along the ground's other axis: Z (Y-up) or Y (Z-up)
};

struct GridLine {
    PXR_NS::GfVec3d a;
    PXR_NS::GfVec3d b;
    GridLineKind kind = GridLineKind::Minor;
};

// The most lines drawn in each direction. A step too fine for the size is coarsened, by
// whole multiples, until the grid fits, so a typo can't ask for millions of lines.
inline constexpr int kMaxGridLinesPerDirection = 1001;

// The step actually used for `size` and `step`: `step`, or the smallest whole multiple
// of it that keeps the grid within kMaxGridLinesPerDirection. 0 when there is no grid
// to draw (a size or step that isn't positive).
double effectiveGridStep(double size, double step);

// The grid's lines. Lines sit on whole multiples of the step from the origin, out to
// half the size each way, so the axes are always among them. `zUp`: the ground is XY.
std::vector<GridLine> buildViewportGrid(double size, double step, bool zUp = false);

}  // namespace usdcc::usd
