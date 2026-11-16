#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/playback.h"
#include "usdcc/usd/stage.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

class QAction;
class QWidget;

namespace usdcc::usd {

class StageManager;

// The app's one transport (docs/ANIMATION.md §4.2, §4.7): plays, stops and steps the
// time of the current stage, the stage of the view panel last focused. Each stage keeps
// its own time and range (Stage::time(), playbackRange()); this moves the current one's.
// Playing stops when the current stage changes.
//
// The clock is a timer on the UI thread at the stage's frame rate. Whether playback
// loops and its mode are the user's, kept in usdcc.ini.
class USDCC_USD_UI_API PlaybackController : public QObject {
    Q_OBJECT

public:
    explicit PlaybackController(StageManager* stageManager, QObject* parent = nullptr);
    ~PlaybackController() override;

    // The stage it drives: StageManager's current one. Null with none open.
    StageRefPtr stage() const { return m_stage; }

    bool isPlaying() const { return m_playing; }
    // From the time shown; from the first frame when that is the last one or outside the
    // range. Does nothing without a stage.
    void play();
    void stop();
    void togglePlaying();

    // Moves the current stage's time, as scrubbing does: stops playing first.
    void setTime(double time);
    // One frame on or back, to the first frame, to the last: these stop playing too.
    void stepForward();
    void stepBackward();
    void goToStart();
    void goToEnd();

    bool loops() const { return m_loops; }
    void setLoops(bool loops);
    PlaybackMode mode() const { return m_mode; }
    void setMode(PlaybackMode mode);

    // Advances playback as the timer does, as if `elapsedSeconds` had passed since
    // play(): for tests, which have no time to wait. Does nothing when stopped.
    void advance(double elapsedSeconds);

    // Play/Stop, Next Frame and Previous Frame as actions on `window`, with keys from the
    // Shortcuts settings (Ctrl+Space, "." and ","), which this defines.
    void addActions(QWidget* window);

Q_SIGNALS:
    // The current stage changed; `stage()` is the new one.
    void stageChanged();
    void playingChanged(bool playing);
    // loops() or mode() changed.
    void optionsChanged();

private:
    void onCurrentStageChanged(const StageRefPtr& stage);
    void tick();

    StageManager* m_stageManager;
    StageRefPtr m_stage;
    usdcc::core::Signal<StageRefPtr>::Connection m_currentStageConnection;
    QTimer m_timer;
    QElapsedTimer m_clock;      // since play(), for Real Time
    double m_startTime = 0.0;   // the stage's time at play()
    bool m_playing = false;
    bool m_loops = true;
    PlaybackMode m_mode = PlaybackMode::EveryFrame;
};

}  // namespace usdcc::usd
