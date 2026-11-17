// The timeline (docs/ANIMATION.md phase 1): the ruler and the range slider, the transport
// on the current stage, the bar under the dock area, and what follows the stage's time:
// the viewport, its HUD, the Attribute Editor and a session. Runs in usdcc_ui_tests (Qt's
// offscreen platform); the viewport is never shown, so nothing here needs GL.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/side_panel.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/playback_controller.h"
#include "usdcc/usd/properties/number_field.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/session_stages.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/time_range_slider.h"
#include "usdcc/usd/time_ruler.h"
#include "usdcc/usd/timeline_bar.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_display.h"
#include "usdcc/usd/viewport_hud.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>

#include <DockManager.h>

#include <QAction>
#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <cmath>
#include <memory>
#include <vector>

using namespace usdcc::usd;
using usdcc::usd::properties::NumberField;
using usdcc::usd::properties::PropertyRow;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    for (int i = 0; i < 8; ++i) QApplication::processEvents();
}

// A UI scale of the test's own, gone with it.
struct Scale {
    QTemporaryDir folder;
    usdcc::ui::UiScaleController controller{*static_cast<QApplication*>(QCoreApplication::instance()),
                                            folder.filePath(QStringLiteral("ui.ini"))};
};

void mouse(QWidget* widget, QEvent::Type type, const QPointF& position, Qt::MouseButton button, Qt::MouseButtons buttons,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, position, widget->mapToGlobal(position), button, buttons, modifiers);
    QApplication::sendEvent(widget, &event);
}
void press(QWidget* widget, double x, Qt::MouseButton button = Qt::LeftButton) {
    mouse(widget, QEvent::MouseButtonPress, QPointF(x, widget->height() / 2.0), button, button);
}
void drag(QWidget* widget, double x, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    mouse(widget, QEvent::MouseMove, QPointF(x, widget->height() / 2.0), Qt::NoButton, Qt::LeftButton, modifiers);
}
void release(QWidget* widget, double x) {
    mouse(widget, QEvent::MouseButtonRelease, QPointF(x, widget->height() / 2.0), Qt::LeftButton, Qt::NoButton);
}

QAction* actionNamed(const QMenu& menu, const char* name) {
    for (QAction* action : menu.actions()) {
        if (action->objectName() == QLatin1String(name)) return action;
    }
    return nullptr;
}

bool isNear(double a, double b) { return std::abs(a - b) < 1e-6; }
}  // namespace

USDCC_TEST(time_ruler_scrubs) {
    Scale scale;
    CHECK(formatFrame(24.0) == "24" && formatFrame(24.5) == "24.5" && formatFrame(-3.0) == "-3" && formatFrame(0.004) == "0",
          "frame numbers: whole when they are");

    TimeRuler ruler;
    ruler.setRange({0.0, 100.0});
    ruler.resize(524, 24);
    std::vector<double> scrubbed;
    int started = 0;
    int finished = 0;
    QObject::connect(&ruler, &TimeRuler::scrubStarted, &ruler, [&]() { ++started; });
    QObject::connect(&ruler, &TimeRuler::timeScrubbed, &ruler, [&](double time) { scrubbed.push_back(time); });
    QObject::connect(&ruler, &TimeRuler::scrubFinished, &ruler, [&]() { ++finished; });
    const auto x = [&](double time) { return ruler.axis().pixelAt(time); };
    CHECK(x(100.0) - x(0.0) > 400.0, "the range spans the ruler (%g px)", x(100.0) - x(0.0));

    press(&ruler, x(40.0) + 1.0);
    CHECK(started == 1 && scrubbed == std::vector<double>{40.0}, "a press scrubs to the frame under it");
    drag(&ruler, x(60.3));
    CHECK(scrubbed.size() == 2 && scrubbed.back() == 60.0, "a drag follows, on whole frames (%g)", scrubbed.back());
    drag(&ruler, x(60.4));
    CHECK(scrubbed.size() == 2, "the same frame again says nothing");
    drag(&ruler, x(60.4), Qt::ShiftModifier);
    CHECK(scrubbed.size() == 3 && isNear(scrubbed.back(), 60.4), "with Shift: any time (%g)", scrubbed.back());
    drag(&ruler, ruler.width() + 300.0);
    CHECK(scrubbed.back() == 100.0, "past the end: the last frame (%g)", scrubbed.back());
    drag(&ruler, -300.0);
    CHECK(scrubbed.back() == 0.0, "before the start: the first");
    release(&ruler, x(10.0));
    CHECK(finished == 1, "the release ends it");
    const size_t count = scrubbed.size();
    drag(&ruler, x(70.0));
    press(&ruler, x(70.0), Qt::RightButton);
    CHECK(scrubbed.size() == count && started == 1, "nothing without the left button down");

    // A range whose ends fall between frames: the frames inside it.
    ruler.setRange({0.5, 10.5});
    press(&ruler, -50.0);
    CHECK(scrubbed.back() == 1.0, "the first whole frame of the range (%g)", scrubbed.back());
    drag(&ruler, ruler.width() + 50.0);
    CHECK(scrubbed.back() == 10.0, "and the last (%g)", scrubbed.back());
    release(&ruler, 0.0);

    ruler.setTime(5.0);
    CHECK(ruler.time() == 5.0, "the playhead is where it's put");
    QPixmap painted = ruler.grab();
    CHECK(!painted.isNull(), "it paints");

    // The part that plays is lit and the rest dimmed, on the one scale.
    ruler.setRange({0.0, 100.0});
    ruler.setPlaybackRange({0.0, 100.0});
    ruler.setTime(-1.0);  // no playhead in the way
    const QImage whole = ruler.grab().toImage();
    ruler.setPlaybackRange({40.0, 60.0});
    CHECK(ruler.playbackRange() == (TimeRange{40.0, 60.0}) && ruler.range() == (TimeRange{0.0, 100.0}),
          "the playback range is a part of what's shown, not what's shown");
    const QImage part = ruler.grab().toImage();
    const auto column = [&](double time) { return static_cast<int>(std::round(x(time) * part.devicePixelRatio())); };
    const auto sameBetween = [&](double from, double to) {
        for (int px = column(from); px <= column(to); ++px) {
            for (int py = 0; py < part.height(); ++py) {
                if (part.pixel(px, py) != whole.pixel(px, py)) return false;
            }
        }
        return true;
    };
    CHECK(sameBetween(42.0, 58.0), "the frames that play look as they did");
    CHECK(!sameBetween(5.0, 35.0) && !sameBetween(65.0, 95.0), "the frames before and after are dimmed");
    press(&ruler, x(80.0));
    CHECK(scrubbed.back() == 80.0, "scrubbing still reaches a frame that doesn't play");
    release(&ruler, x(80.0));
    ruler.setEnabled(false);
    painted = ruler.grab();
    CHECK(!painted.isNull(), "greyed out too");
}

USDCC_TEST(time_range_slider_drags) {
    Scale scale;
    TimeRangeSlider slider;
    slider.setFullRange({0.0, 100.0});
    slider.setRange({20.0, 60.0});
    slider.resize(524, 12);
    std::vector<TimeRange> edits;
    // As the timeline does it: the edit goes to the stage, which hands it back.
    QObject::connect(&slider, &TimeRangeSlider::rangeEdited, &slider, [&](const TimeRange& range) {
        edits.push_back(range);
        slider.setRange(range);
    });
    const double perFrame = slider.barRect().width() / 40.0;
    CHECK(perFrame > 3.0, "the bar is 40 frames wide (%g px a frame)", perFrame);

    // The end.
    double from = slider.barRect().right() - 1.0;
    press(&slider, from);
    drag(&slider, from + 10.0 * perFrame);
    CHECK(slider.range() == (TimeRange{20.0, 70.0}), "dragging the end moves the end (%g to %g)", slider.range().start,
          slider.range().end);
    drag(&slider, from + 500.0 * perFrame);
    CHECK(slider.range() == (TimeRange{20.0, 100.0}), "no further than the track");
    drag(&slider, from - 500.0 * perFrame);
    CHECK(slider.range() == (TimeRange{20.0, 21.0}), "and never less than a frame from the start");
    drag(&slider, from);
    release(&slider, from);
    CHECK(slider.range() == (TimeRange{20.0, 60.0}), "back where it was");

    // The start.
    from = slider.barRect().left() + 1.0;
    press(&slider, from);
    drag(&slider, from - 5.0 * perFrame);
    CHECK(slider.range() == (TimeRange{15.0, 60.0}), "dragging the start moves the start");
    drag(&slider, from - 500.0 * perFrame);
    CHECK(slider.range().start == 0.0, "to the track's start");
    release(&slider, from);

    // The middle: both ends, the length kept.
    slider.setRange({20.0, 60.0});
    from = slider.barRect().center().x();
    press(&slider, from);
    drag(&slider, from + 5.0 * perFrame);
    CHECK(slider.range() == (TimeRange{25.0, 65.0}), "dragging the middle moves both (%g to %g)", slider.range().start,
          slider.range().end);
    drag(&slider, from + 500.0 * perFrame);
    CHECK(slider.range() == (TimeRange{60.0, 100.0}), "until an end meets the track's");
    release(&slider, from);

    const size_t count = edits.size();
    press(&slider, 2.0);
    drag(&slider, 80.0);
    release(&slider, 80.0);
    CHECK(edits.size() == count, "a press off the bar drags nothing");
    CHECK(!slider.grab().isNull(), "it paints");
}

USDCC_TEST(playback_controller) {
    TestSettings settings;
    StageManager stages;
    const StageRefPtr stage = stages.createStage();
    stage->setPlaybackRange({1.0, 4.0});
    {
        PlaybackController playback(&stages);
        int playingChanges = 0;
        int stageChanges = 0;
        QObject::connect(&playback, &PlaybackController::playingChanged, &playback, [&]() { ++playingChanges; });
        QObject::connect(&playback, &PlaybackController::stageChanged, &playback, [&]() { ++stageChanges; });
        CHECK(playback.stage() == stage && !playback.isPlaying(), "on the current stage, stopped");
        CHECK(playback.loops() && playback.mode() == PlaybackMode::EveryFrame, "looping, every frame, to start with");

        // Every Frame, looping.
        playback.play();
        CHECK(playback.isPlaying() && stage->time() == 1.0 && playingChanges == 1, "play: from the range's first frame (%g)",
              stage->time());
        std::vector<double> shown;
        for (int i = 0; i < 5; ++i) {
            playback.advance(0.0);
            shown.push_back(stage->time());
        }
        CHECK(shown == (std::vector<double>{2, 3, 4, 1, 2}), "every frame, round the range");
        CHECK(playback.isPlaying(), "and it goes on");

        // Not looping: it stops on the last frame, and Play then starts from the top.
        playback.setLoops(false);
        playback.advance(0.0);
        playback.advance(0.0);
        CHECK(stage->time() == 4.0 && playback.isPlaying(), "on the last frame");
        playback.advance(0.0);
        CHECK(stage->time() == 4.0 && !playback.isPlaying(), "past it: stopped there");
        playback.advance(0.0);
        CHECK(stage->time() == 4.0, "stopped, the clock moves nothing");
        playback.play();
        CHECK(playback.isPlaying() && stage->time() == 1.0, "Play on the last frame starts again");

        // Real Time: the frame the clock says.
        playback.setMode(PlaybackMode::RealTime);
        playback.advance(0.09);  // two frames' worth at 24 fps
        CHECK(stage->time() == 3.0, "frames are skipped when time ran on (%g)", stage->time());

        // Scrubbing and stepping stop it.
        playback.setTime(2.0);
        CHECK(!playback.isPlaying() && stage->time() == 2.0, "setting the time stops playing");
        playback.stepForward();
        CHECK(stage->time() == 3.0, "a frame on");
        playback.stepBackward();
        playback.stepBackward();
        playback.stepBackward();
        CHECK(stage->time() == 1.0, "back, to the first frame and no further");
        playback.goToEnd();
        CHECK(stage->time() == 4.0, "the last frame");
        playback.goToStart();
        CHECK(stage->time() == 1.0, "the first");
        playback.togglePlaying();
        playback.stepForward();
        CHECK(!playback.isPlaying(), "stepping stops playing too");

        // Another stage becomes current: playing stops, and the transport is on that one.
        playback.play();
        const StageRefPtr second = stages.createStage();
        stages.setCurrentStage(second);
        CHECK(playback.stage() == second && !playback.isPlaying() && stageChanges == 1, "it follows the current stage");
        const double firstTime = stage->time();
        playback.setTime(9.0);
        CHECK(second->time() == 9.0 && stage->time() == firstTime, "each stage keeps its own time");
        stages.closeStage(second);
        CHECK(playback.stage() == stage, "back on the stage that's left");
        stages.closeStage(stage);
        CHECK(!playback.stage(), "no stage, no transport");
        playback.play();
        playback.stepForward();
        CHECK(!playback.isPlaying(), "and nothing to play");
    }
    {
        // What the user chose is kept (usdcc.ini).
        PlaybackController again(&stages);
        CHECK(!again.loops() && again.mode() == PlaybackMode::RealTime, "Loop and the mode are remembered");
        again.setLoops(true);
        again.setMode(PlaybackMode::EveryFrame);
    }

    // The clock itself: a timer at the stage's frame rate.
    const StageRefPtr timed = stages.createStage();
    timed->setPlaybackRange({1.0, 200.0});
    PlaybackController playback(&stages);
    playback.play();
    QTest::qWait(300);
    playback.stop();
    CHECK(timed->time() > 2.0 && timed->time() < 20.0, "about 24 frames a second: %g frames in 0.3 s", timed->time() - 1.0);
    const double stoppedAt = timed->time();
    QTest::qWait(120);
    CHECK(timed->time() == stoppedAt, "stopped: the time stays");
}

USDCC_TEST(timeline_bar) {
    TestSettings settings;
    Scale scale;
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    PlaybackController playback(&stages);
    TimelineBar bar(&playback, undo);
    bar.resize(1000, 52);
    CHECK(!bar.ruler()->isEnabled() && !bar.frameField()->isEnabled() && !bar.playButton()->isEnabled(), "no stage: greyed out");

    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr usd = stage->usdStage();
    CHECK(bar.ruler()->isEnabled() && bar.frameField()->isEnabled() && bar.playButton()->isEnabled(), "a stage: it works");
    CHECK(bar.startField()->value() == 0.0 && bar.endField()->value() == 100.0, "a stage with no range: 0 to 100");
    CHECK(bar.ruler()->range() == kDefaultTimeRange && bar.rangeSlider()->fullRange() == kDefaultTimeRange, "on the ruler too");
    CHECK(bar.rateLabel()->text() == "24 fps", "the frame rate: %s", qPrintable(bar.rateLabel()->text()));

    // The stage's time, both ways.
    stage->setTime(12.0);
    CHECK(bar.frameField()->value() == 12.0 && bar.ruler()->time() == 12.0, "the stage's time shows");
    Q_EMIT bar.ruler()->timeScrubbed(30.0);
    CHECK(stage->time() == 30.0 && bar.frameField()->value() == 30.0, "scrubbing the ruler moves the stage");
    Q_EMIT bar.frameField()->editStarted();
    Q_EMIT bar.frameField()->valueEdited(41.6);
    CHECK(stage->time() == 42.0, "the frame field too, on whole frames (%g)", stage->time());
    Q_EMIT bar.frameField()->editCanceled();
    CHECK(stage->time() == 30.0, "Esc puts the frame back");
    stage->setTime(30.5);
    CHECK(bar.frameField()->text() == "30.50", "a time between two frames shows as one (%s)", qPrintable(bar.frameField()->text()));
    stage->setTime(30.0);
    CHECK(bar.frameField()->text() == "30", "a whole one as a whole one (%s)", qPrintable(bar.frameField()->text()));

    // The playback range: the fields and the slider.
    Q_EMIT bar.startField()->valueEdited(10.0);
    CHECK(stage->playbackRange() == (TimeRange{10.0, 100.0}) && bar.ruler()->range() == (TimeRange{10.0, 100.0}),
          "the start field sets where playback starts");
    CHECK(bar.rangeSlider()->fullRange() == (TimeRange{10.0, 100.0}) && bar.rangeSlider()->range() == (TimeRange{10.0, 100.0}),
          "a stage with no range of its own: a typed range is what both rows show");
    Q_EMIT bar.endField()->valueEdited(5.0);
    CHECK(stage->playbackRange() == (TimeRange{4.0, 5.0}), "an end before the start takes the start with it (%g to %g)",
          stage->playbackRange().start, stage->playbackRange().end);
    Q_EMIT bar.startField()->valueEdited(200.0);
    CHECK(stage->playbackRange() == (TimeRange{200.0, 201.0}), "and a start past the end the end");
    CHECK(bar.shownRange() == (TimeRange{200.0, 201.0}) && bar.ruler()->range() == bar.shownRange() &&
              bar.rangeSlider()->fullRange() == bar.shownRange(),
          "the ruler and the slider show the same times");

    // Dragging the slider changes what plays, not what's shown: the bar stays under the
    // frames it stands for, and a range made shorter can be made longer again.
    stage->clearPlaybackRange();
    CHECK(bar.shownRange() == kDefaultTimeRange, "back to the whole range");
    Q_EMIT bar.rangeSlider()->rangeEdited(TimeRange{20.0, 40.0});
    CHECK(stage->playbackRange() == (TimeRange{20.0, 40.0}) && bar.startField()->value() == 20.0 && bar.endField()->value() == 40.0,
          "dragging the slider sets both ends");
    CHECK(bar.ruler()->range() == kDefaultTimeRange && bar.rangeSlider()->fullRange() == kDefaultTimeRange,
          "the ruler keeps its scale, the slider's (%g to %g)", bar.ruler()->range().start, bar.ruler()->range().end);
    CHECK(bar.ruler()->playbackRange() == (TimeRange{20.0, 40.0}) && bar.rangeSlider()->range() == (TimeRange{20.0, 40.0}),
          "and lights the frames the bar lies under");
    usd->DefinePrim(SdfPath("/SomethingElse"));
    settle();
    CHECK(bar.shownRange() == kDefaultTimeRange, "an edit to the stage doesn't change what's shown");
    Q_EMIT bar.rangeSlider()->rangeEdited(TimeRange{20.0, 90.0});
    CHECK(stage->playbackRange() == (TimeRange{20.0, 90.0}) && bar.shownRange() == kDefaultTimeRange, "longer again, the same way");
    Q_EMIT bar.rangeSlider()->rangeEdited(TimeRange{20.0, 40.0});

    // The menu.
    {
        QMenu menu;
        bar.populateMenu(&menu);
        QAction* reset = actionNamed(menu, "timelineResetRange");
        QAction* author = actionNamed(menu, "timelineSetStageRange");
        CHECK(reset && reset->isEnabled() && author && author->isEnabled(), "a range of its own can be reset or written");
        if (reset) reset->trigger();
        CHECK(!stage->hasPlaybackRange() && bar.endField()->value() == 100.0, "Reset: the stage's range again");
    }
    {
        QMenu menu;
        bar.populateMenu(&menu);
        QAction* reset = actionNamed(menu, "timelineResetRange");
        CHECK(reset && !reset->isEnabled(), "nothing to reset now");
    }
    stage->setPlaybackRange({1.0, 48.0});
    bar.setStageRangeToPlaybackRange();
    CHECK(usd->HasAuthoredTimeCodeRange() && usd->GetStartTimeCode() == 1.0 && usd->GetEndTimeCode() == 48.0,
          "Set Stage Range writes the start and end time codes");
    CHECK(!stage->hasPlaybackRange() && stage->playbackRange() == (TimeRange{1.0, 48.0}), "and playback follows the stage again");
    CHECK(undo->stack(stage)->count() == 1 && undo->stack(stage)->undoText() == "Set Stage Range", "one undo step");
    undo->stack(stage)->undo();
    settle();
    CHECK(!usd->HasAuthoredTimeCodeRange() && bar.endField()->value() == 100.0, "undone: no range, and the timeline says so (%g)",
          bar.endField()->value());
    undo->stack(stage)->redo();
    settle();
    CHECK(bar.startField()->value() == 1.0 && bar.endField()->value() == 48.0, "redone: the timeline follows the stage's metadata");

    // A stage with a range of its own: all of it stays in view, whatever part plays.
    Q_EMIT bar.endField()->valueEdited(20.0);
    CHECK(stage->playbackRange() == (TimeRange{1.0, 20.0}) && bar.shownRange() == (TimeRange{1.0, 48.0}),
          "a shorter range typed in: the stage's range is still what's shown");
    CHECK(bar.ruler()->range() == bar.rangeSlider()->fullRange() && bar.ruler()->playbackRange() == (TimeRange{1.0, 20.0}),
          "on one scale, the part that plays lit");
    Q_EMIT bar.endField()->valueEdited(60.0);
    CHECK(bar.shownRange() == (TimeRange{1.0, 60.0}), "a range past the stage's end is shown too");
    stage->clearPlaybackRange();
    CHECK(bar.shownRange() == (TimeRange{1.0, 48.0}), "reset: the stage's range");

    // The frame rate is the stage's.
    usd->SetTimeCodesPerSecond(30.0);
    settle();
    CHECK(bar.rateLabel()->text() == "30 fps", "a new frame rate shows: %s", qPrintable(bar.rateLabel()->text()));

    // The transport.
    bar.playButton()->click();
    CHECK(playback.isPlaying() && bar.playButton()->toolTip() == "Stop", "Play plays, and becomes Stop");
    bar.playButton()->click();
    CHECK(!playback.isPlaying() && bar.playButton()->toolTip() == "Play", "and back");
    CHECK(bar.loopButton()->isChecked() == playback.loops(), "Loop shows what the transport does");
    bar.loopButton()->click();
    CHECK(bar.loopButton()->isChecked() == playback.loops() && !playback.loops(), "and switches it");
    bar.loopButton()->click();

    // Another stage: its time and range.
    const UsdStageRefPtr ranged = UsdStage::CreateInMemory();
    ranged->SetStartTimeCode(100.0);
    ranged->SetEndTimeCode(150.0);
    const StageRefPtr second = stages.adoptStage(ranged);
    stages.setCurrentStage(second);
    CHECK(bar.frameField()->value() == 100.0 && bar.startField()->value() == 100.0 && bar.endField()->value() == 150.0,
          "the current stage's time and range (%g, %g to %g)", bar.frameField()->value(), bar.startField()->value(),
          bar.endField()->value());
    stage->setTime(3.0);
    CHECK(bar.frameField()->value() == 100.0, "a stage that isn't current moves nothing here");
    stages.setCurrentStage(stage);
    CHECK(bar.frameField()->value() == 3.0, "back on the first: where it was left");

    stages.closeStage(second);
    stages.closeStage(stage);
    CHECK(!bar.ruler()->isEnabled() && bar.rateLabel()->text().isEmpty(), "no stage again: greyed out");
    CHECK(!bar.grab().isNull(), "it paints");
}

USDCC_TEST(main_window_bottom_bar) {
    TestSettings settings;
    Scale scale;
    usdcc::ui::MainWindow window;
    CHECK(window.dockManager()->parentWidget() == window.centralWidget(), "the dock area shares the centre");
    CHECK(!window.bottomBar() && !window.bottomBarAction(), "no bar until one is set");
    auto* bar = new QWidget;
    window.setBottomBar(bar, QStringLiteral("Timeline"));
    CHECK(window.bottomBar() == bar && bar->parentWidget() == window.centralWidget(), "the bar is under it");
    QAction* action = window.bottomBarAction();
    CHECK(action && action->isCheckable() && action->isChecked() && action->text() == "Timeline", "with a View entry, on");
    window.buildViewMenu();
    bool listed = false;
    for (QMenu* menu : window.findChildren<QMenu*>()) listed = listed || menu->actions().contains(action);
    CHECK(listed, "in the View menu");
    {
        // A side panel's title bar menu lists what the View menu does: the bar
        // too, which Qt's own (dock widgets and toolbars only) left out.
        auto* panel = new usdcc::ui::SidePanel(QStringLiteral("Notes"));
        window.addSidePanel(panel, Qt::RightDockWidgetArea);
        std::unique_ptr<QMenu> popup(window.createPopupMenu());
        const QList<QAction*> entries = popup ? popup->actions() : QList<QAction*>();
        CHECK(entries.contains(action), "the title bar menu has the bar");
        CHECK(entries.contains(panel->toggleViewAction()), "and the side panel added after the View menu");
        CHECK(!entries.contains(window.fullScreenAction()), "but not the View menu's other entries");
    }

    if (!action) return;
    action->setChecked(false);
    CHECK(bar->isHidden(), "unticked: hidden");
    const QJsonObject state = window.layoutState();
    CHECK(state.value("bottomBarVisible") == false, "a session remembers");
    action->setChecked(true);
    CHECK(!bar->isHidden(), "ticked: back");
    window.restoreLayoutState(state, false);
    CHECK(!action->isChecked() && bar->isHidden(), "and puts it back as it was");
    QJsonObject older = state;
    older.remove("bottomBarVisible");
    window.restoreLayoutState(older, false);
    CHECK(bar->isHidden(), "a session from before the bar leaves it alone");
}

USDCC_TEST(viewport_follows_stage_time) {
    TestSettings settings;
    defineViewportDisplaySettings();
    CHECK(formatHudFrame(12.0) == "Frame 12" && formatHudFrame(24.5) == "Frame 24.5", "the HUD's frame line");

    StageManager stages;
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    const StageRefPtr stage = stages.createStage();
    stage->usdStage()->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    auto panel = std::make_unique<ViewportViewPanel>(&stages, stage, toolSet, nullptr);
    HydraViewportWindow* window = panel->hydraWindow();
    CHECK(window->time() == UsdTimeCode(0.0), "the viewport draws at the stage's time, a numeric one");
    stage->setTime(12.0);
    CHECK(window->time() == UsdTimeCode(12.0), "and follows it");

    HudInput input = window->hudInput();
    CHECK(input.frame && *input.frame == 12.0, "the HUD is told the frame");
    input.size = QSizeF(800, 500);
    bool shown = false;
    for (const HudText& text : buildViewportHud(input, QApplication::font()).texts) shown = shown || text.text == "Frame 12";
    CHECK(shown, "and shows it");

    // Another stage in the panel: that one's time, and the first no longer moves it.
    const StageRefPtr second = stages.createStage();
    second->setTime(7.0);
    panel->setStage(second);
    CHECK(window->time() == UsdTimeCode(7.0), "the panel's stage's time");
    stage->setTime(20.0);
    CHECK(window->time() == UsdTimeCode(7.0), "not another stage's");
    panel->setStage(nullptr);
    CHECK(window->time().IsDefault() && !window->hudInput().frame, "no stage: no time, no frame line");

    // A viewport nobody gave a time (a preview) draws at the default one.
    HydraViewportWindow preview;
    CHECK(preview.time().IsDefault() && !preview.hudInput().frame, "a bare viewport: the default time");
}

USDCC_TEST(attributes_show_the_value_at_the_stage_time) {
    TestSettings settings;
    Scale scale;
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdGeomCube cube = UsdGeomCube::Define(stage->usdStage(), SdfPath("/Box"));
    cube.GetSizeAttr().Set(1.0, UsdTimeCode(0.0));
    cube.GetSizeAttr().Set(3.0, UsdTimeCode(10.0));
    stages.setSelectedPaths(stage, {SdfPath("/Box")});

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.resize(420, 700);
    settle();
    NumberField* size = nullptr;
    for (PropertyRow* row : panel.findChildren<PropertyRow*>()) {
        if (row->propertyPath() == "/Box.size") size = row->findChild<NumberField*>();
    }
    CHECK(size != nullptr, "the size row has a number field");
    if (!size) return;
    CHECK(isNear(size->value(), 1.0), "at the stage's time, its first sample (%g)", size->value());
    CHECK(size->isReadOnly(), "animated: still read-only until keys can be set");

    stage->setTime(5.0);
    QTest::qWait(150);
    CHECK(isNear(size->value(), 2.0), "halfway to the next sample after the time moved (%g)", size->value());
    stage->setTime(10.0);
    QTest::qWait(150);
    CHECK(isNear(size->value(), 3.0), "and on it (%g)", size->value());
}

USDCC_TEST(session_keeps_stage_time) {
    StageManager stages;
    const StageRefPtr stage = stages.createStage();
    QJsonObject state = session_stages::capture(stages, stage);
    CHECK(state.value("time").toDouble(-1.0) == 0.0 && !state.contains("playbackRange"), "the time; no range unless it has its own");
    stage->setTime(17.0);
    stage->setPlaybackRange({5.0, 40.0});
    state = session_stages::capture(stages, stage);
    CHECK(state.value("time").toDouble() == 17.0, "where the timeline stood");
    CHECK(state.value("playbackRange").toArray() == (QJsonArray{5.0, 40.0}), "and what it played");

    const StageRefPtr again = stages.createStage();
    session_stages::apply(stages, again, state, nullptr);
    CHECK(again->time() == 17.0 && again->hasPlaybackRange() && again->playbackRange() == (TimeRange{5.0, 40.0}),
          "loading puts both back");

    const StageRefPtr older = stages.createStage();
    older->setTime(3.0);
    QJsonObject before = state;
    before.remove("time");
    before.remove("playbackRange");
    session_stages::apply(stages, older, before, nullptr);
    CHECK(older->time() == 3.0 && !older->hasPlaybackRange(), "a session from before the timeline changes neither");
}

// A playback range shortened inside a longer stage range, with Loop on: playback stays
// inside it and goes round, in both modes, whether it was shortened before Play or while
// playing. With the real clock, and the range set by dragging the slider.
USDCC_TEST(timeline_loops_in_a_shortened_range) {
    TestSettings settings;
    Scale scale;
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const UsdStageRefPtr usd = UsdStage::CreateInMemory();
    usd->SetStartTimeCode(1230.0);
    usd->SetEndTimeCode(2460.0);
    usd->SetTimeCodesPerSecond(60.0);
    const StageRefPtr stage = stages.adoptStage(usd);
    PlaybackController playback(&stages);
    TimelineBar bar(&playback, undo);
    bar.resize(1400, 52);
    bar.show();
    settle();
    CHECK(playback.loops(), "Loop is on");

    std::vector<double> shown;
    auto connection = stage->timeChanged.connect([&](double time) { shown.push_back(time); });
    // Drags an end of the slider's bar to where `time` is on the track.
    const auto dragEnd = [&](bool startEnd, double time) {
        TimeRangeSlider* slider = bar.rangeSlider();
        const QRectF rect = slider->barRect();
        const double from = startEnd ? rect.left() + 1.0 : rect.right() - 1.0;
        const double perFrame = rect.width() / stage->playbackRange().length();
        const double to = from + (time - (startEnd ? stage->playbackRange().start : stage->playbackRange().end)) * perFrame;
        press(slider, from);
        for (int i = 1; i <= 10; ++i) drag(slider, from + (to - from) * i / 10.0);
        release(slider, to);
    };
    // What was shown since `shown` was last cleared: all of it inside the range, and at
    // least once back at an earlier frame.
    const auto check = [&](const char* what) {
        const TimeRange range = stage->playbackRange();
        bool inside = !shown.empty();
        int wraps = 0;
        for (size_t i = 0; i < shown.size(); ++i) {
            inside = inside && range.contains(shown[i]);
            if (i > 0 && shown[i] < shown[i - 1]) ++wraps;
        }
        CHECK(inside, "%s: every frame shown is inside %g to %g (%zu frames, from %g to %g)", what, range.start, range.end,
              shown.size(), shown.empty() ? 0.0 : shown.front(), shown.empty() ? 0.0 : shown.back());
        CHECK(wraps >= 1, "%s: it went round (%d times in %zu frames)", what, wraps, shown.size());
        CHECK(playback.isPlaying(), "%s: and is still playing", what);
    };

    for (const PlaybackMode mode : {PlaybackMode::EveryFrame, PlaybackMode::RealTime}) {
        const char* name = mode == PlaybackMode::EveryFrame ? "Every Frame" : "Real Time";
        playback.setMode(mode);

        // Shortened first, then Play.
        stage->clearPlaybackRange();
        dragEnd(false, 1300.0);
        dragEnd(true, 1280.0);
        CHECK(stage->playbackRange().length() < 40.0 && stage->playbackRange().start > 1230.0,
              "%s: the slider shortened the range (%g to %g)", name, stage->playbackRange().start, stage->playbackRange().end);
        // The bar lies under the frames that play: the ruler above it has the same scale.
        CHECK(bar.ruler()->range() == (TimeRange{1230.0, 2460.0}) && bar.rangeSlider()->fullRange() == bar.ruler()->range(),
              "%s: the ruler still shows the whole stage range, as the slider does", name);
        const QRectF barRect = bar.rangeSlider()->barRect();
        CHECK(std::abs(barRect.left() - bar.ruler()->axis().pixelAt(stage->playbackRange().start)) < 1.0 &&
                  std::abs(barRect.right() - bar.ruler()->axis().pixelAt(stage->playbackRange().end)) < 1.0,
              "%s: the bar's ends are where the ruler has the range's ends", name);
        stage->setTime(stage->playbackRange().start + 5.0);
        shown.clear();
        bar.playButton()->click();
        QTest::qWait(900);
        check(name);
        playback.stop();

        // Playing the whole range, then shortened under it.
        stage->clearPlaybackRange();
        stage->setTime(1500.0);
        bar.playButton()->click();
        QTest::qWait(150);
        dragEnd(false, 1300.0);
        dragEnd(true, 1280.0);
        QTest::qWait(50);
        shown.clear();
        QTest::qWait(900);
        check(name);
        playback.stop();
    }
    playback.setMode(PlaybackMode::EveryFrame);
}
