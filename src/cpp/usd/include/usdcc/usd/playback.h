#pragma once

// Where playback goes next (docs/ANIMATION.md §4.7), as plain arithmetic on time codes:
// PlaybackController supplies the clock and the stage. Playback shows whole frames, the
// ones inside the range. Qt-free.

#include "usdcc/usd/time_range.h"

namespace usdcc::usd {

enum class PlaybackMode {
    EveryFrame,  // each frame in turn, however long it takes to draw
    RealTime,    // the frame the clock says, skipping what there was no time to draw
};

struct PlaybackStep {
    double time = 0.0;      // the frame to show
    bool finished = false;  // it was the last one, and playback doesn't loop
};

// The first and the last whole frame inside `range`. A range that holds none gives its
// start for both.
TimeRange playbackFrames(const TimeRange& range);

// Every Frame: the frame after `time`. Past the last one it goes back to the first when
// looping, and otherwise stays on the last and is finished. A time before the range
// goes to the first frame.
PlaybackStep nextFrame(double time, const TimeRange& range, bool loops);

// Real Time: the frame to show `elapsedSeconds` after playback started on `startTime`,
// at `timeCodesPerSecond`. Looping wraps round the range; otherwise it stops on the last
// frame.
PlaybackStep frameAfter(double startTime, double elapsedSeconds, double timeCodesPerSecond, const TimeRange& range,
                        bool loops);

// One frame on or back from `time` (`frames`: 1 or -1, or more), kept inside the range:
// a time between two frames goes to the nearer one in that direction.
double steppedFrame(double time, int frames, const TimeRange& range);

}  // namespace usdcc::usd
