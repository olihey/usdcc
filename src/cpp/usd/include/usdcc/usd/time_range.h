#pragma once

// A stage's time range and frame rate (docs/ANIMATION.md §5.1), in time codes. Qt-free.

#include <pxr/usd/usd/common.h>

namespace usdcc::usd {

struct TimeRange {
    double start = 0.0;
    double end = 0.0;

    double length() const { return end - start; }
    bool contains(double time) const { return time >= start && time <= end; }
    bool operator==(const TimeRange&) const = default;
};

// What a stage with no authored range plays: 0 to 100.
inline constexpr TimeRange kDefaultTimeRange{0.0, 100.0};
inline constexpr double kDefaultTimeCodesPerSecond = 24.0;

// `range` with its ends in order.
TimeRange ordered(TimeRange range);
// The smallest range holding both.
TimeRange united(const TimeRange& a, const TimeRange& b);
// `time` moved inside `range`.
double clampedTo(double time, const TimeRange& range);
// The whole frame nearest `time`: a frame is one time code.
double snappedToFrame(double time);

// Whether the stage's start and end time codes are both authored (root or session layer).
bool hasAuthoredTimeRange(const PXR_NS::UsdStagePtr& stage);
// The stage's startTimeCode to endTimeCode; kDefaultTimeRange when they aren't both
// authored, or are the wrong way round.
TimeRange stageTimeRange(const PXR_NS::UsdStagePtr& stage);
// How many time codes play in a second: the stage's timeCodesPerSecond, 24 when it has
// none or one that can't be (zero or less).
double timeCodesPerSecond(const PXR_NS::UsdStagePtr& stage);

}  // namespace usdcc::usd
