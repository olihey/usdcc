#include "usdcc/usd/viewport_grid.h"

#include <cmath>

namespace usdcc::usd {

namespace {
// How many steps fit in half the size, with a little slack so that a size that is a
// whole multiple of the step (20 and 1, 1 and 0.1) still reaches its edge.
long long stepsPerSide(double size, double step) { return static_cast<long long>(std::floor(size / 2.0 / step + 1e-9)); }
}  // namespace

double effectiveGridStep(double size, double step) {
    if (!(size > 0.0) || !(step > 0.0) || !std::isfinite(size) || !std::isfinite(step)) return 0.0;
    constexpr long long kMaxPerSide = (kMaxGridLinesPerDirection - 1) / 2;
    const long long perSide = stepsPerSide(size, step);
    if (perSide <= kMaxPerSide) return step;
    const double multiple = std::ceil(static_cast<double>(perSide) / static_cast<double>(kMaxPerSide));
    return step * multiple;
}

std::vector<GridLine> buildViewportGrid(double size, double step, bool zUp) {
    const double used = effectiveGridStep(size, step);
    if (used <= 0.0) return {};
    const long long perSide = stepsPerSide(size, used);
    if (perSide <= 0) return {};  // a step wider than half the grid: nothing but its centre
    const double extent = static_cast<double>(perSide) * used;

    // The ground's two axes: X and the other one (Z for Y-up, Y for Z-up).
    const auto point = [zUp](double x, double other) {
        return zUp ? PXR_NS::GfVec3d(x, other, 0.0) : PXR_NS::GfVec3d(x, 0.0, other);
    };
    const auto kindAt = [](long long index, GridLineKind axis) {
        if (index == 0) return axis;
        return index % 10 == 0 ? GridLineKind::Major : GridLineKind::Minor;
    };

    std::vector<GridLine> lines;
    lines.reserve(static_cast<std::size_t>(4 * perSide + 2));
    for (long long i = -perSide; i <= perSide; ++i) {
        const double offset = static_cast<double>(i) * used;
        // Parallel to X, at `offset` along the other axis: the X axis itself at 0.
        lines.push_back({point(-extent, offset), point(extent, offset), kindAt(i, GridLineKind::AxisX)});
        // Parallel to the other axis, at `offset` along X.
        lines.push_back({point(offset, -extent), point(offset, extent), kindAt(i, GridLineKind::AxisOther)});
    }
    return lines;
}

}  // namespace usdcc::usd
