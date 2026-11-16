// Time without a window (docs/ANIMATION.md phase 1): the time and playback range a Stage
// keeps, a stage's own range and frame rate, where playback goes next, and the time axis
// the timeline's ruler draws with.
#include "test_harness.h"

#include "usdcc/usd/playback.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/time_axis.h"
#include "usdcc/usd/time_range.h"

#include <pxr/usd/usd/stage.h>

#include <cmath>
#include <limits>
#include <vector>

using namespace PXR_NS;
using namespace usdcc::usd;

namespace {
bool isNear(double a, double b) { return std::abs(a - b) < 1e-9; }
}  // namespace

USDCC_TEST(stage_time) {
    StageManager manager;
    const StageRefPtr stage = manager.createStage();
    CHECK(stage->time() == 0.0, "a stage with no start time code starts at 0 (%g)", stage->time());
    CHECK(!stage->timeCode().IsDefault() && stage->timeCode().GetValue() == 0.0, "a numeric time, never the default one");

    std::vector<double> times;
    auto connection = stage->timeChanged.connect([&](double time) { times.push_back(time); });
    stage->setTime(12.0);
    CHECK(stage->time() == 12.0 && times == std::vector<double>{12.0}, "setTime() moves it and says so once");
    stage->setTime(12.0);
    CHECK(times.size() == 1, "the same time again says nothing");
    stage->setTime(std::numeric_limits<double>::quiet_NaN());
    stage->setTime(std::numeric_limits<double>::infinity());
    CHECK(stage->time() == 12.0 && times.size() == 1, "not a number: ignored");
    stage->setTime(12.5);
    CHECK(stage->timeCode() == UsdTimeCode(12.5), "between two frames is a time too");

    // Nothing in the stage changed: no change notice, so no undo step and no dirty layer.
    int changes = 0;
    auto changed = stage->changed.connect([&](const StageChanges&) { ++changes; });
    stage->setTime(3.0);
    stage->setPlaybackRange({1.0, 5.0});
    CHECK(changes == 0 && !stage->usdStage()->GetRootLayer()->IsDirty(), "time and range are view state");

    // A stage that has a range starts at its start.
    const UsdStageRefPtr ranged = UsdStage::CreateInMemory();
    ranged->SetStartTimeCode(10.0);
    ranged->SetEndTimeCode(48.0);
    const StageRefPtr second = manager.adoptStage(ranged);
    CHECK(second->time() == 10.0, "a stage starts at its start time code (%g)", second->time());
    CHECK(stage->time() == 3.0, "each stage has its own time");
}

USDCC_TEST(stage_time_range) {
    const UsdStageRefPtr usd = UsdStage::CreateInMemory();
    CHECK(!hasAuthoredTimeRange(usd) && stageTimeRange(usd) == kDefaultTimeRange, "no range: 0 to 100");
    CHECK(timeCodesPerSecond(usd) == 24.0, "no frame rate: 24");
    usd->SetStartTimeCode(5.0);
    CHECK(!hasAuthoredTimeRange(usd) && stageTimeRange(usd) == kDefaultTimeRange, "a start alone isn't a range");
    usd->SetEndTimeCode(30.0);
    CHECK(hasAuthoredTimeRange(usd) && stageTimeRange(usd) == (TimeRange{5.0, 30.0}), "start and end: the stage's range");
    usd->SetEndTimeCode(1.0);
    CHECK(stageTimeRange(usd) == kDefaultTimeRange, "an end before the start: the default again");
    usd->SetEndTimeCode(30.0);
    usd->SetTimeCodesPerSecond(30.0);
    CHECK(timeCodesPerSecond(usd) == 30.0, "the stage's frame rate");
    usd->SetTimeCodesPerSecond(0.0);
    CHECK(timeCodesPerSecond(usd) == 24.0, "a rate that can't be: 24");

    StageManager manager;
    const StageRefPtr stage = manager.adoptStage(usd);
    int rangeChanges = 0;
    auto connection = stage->playbackRangeChanged.connect([&]() { ++rangeChanges; });
    CHECK(!stage->hasPlaybackRange() && stage->playbackRange() == (TimeRange{5.0, 30.0}), "playback follows the stage's range");
    usd->SetEndTimeCode(40.0);
    CHECK(stage->playbackRange().end == 40.0, "as it changes");
    stage->setPlaybackRange({20.0, 10.0});
    CHECK(stage->hasPlaybackRange() && stage->playbackRange() == (TimeRange{10.0, 20.0}) && rangeChanges == 1,
          "a range of its own, its ends put in order");
    stage->setPlaybackRange({10.0, 20.0});
    CHECK(rangeChanges == 1, "the same range again says nothing");
    usd->SetEndTimeCode(50.0);
    CHECK(stage->playbackRange().end == 20.0 && usd->GetEndTimeCode() == 50.0, "the two are apart now");
    stage->clearPlaybackRange();
    CHECK(!stage->hasPlaybackRange() && stage->playbackRange() == (TimeRange{5.0, 50.0}) && rangeChanges == 2,
          "cleared: the stage's again");

    CHECK(snappedToFrame(4.4) == 4.0 && snappedToFrame(4.5) == 5.0 && snappedToFrame(-0.4) == 0.0, "the nearest frame");
    CHECK(clampedTo(7.0, {10.0, 20.0}) == 10.0 && clampedTo(25.0, {10.0, 20.0}) == 20.0 && clampedTo(12.0, {20.0, 10.0}) == 12.0,
          "kept inside a range");
    CHECK(united({0.0, 100.0}, {-10.0, 40.0}) == (TimeRange{-10.0, 100.0}), "the range holding two");
}

USDCC_TEST(playback_steps) {
    const TimeRange range{1.0, 4.0};
    CHECK(playbackFrames({0.5, 4.5}) == (TimeRange{1.0, 4.0}), "the whole frames inside a range");
    CHECK(playbackFrames({2.2, 2.6}) == (TimeRange{2.2, 2.2}), "a range that holds none: its start");

    // Every Frame.
    CHECK(nextFrame(1.0, range, true).time == 2.0 && !nextFrame(1.0, range, true).finished, "the next frame");
    CHECK(nextFrame(2.5, range, true).time == 3.0, "from between two: the next whole one");
    CHECK(nextFrame(-3.0, range, true).time == 1.0, "from before the range: its first frame");
    const PlaybackStep wrapped = nextFrame(4.0, range, true);
    CHECK(wrapped.time == 1.0 && !wrapped.finished, "looping: past the last frame comes the first");
    const PlaybackStep ended = nextFrame(4.0, range, false);
    CHECK(ended.time == 4.0 && ended.finished, "not looping: it stays on the last and is finished");
    double time = 1.0;
    std::vector<double> played;
    for (int i = 0; i < 7; ++i) played.push_back(time = nextFrame(time, range, true).time);
    CHECK(played == (std::vector<double>{2, 3, 4, 1, 2, 3, 4}), "every frame, round and round");

    // Real Time, at 24 frames a second.
    CHECK(frameAfter(1.0, 0.0, 24.0, range, true).time == 1.0, "no time passed: still the first frame");
    CHECK(frameAfter(1.0, 1.0 / 24.0 + 1e-6, 24.0, range, true).time == 2.0, "a frame's time later: the next");
    CHECK(frameAfter(1.0, 0.09, 24.0, range, true).time == 3.0, "frames are skipped when time ran on (0.09 s: 2 frames)");
    const PlaybackStep late = frameAfter(1.0, 1.0, 24.0, range, false);
    CHECK(late.time == 4.0 && late.finished, "not looping: the last frame, finished");
    // 24 frames after frame 1 in a 4-frame loop: six times round, back on frame 1.
    CHECK(frameAfter(1.0, 1.0 + 1e-6, 24.0, range, true).time == 1.0, "looping wraps round the range");
    CHECK(frameAfter(1.0, (5.0 + 1e-6) / 24.0, 24.0, range, true).time == 2.0, "five frames on: 1 2 3 4 1 2");

    // Stepping.
    CHECK(steppedFrame(2.0, 1, range) == 3.0 && steppedFrame(2.0, -1, range) == 1.0, "a frame on and back");
    CHECK(steppedFrame(2.5, 1, range) == 3.0 && steppedFrame(2.5, -1, range) == 2.0, "from between two: the nearer in that direction");
    CHECK(steppedFrame(4.0, 1, range) == 4.0 && steppedFrame(1.0, -1, range) == 1.0, "not past the ends");
    CHECK(steppedFrame(40.0, -1, range) == 4.0, "from outside the range: into it");
}

USDCC_TEST(time_axis) {
    TimeAxis axis;
    axis.setRange({0.0, 100.0});
    axis.setExtent(10.0, 510.0);
    CHECK(isNear(axis.pixelsPerTimeCode(), 5.0), "5 pixels a frame (%g)", axis.pixelsPerTimeCode());
    CHECK(isNear(axis.pixelAt(0.0), 10.0) && isNear(axis.pixelAt(100.0), 510.0) && isNear(axis.pixelAt(24.0), 130.0), "time to pixel");
    CHECK(isNear(axis.timeAt(130.0), 24.0) && isNear(axis.timeAt(10.0), 0.0), "and back");
    CHECK(axis.frameAt(131.0) == 24.0 && axis.frameAt(133.0) == 25.0, "the frame nearest a pixel");

    // Ticks: labelled ones far enough apart for their numbers, the others dividing them.
    TimeAxis::TickSteps steps = axis.tickSteps(40.0, 5.0);
    CHECK(steps.major == 10.0 && steps.minor == 1.0, "at 5 px a frame: every 10 labelled, every frame ticked (%g, %g)",
          steps.major, steps.minor);
    CHECK(axis.ticks(10.0).size() == 11 && axis.ticks(10.0).front() == 0.0 && axis.ticks(10.0).back() == 100.0,
          "0, 10, ... 100");
    steps = axis.tickSteps(30.0, 12.0);
    CHECK(steps.major == 10.0 && steps.minor == 5.0, "ticks further apart: every 5 (%g, %g)", steps.major, steps.minor);
    axis.setRange({0.0, 1000.0});
    steps = axis.tickSteps(40.0, 5.0);
    CHECK(steps.major == 100.0 && steps.minor == 10.0, "a long range: 100 and 10 (%g, %g)", steps.major, steps.minor);
    axis.setRange({0.0, 20.0});
    steps = axis.tickSteps(40.0, 5.0);
    CHECK(steps.major == 2.0 && steps.minor == 1.0, "a short one: never less than a frame (%g, %g)", steps.major, steps.minor);
    axis.setRange({3.5, 12.2});
    CHECK(axis.ticks(5.0) == (std::vector<double>{5.0, 10.0}), "only the ticks inside the range");

    // Zooming keeps the time under the cursor where it is; panning moves everything.
    axis.setRange({0.0, 100.0});
    axis.zoom(0.5, 130.0);
    CHECK(isNear(axis.range().length(), 50.0) && isNear(axis.timeAt(130.0), 24.0), "zoomed in about frame 24 (%g to %g)",
          axis.range().start, axis.range().end);
    axis.zoom(2.0, 130.0);
    CHECK(isNear(axis.range().start, 0.0) && isNear(axis.range().end, 100.0), "and out again");
    axis.pan(50.0);
    CHECK(isNear(axis.range().start, -10.0) && isNear(axis.range().end, 90.0), "dragged 50 px right: 10 frames earlier");
    axis.zoom(1e-9, 130.0);
    CHECK(axis.range().length() >= 0.1 - 1e-12, "never closer than a tenth of a frame across");

    axis.setRange({7.0, 7.0});
    CHECK(axis.range().length() == 1.0 && std::isfinite(axis.pixelAt(7.5)), "a range of no length still maps");
    axis.setRange({9.0, 2.0});
    CHECK(axis.range() == (TimeRange{2.0, 9.0}), "a range the wrong way round is put in order");
}
