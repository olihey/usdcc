#include "usdcc/usd/playback_controller.h"

#include "usdcc/core/settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/stage_manager.h"

#include <QAction>
#include <QSettings>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

namespace {
constexpr auto kLoopKey = "Timeline/loop";
constexpr auto kEveryFrameKey = "Timeline/everyFrame";
constexpr auto kPlayShortcutKey = "Shortcuts/timeline.play";
constexpr auto kNextFrameShortcutKey = "Shortcuts/timeline.nextFrame";
constexpr auto kPreviousFrameShortcutKey = "Shortcuts/timeline.previousFrame";
}  // namespace

PlaybackController::PlaybackController(StageManager* stageManager, QObject* parent)
    : QObject(parent), m_stageManager(stageManager), m_stage(stageManager->currentStage()) {
    {
        QSettings settings = usdcc::ui::appSettings();
        m_loops = settings.value(kLoopKey, true).toBool();
        m_mode = settings.value(kEveryFrameKey, true).toBool() ? PlaybackMode::EveryFrame : PlaybackMode::RealTime;
    }
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &PlaybackController::tick);
    m_currentStageConnection =
        stageManager->currentStageChanged.connect([this](const StageRefPtr& stage) { onCurrentStageChanged(stage); });
}

PlaybackController::~PlaybackController() = default;

void PlaybackController::onCurrentStageChanged(const StageRefPtr& stage) {
    if (stage == m_stage) return;
    stop();  // on the stage it was playing
    m_stage = stage;
    Q_EMIT stageChanged();
}

void PlaybackController::play() {
    if (m_playing || !m_stage) return;
    const TimeRange frames = playbackFrames(m_stage->playbackRange());
    // On the last frame, or anywhere outside the range: from the top.
    if (m_stage->time() >= frames.end || m_stage->time() < frames.start) m_stage->setTime(frames.start);
    m_startTime = m_stage->time();
    m_clock.start();
    m_playing = true;
    const double rate = timeCodesPerSecond(m_stage->usdStage());
    m_timer.start(std::max(1, static_cast<int>(std::lround(1000.0 / rate))));
    Q_EMIT playingChanged(true);
}

void PlaybackController::stop() {
    if (!m_playing) return;
    m_playing = false;
    m_timer.stop();
    Q_EMIT playingChanged(false);
}

void PlaybackController::togglePlaying() {
    if (m_playing) {
        stop();
    } else {
        play();
    }
}

void PlaybackController::tick() { advance(static_cast<double>(m_clock.nsecsElapsed()) / 1e9); }

void PlaybackController::advance(double elapsedSeconds) {
    if (!m_playing || !m_stage) return;
    const TimeRange range = m_stage->playbackRange();
    const PlaybackStep step =
        m_mode == PlaybackMode::EveryFrame
            ? nextFrame(m_stage->time(), range, m_loops)
            : frameAfter(m_startTime, elapsedSeconds, timeCodesPerSecond(m_stage->usdStage()), range, m_loops);
    m_stage->setTime(step.time);
    if (step.finished) stop();
}

void PlaybackController::setTime(double time) {
    stop();
    if (m_stage) m_stage->setTime(time);
}

void PlaybackController::stepForward() {
    if (m_stage) setTime(steppedFrame(m_stage->time(), 1, m_stage->playbackRange()));
}

void PlaybackController::stepBackward() {
    if (m_stage) setTime(steppedFrame(m_stage->time(), -1, m_stage->playbackRange()));
}

void PlaybackController::goToStart() {
    if (m_stage) setTime(playbackFrames(m_stage->playbackRange()).start);
}

void PlaybackController::goToEnd() {
    if (m_stage) setTime(playbackFrames(m_stage->playbackRange()).end);
}

void PlaybackController::setLoops(bool loops) {
    if (loops == m_loops) return;
    m_loops = loops;
    usdcc::ui::appSettings().setValue(kLoopKey, loops);
    Q_EMIT optionsChanged();
}

void PlaybackController::setMode(PlaybackMode mode) {
    if (mode == m_mode) return;
    m_mode = mode;
    usdcc::ui::appSettings().setValue(kEveryFrameKey, mode == PlaybackMode::EveryFrame);
    // Real Time counts from where it is now, not from where Every Frame began.
    if (m_playing && m_stage) {
        m_startTime = m_stage->time();
        m_clock.start();
    }
    Q_EMIT optionsChanged();
}

void PlaybackController::addActions(QWidget* window) {
    struct Entry {
        const char* key;
        const char* label;
        const char* description;
        const char* defaultKey;
        const char* objectName;
        QString text;
        void (PlaybackController::*run)();
    };
    const Entry entries[] = {
        {kPlayShortcutKey, "Play or stop", "Start playing the current stage's animation, or stop it.", "Ctrl+Space",
         "timelinePlayAction", tr("Play / Stop"), &PlaybackController::togglePlaying},
        {kNextFrameShortcutKey, "Next frame", "Move the current stage one frame on.", ".", "timelineNextFrameAction",
         tr("Next Frame"), &PlaybackController::stepForward},
        {kPreviousFrameShortcutKey, "Previous frame", "Move the current stage one frame back.", ",",
         "timelinePreviousFrameAction", tr("Previous Frame"), &PlaybackController::stepBackward},
    };
    for (const Entry& entry : entries) {
        usdcc::core::Settings::instance().define({
            .key = entry.key,
            .category = "Shortcuts",
            .group = "Timeline",
            .label = entry.label,
            .description = entry.description,
            .type = usdcc::core::SettingType::Shortcut,
            .defaultValue = std::string(entry.defaultKey),
        });
        auto* action = new QAction(entry.text, window);
        action->setObjectName(QLatin1String(entry.objectName));
        // Application-wide, as the tool keys are: a floating viewport has them too, and a
        // text field still gets the character typed.
        action->setShortcutContext(Qt::ApplicationShortcut);
        usdcc::ui::bindShortcut(action, entry.key);
        window->addAction(action);
        connect(action, &QAction::triggered, this, entry.run);
    }
}

}  // namespace usdcc::usd
