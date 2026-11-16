#include "usdcc/usd/playback.h"

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

TimeRange playbackFrames(const TimeRange& range) {
    const TimeRange r = ordered(range);
    const double first = std::ceil(r.start);
    const double last = std::floor(r.end);
    return last < first ? TimeRange{r.start, r.start} : TimeRange{first, last};
}

PlaybackStep nextFrame(double time, const TimeRange& range, bool loops) {
    const TimeRange frames = playbackFrames(range);
    if (time < frames.start) return {frames.start, false};
    const double next = std::floor(time) + 1.0;
    if (next <= frames.end) return {next, false};
    return loops ? PlaybackStep{frames.start, false} : PlaybackStep{frames.end, true};
}

PlaybackStep frameAfter(double startTime, double elapsedSeconds, double timeCodesPerSecond, const TimeRange& range,
                        bool loops) {
    const TimeRange frames = playbackFrames(range);
    const double start = std::clamp(std::floor(startTime), frames.start, frames.end);
    const double played = std::floor(std::max(0.0, elapsedSeconds) * timeCodesPerSecond);
    const double time = start + played;
    if (time <= frames.end) return {time, false};
    if (!loops) return {frames.end, true};
    const double count = frames.length() + 1.0;  // frames in the range
    return {frames.start + std::fmod(time - frames.start, count), false};
}

double steppedFrame(double time, int frames, const TimeRange& range) {
    const TimeRange inside = playbackFrames(range);
    if (frames == 0) return std::clamp(time, inside.start, inside.end);
    // From between two frames, the first step lands on the next whole one.
    const double from = frames > 0 ? std::floor(time) : std::ceil(time);
    return std::clamp(from + frames, inside.start, inside.end);
}

}  // namespace usdcc::usd
