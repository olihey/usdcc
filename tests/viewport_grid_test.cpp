// The viewport's ground grid as lines (docs/VIEWPORT_GRID.md): where they go, which are
// the axes and the major lines, the Z-up ground, and a step too fine for the size.
#include "test_harness.h"

#include "usdcc/usd/viewport_grid.h"

#include <algorithm>
#include <cmath>

using namespace usdcc::usd;
using PXR_NS::GfVec3d;

namespace {
long long countOf(const std::vector<GridLine>& lines, GridLineKind kind) {
    return std::count_if(lines.begin(), lines.end(), [kind](const GridLine& l) { return l.kind == kind; });
}
}  // namespace

USDCC_TEST(viewport_grid_lines) {
    // 20 wide, a line every 1: 21 lines each way, from -10 to 10.
    const std::vector<GridLine> lines = buildViewportGrid(20.0, 1.0);
    CHECK(lines.size() == 42, "21 lines each way: %zu", lines.size());
    CHECK(countOf(lines, GridLineKind::AxisX) == 1 && countOf(lines, GridLineKind::AxisOther) == 1, "one line per axis");
    // -10, 10 each way are major (every tenth line); 0 is an axis instead.
    CHECK(countOf(lines, GridLineKind::Major) == 4, "major lines at +-10: %lld", countOf(lines, GridLineKind::Major));
    bool flat = true, inside = true;
    for (const GridLine& line : lines) {
        flat = flat && line.a[1] == 0.0 && line.b[1] == 0.0;
        for (const GfVec3d& p : {line.a, line.b}) inside = inside && std::abs(p[0]) <= 10.0 && std::abs(p[2]) <= 10.0;
    }
    CHECK(flat, "Y-up: on the XZ ground");
    CHECK(inside, "within half the size");
    const auto axisX = std::find_if(lines.begin(), lines.end(), [](const GridLine& l) { return l.kind == GridLineKind::AxisX; });
    CHECK(axisX->a == GfVec3d(-10, 0, 0) && axisX->b == GfVec3d(10, 0, 0), "the X axis runs along X through the origin");
    const auto axisZ = std::find_if(lines.begin(), lines.end(), [](const GridLine& l) { return l.kind == GridLineKind::AxisOther; });
    CHECK(axisZ->a == GfVec3d(0, 0, -10) && axisZ->b == GfVec3d(0, 0, 10), "the other axis is Z");

    // Z-up: the ground is XY, the other axis Y.
    const std::vector<GridLine> zUp = buildViewportGrid(20.0, 1.0, /*zUp=*/true);
    bool onXY = true;
    for (const GridLine& line : zUp) onXY = onXY && line.a[2] == 0.0 && line.b[2] == 0.0;
    CHECK(onXY && zUp.size() == 42, "Z-up: on the XY ground");
    const auto axisY = std::find_if(zUp.begin(), zUp.end(), [](const GridLine& l) { return l.kind == GridLineKind::AxisOther; });
    CHECK(axisY->a == GfVec3d(0, -10, 0) && axisY->b == GfVec3d(0, 10, 0), "the other axis is Y");

    // A size that isn't a whole number of steps: the grid stops at the last whole step.
    const std::vector<GridLine> ragged = buildViewportGrid(5.0, 1.0);
    CHECK(ragged.size() == 10, "lines at -2..2: %zu", ragged.size());
    CHECK(ragged.front().a[0] == -2.0, "a square ending on a line: %f", ragged.front().a[0]);
    // Fractions that don't add up exactly in binary still reach the edge.
    CHECK(buildViewportGrid(1.0, 0.1).size() == 22, "1 wide by 0.1: 11 lines each way");
}

USDCC_TEST(viewport_grid_limits) {
    CHECK(buildViewportGrid(0.0, 1.0).empty() && buildViewportGrid(10.0, 0.0).empty() &&
              buildViewportGrid(-1.0, 1.0).empty() && buildViewportGrid(10.0, -1.0).empty() &&
              buildViewportGrid(std::nan(""), 1.0).empty(),
          "nothing for a size or step that isn't positive");
    CHECK(buildViewportGrid(1.0, 5.0).empty(), "nothing for a step wider than half the grid");

    // Coarsened by whole multiples so it stays within the cap, and the axes stay on it.
    CHECK(effectiveGridStep(20.0, 1.0) == 1.0, "a step that fits is used as is");
    const double step = effectiveGridStep(100000.0, 0.01);
    CHECK(step > 0.01 && std::abs(step / 0.01 - std::round(step / 0.01)) < 1e-6, "a whole multiple: %f", step);
    const std::vector<GridLine> fine = buildViewportGrid(100000.0, 0.01);
    CHECK(fine.size() <= 2 * static_cast<std::size_t>(kMaxGridLinesPerDirection) && fine.size() > 100,
          "capped: %zu lines", fine.size());
    CHECK(countOf(fine, GridLineKind::AxisX) == 1, "still has its axes");
}
