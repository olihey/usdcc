#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>

#include <QWidget>

#include <memory>
#include <optional>
#include <vector>

class QLabel;
class QMenu;
class QTimer;
class QToolButton;

namespace usdcc::usd {

class PlaybackController;
class TimeRangeSlider;
class TimeRuler;
class UndoManager;

namespace properties {
class NumberField;
}

// The timeline (docs/ANIMATION.md §4.3): one thin bar for the app, under the dock area,
// on the current stage. The frame shown and the transport buttons, then the ruler with
// the range slider under it, between the playback range's start and end, then the frame
// rate, Loop, Simulate and a menu. The ruler shows the frames a simulation has cached
// (docs/PHYSICS.md §5.1).
//
// The ruler and the slider show the same times, edge to edge (shownRange()), so the
// slider's bar lies under exactly the frames that play, which the ruler lights. That
// range is the stage's own with the playback range, or the playback range alone for a
// stage that has none. Dragging the slider leaves it as it is, so a range can be made
// shorter and longer again; anything else that sets the playback range fits it anew.
//
// It shows what the stage says (its time, playback range and frame rate) and hands every
// change to the PlaybackController or the Stage; nothing is kept here. Without a stage it
// is greyed out.
class USDCC_USD_UI_API TimelineBar : public QWidget {
    Q_OBJECT

public:
    TimelineBar(PlaybackController* playback, std::shared_ptr<UndoManager> undoManager, QWidget* parent = nullptr);
    ~TimelineBar() override;

    TimeRuler* ruler() const { return m_ruler; }
    TimeRangeSlider* rangeSlider() const { return m_slider; }
    properties::NumberField* frameField() const { return m_frame; }
    properties::NumberField* startField() const { return m_start; }
    properties::NumberField* endField() const { return m_end; }
    QToolButton* playButton() const { return m_play; }
    QToolButton* loopButton() const { return m_loop; }
    QToolButton* simulateButton() const { return m_simulate; }
    QLabel* rateLabel() const { return m_rate; }
    // The times the ruler and the slider both show.
    TimeRange shownRange() const { return m_shown; }

    // The menu behind the last button: Loop, Every Frame or Real Time, Simulate and Reset
    // Simulation, and the range.
    void populateMenu(QMenu* menu);

    // Writes the playback range to the stage's root layer as its start and end time
    // codes, one undo step, and goes back to following the stage's range.
    void setStageRangeToPlaybackRange();

    // Keys (docs/ANIMATION.md phase 2). The ruler marks the selection's keys, those of
    // every animated attribute of the selected prims.
    QToolButton* autoKeyButton() const { return m_autoKey; }
    std::vector<double> selectionKeyTimes() const;
    // A key on every transform op of the selected prims, at the frame shown: one undo step.
    void setKeyOnSelection();
    void goToNextKey();
    void goToPreviousKey();
    // Phase 3, what the ruler's key marks do: each moves, deletes or copies every key of
    // the selection at that time; paste puts the keys copied last at the frame shown, on
    // the selected prims. One undo step each.
    void moveSelectionKeys(double from, double to);
    void deleteSelectionKeys(double time);
    void copySelectionKeys(double time);
    void pasteKeys();
    // Set Key, Next Key and Previous Key as the window's actions, with their shortcuts
    // (Settings > Shortcuts > Timeline): S, Alt+. and Alt+, by default.
    void addActions(QWidget* window);

private:
    void refreshKeys();
    std::vector<PXR_NS::SdfPath> selectedPaths() const;
    std::vector<PXR_NS::UsdAttribute> selectionAnimatedAttributes() const;
    void refreshAutoKey();
    void bindStage();
    void refreshTime();
    // `fit`: the shown range is worked out anew; otherwise it only grows, to hold the
    // playback range.
    void refreshRange(bool fit);
    TimeRange fittedRange() const;
    void refreshPlaying();
    void refreshOptions();
    void refreshSimulation();

    PlaybackController* m_playback;
    std::shared_ptr<UndoManager> m_undoManager;
    StageRefPtr m_stage;
    usdcc::core::Signal<double>::Connection m_timeConnection;
    usdcc::core::Signal<>::Connection m_rangeConnection;
    usdcc::core::Signal<const StageChanges&>::Connection m_changedConnection;
    usdcc::core::Signal<>::Connection m_physicsConnection;
    usdcc::core::Signal<StageRefPtr, std::vector<PXR_NS::SdfPath>>::Connection m_selectionConnection;
    usdcc::core::Signal<bool>::Connection m_autoKeyConnection;
    QTimer* m_keysTimer;  // the key marks again, a moment after edits stop

    properties::NumberField* m_frame;
    properties::NumberField* m_start;
    properties::NumberField* m_end;
    TimeRuler* m_ruler;
    TimeRangeSlider* m_slider;
    QToolButton* m_play;
    QToolButton* m_loop;
    QToolButton* m_simulate;
    QToolButton* m_autoKey;
    QLabel* m_rate;
    TimeRange m_shown = kDefaultTimeRange;
    // The stage's own range as last seen (nothing: it has none), to tell when it changes.
    std::optional<TimeRange> m_stageRange;
    bool m_sliderEditing = false;  // inside the slider's rangeEdited()
    double m_timeBeforeEdit = 0.0;      // the frame field's, for Esc
    TimeRange m_rangeBeforeEdit;        // the range fields'
    bool m_hadRangeBeforeEdit = false;
};

}  // namespace usdcc::usd
