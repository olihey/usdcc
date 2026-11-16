#include "usdcc/usd/time_range.h"

#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

TimeRange ordered(TimeRange range) {
    if (range.end < range.start) std::swap(range.start, range.end);
    return range;
}

TimeRange united(const TimeRange& a, const TimeRange& b) {
    return {std::min(a.start, b.start), std::max(a.end, b.end)};
}

double clampedTo(double time, const TimeRange& range) {
    const TimeRange r = ordered(range);
    return std::clamp(time, r.start, r.end);
}

double snappedToFrame(double time) { return std::round(time); }

bool hasAuthoredTimeRange(const PXR_NS::UsdStagePtr& stage) { return stage && stage->HasAuthoredTimeCodeRange(); }

TimeRange stageTimeRange(const PXR_NS::UsdStagePtr& stage) {
    if (!hasAuthoredTimeRange(stage)) return kDefaultTimeRange;
    const TimeRange range{stage->GetStartTimeCode(), stage->GetEndTimeCode()};
    return range.end < range.start ? kDefaultTimeRange : range;
}

double timeCodesPerSecond(const PXR_NS::UsdStagePtr& stage) {
    const double rate = stage ? stage->GetTimeCodesPerSecond() : kDefaultTimeCodesPerSecond;
    return std::isfinite(rate) && rate > 0.0 ? rate : kDefaultTimeCodesPerSecond;
}

}  // namespace usdcc::usd
