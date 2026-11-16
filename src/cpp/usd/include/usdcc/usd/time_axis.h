#pragma once

// The mapping between time and pixels that the timeline's ruler, and later the dope sheet
// and the curve editor, share (docs/ANIMATION.md §5.5): where a time is drawn, which time
// is under the cursor, zooming, panning, snapping to frames, and how far apart the ticks
// are. Plain data, no Qt: it is tested without a window.

#include "usdcc/core/export.h"

#include "usdcc/usd/time_range.h"

#include <vector>

namespace usdcc::usd {

class USDCC_USD_UI_API TimeAxis {
public:
    // The times shown: `range.start` is drawn at left() and `range.end` at right(). A
    // range of no length is shown as one time code long.
    void setRange(const TimeRange& range);
    const TimeRange& range() const { return m_range; }

    // Where the range is drawn, in pixels: its two ends. `right` no further left than
    // `left` + 1.
    void setExtent(double left, double right);
    double left() const { return m_left; }
    double right() const { return m_right; }

    double pixelsPerTimeCode() const;
    double pixelAt(double time) const;
    double timeAt(double pixel) const;
    // The whole frame nearest `pixel`.
    double frameAt(double pixel) const;

    // Shows `factor` times as much time (below 1: zooms in), the time at `anchorPixel`
    // staying under it. Never closer than a tenth of a time code across the extent.
    void zoom(double factor, double anchorPixel);
    // Moves what's shown by `pixels`: positive drags the times to the right, showing
    // earlier ones.
    void pan(double pixels);

    // How far apart ticks are, in time codes: the labelled ones at least `labelPixels`
    // apart, the others at least `tickPixels` apart and dividing them. Whole frames, from
    // 1, 2, 5, 10, 20, 50, ...
    struct TickSteps {
        double major = 1.0;
        double minor = 1.0;
    };
    TickSteps tickSteps(double labelPixels, double tickPixels) const;
    // The multiples of `step` inside the range, in order.
    std::vector<double> ticks(double step) const;

private:
    TimeRange m_range{0.0, 1.0};
    double m_left = 0.0;
    double m_right = 1.0;
};

}  // namespace usdcc::usd
