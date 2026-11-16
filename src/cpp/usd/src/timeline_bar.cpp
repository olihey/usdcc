#include "usdcc/usd/timeline_bar.h"

#include "usdcc/ui/panel_icon_button.h"
#include "usdcc/ui/panel_menu_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/playback_controller.h"
#include "usdcc/usd/properties/number_field.h"
#include "usdcc/usd/time_range_slider.h"
#include "usdcc/usd/time_ruler.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QAction>
#include <QActionGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace usdcc::usd {

using properties::NumberField;
using usdcc::ui::PanelIconButton;

namespace {
constexpr int kButtonHeight = 22;
constexpr int kFieldWidth = 52;

PanelIconButton* transportButton(const char* icon, const QString& toolTip, QWidget* parent) {
    auto* button = new PanelIconButton(QStringLiteral(":/usdcc/theme/%1.png").arg(QLatin1String(icon)), toolTip, parent);
    usdcc::ui::scaled::fixedHeight(button, kButtonHeight);
    button->setFocusPolicy(Qt::NoFocus);  // the keyboard stays where the user was working
    return button;
}

NumberField* makeFrameField(const QString& toolTip, QWidget* parent) {
    auto* field = new NumberField(parent);
    field->setDecimals(0);
    field->setStep(0.25);  // four pixels of drag to a frame
    field->setToolTip(toolTip);
    usdcc::ui::scaled::fixedWidth(field, kFieldWidth);
    return field;
}
}  // namespace

TimelineBar::TimelineBar(PlaybackController* playback, std::shared_ptr<UndoManager> undoManager, QWidget* parent)
    : QWidget(parent), m_playback(playback), m_undoManager(std::move(undoManager)) {
    setObjectName(QStringLiteral("timelineBar"));  // styled by theme/dark.qss
    setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QHBoxLayout(this);
    usdcc::ui::scaled::margins(layout, 6, 3, 6, 3);
    usdcc::ui::scaled::spacing(layout, 3);

    m_frame = makeFrameField(tr("The frame shown. Drag to scrub, click to type one."), this);
    layout->addWidget(m_frame);
    layout->addSpacing(usdcc::ui::scaled::pixels(4));

    auto* toStart = transportButton("skip-start", tr("Go to the first frame"), this);
    auto* back = transportButton("step-back", tr("Previous frame"), this);
    m_play = transportButton("play", tr("Play"), this);
    m_play->setObjectName(QStringLiteral("timelinePlayButton"));
    auto* forward = transportButton("step-forward", tr("Next frame"), this);
    auto* toEnd = transportButton("skip-end", tr("Go to the last frame"), this);
    for (QToolButton* button : std::initializer_list<QToolButton*>{toStart, back, m_play, forward, toEnd}) {
        layout->addWidget(button);
    }
    layout->addSpacing(usdcc::ui::scaled::pixels(4));

    m_start = makeFrameField(tr("The first frame of the playback range"), this);
    m_end = makeFrameField(tr("The last frame of the playback range"), this);
    auto* middle = new QVBoxLayout;
    middle->setContentsMargins(0, 0, 0, 0);
    usdcc::ui::scaled::spacing(middle, 2);
    m_ruler = new TimeRuler(this);
    m_slider = new TimeRangeSlider(this);
    m_slider->setToolTip(tr("The playback range: the frames above it play. Drag an end to change it, the middle to move it."));
    middle->addWidget(m_ruler);
    middle->addWidget(m_slider);
    layout->addWidget(m_start);
    layout->addLayout(middle, 1);
    layout->addWidget(m_end);
    layout->addSpacing(usdcc::ui::scaled::pixels(4));

    m_rate = new QLabel(this);
    m_rate->setObjectName(QStringLiteral("propertyNote"));  // muted, as the attribute editor's notes
    m_rate->setToolTip(tr("The stage's frame rate: its time codes per second (Layer Properties)."));
    layout->addWidget(m_rate);

    m_loop = transportButton("loop", tr("Loop: start again after the last frame"), this);
    m_loop->setCheckable(true);
    layout->addWidget(m_loop);
    auto* menuButton = new usdcc::ui::PanelMenuButton(tr("Timeline options"), [this](QMenu* menu) { populateMenu(menu); }, this);
    menuButton->setIcon(QIcon(QStringLiteral(":/usdcc/theme/more.png")));
    usdcc::ui::scaled::fixedHeight(menuButton, kButtonHeight);
    menuButton->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(menuButton);

    connect(toStart, &QToolButton::clicked, m_playback, &PlaybackController::goToStart);
    connect(back, &QToolButton::clicked, m_playback, &PlaybackController::stepBackward);
    connect(m_play, &QToolButton::clicked, m_playback, &PlaybackController::togglePlaying);
    connect(forward, &QToolButton::clicked, m_playback, &PlaybackController::stepForward);
    connect(toEnd, &QToolButton::clicked, m_playback, &PlaybackController::goToEnd);
    connect(m_loop, &QToolButton::clicked, m_playback, &PlaybackController::setLoops);

    // Scrubbing, in the ruler and in the frame field: whole frames, and playing stops.
    connect(m_ruler, &TimeRuler::timeScrubbed, m_playback, &PlaybackController::setTime);
    connect(m_frame, &NumberField::editStarted, this, [this]() { m_timeBeforeEdit = m_stage ? m_stage->time() : 0.0; });
    connect(m_frame, &NumberField::valueEdited, this, [this](double value) { m_playback->setTime(snappedToFrame(value)); });
    connect(m_frame, &NumberField::editFinished, this, [this]() { refreshTime(); });
    connect(m_frame, &NumberField::editCanceled, this, [this]() { m_playback->setTime(m_timeBeforeEdit); });

    // The range: either field, or the slider. A start typed past the end takes the end
    // with it, and the other way round, so the range is never less than a frame.
    const auto rememberRange = [this]() {
        m_hadRangeBeforeEdit = m_stage && m_stage->hasPlaybackRange();
        m_rangeBeforeEdit = m_stage ? m_stage->playbackRange() : TimeRange{};
    };
    const auto restoreRange = [this]() {
        if (!m_stage) return;
        if (m_hadRangeBeforeEdit) {
            m_stage->setPlaybackRange(m_rangeBeforeEdit);
        } else {
            m_stage->clearPlaybackRange();
        }
    };
    connect(m_start, &NumberField::editStarted, this, rememberRange);
    connect(m_end, &NumberField::editStarted, this, rememberRange);
    connect(m_start, &NumberField::editCanceled, this, restoreRange);
    connect(m_end, &NumberField::editCanceled, this, restoreRange);
    connect(m_start, &NumberField::editFinished, this, [this]() { refreshRange(false); });
    connect(m_end, &NumberField::editFinished, this, [this]() { refreshRange(false); });
    connect(m_start, &NumberField::valueEdited, this, [this](double value) {
        if (!m_stage) return;
        const double start = snappedToFrame(value);
        m_stage->setPlaybackRange({start, std::max(m_stage->playbackRange().end, start + 1.0)});
    });
    connect(m_end, &NumberField::valueEdited, this, [this](double value) {
        if (!m_stage) return;
        const double end = snappedToFrame(value);
        m_stage->setPlaybackRange({std::min(m_stage->playbackRange().start, end - 1.0), end});
    });
    connect(m_slider, &TimeRangeSlider::rangeEdited, this, [this](const TimeRange& range) {
        if (!m_stage) return;
        // The track stays as it is under the drag: only the bar moves.
        m_sliderEditing = true;
        m_stage->setPlaybackRange(range);
        m_sliderEditing = false;
    });

    connect(m_playback, &PlaybackController::stageChanged, this, &TimelineBar::bindStage);
    connect(m_playback, &PlaybackController::playingChanged, this, &TimelineBar::refreshPlaying);
    connect(m_playback, &PlaybackController::optionsChanged, this, &TimelineBar::refreshOptions);
    refreshPlaying();
    refreshOptions();
    bindStage();
}

TimelineBar::~TimelineBar() = default;

void TimelineBar::bindStage() {
    m_stage = m_playback->stage();
    m_timeConnection = {};
    m_rangeConnection = {};
    m_changedConnection = {};
    if (m_stage) {
        m_timeConnection = m_stage->timeChanged.connect([this](double) { refreshTime(); });
        m_rangeConnection = m_stage->playbackRangeChanged.connect([this]() { refreshRange(!m_sliderEditing); });
        // The stage's own range and frame rate are metadata, which any edit may change:
        // fitted anew only when the range did.
        m_changedConnection = m_stage->changed.connect([this](const StageChanges&) {
            const PXR_NS::UsdStageRefPtr& usd = m_stage->usdStage();
            const std::optional<TimeRange> now = hasAuthoredTimeRange(usd) ? std::optional(stageTimeRange(usd)) : std::nullopt;
            refreshRange(now != m_stageRange);
        });
    }
    for (QWidget* child : findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) child->setEnabled(m_stage != nullptr);
    refreshRange(true);
    refreshTime();
}

void TimelineBar::refreshTime() {
    const double time = m_stage ? m_stage->time() : 0.0;
    m_ruler->setTime(time);
    // A whole frame as one; a time between two with what's after the point.
    m_frame->setDecimals(time == std::round(time) ? 0 : 2);
    m_frame->setValue(time);
}

TimeRange TimelineBar::fittedRange() const {
    if (!m_stage) return kDefaultTimeRange;
    const TimeRange playback = m_stage->playbackRange();
    // A stage with no range of its own has nothing wider to show than what plays.
    return m_stageRange ? united(*m_stageRange, playback) : playback;
}

void TimelineBar::refreshRange(bool fit) {
    const TimeRange range = m_stage ? m_stage->playbackRange() : kDefaultTimeRange;
    m_stageRange.reset();
    if (m_stage && hasAuthoredTimeRange(m_stage->usdStage())) m_stageRange = stageTimeRange(m_stage->usdStage());
    m_shown = fit ? fittedRange() : united(m_shown, range);
    // One scale for both, so the bar lies under the frames it stands for.
    m_ruler->setRange(m_shown);
    m_ruler->setPlaybackRange(range);
    m_slider->setFullRange(m_shown);
    m_slider->setRange(range);
    m_start->setValue(range.start);
    m_end->setValue(range.end);
    if (m_stage) {
        const double rate = timeCodesPerSecond(m_stage->usdStage());
        m_rate->setText(tr("%1 fps").arg(QString::number(rate, 'g', 5)));
    } else {
        m_rate->setText(QString());
    }
}

void TimelineBar::refreshPlaying() {
    const bool playing = m_playback->isPlaying();
    m_play->setIcon(QIcon(playing ? QStringLiteral(":/usdcc/theme/pause.png") : QStringLiteral(":/usdcc/theme/play.png")));
    m_play->setToolTip(playing ? tr("Stop") : tr("Play"));
}

void TimelineBar::refreshOptions() { m_loop->setChecked(m_playback->loops()); }

void TimelineBar::populateMenu(QMenu* menu) {
    QAction* loop = menu->addAction(tr("Loop"));
    loop->setCheckable(true);
    loop->setChecked(m_playback->loops());
    connect(loop, &QAction::triggered, m_playback, &PlaybackController::setLoops);

    menu->addSeparator();
    auto* modes = new QActionGroup(menu);
    const struct {
        PlaybackMode mode;
        QString text;
        QString tip;
    } entries[] = {
        {PlaybackMode::EveryFrame, tr("Play Every Frame"), tr("Show each frame, however long it takes to draw")},
        {PlaybackMode::RealTime, tr("Play in Real Time"), tr("Keep to the frame rate, skipping frames when drawing is slower")},
    };
    for (const auto& entry : entries) {
        QAction* action = menu->addAction(entry.text);
        action->setCheckable(true);
        action->setChecked(m_playback->mode() == entry.mode);
        action->setStatusTip(entry.tip);
        modes->addAction(action);
        connect(action, &QAction::triggered, m_playback, [this, mode = entry.mode]() { m_playback->setMode(mode); });
    }

    menu->addSeparator();
    QAction* reset = menu->addAction(tr("Reset Playback Range"));
    reset->setObjectName(QStringLiteral("timelineResetRange"));
    reset->setStatusTip(tr("Play the stage's own range again"));
    reset->setEnabled(m_stage && m_stage->hasPlaybackRange());
    connect(reset, &QAction::triggered, this, [this]() {
        if (m_stage) m_stage->clearPlaybackRange();
    });
    QAction* author = menu->addAction(tr("Set Stage Range to Playback Range"));
    author->setObjectName(QStringLiteral("timelineSetStageRange"));
    author->setStatusTip(tr("Write the playback range to the root layer as the stage's start and end time codes"));
    author->setEnabled(m_stage && m_undoManager &&
                       (!hasAuthoredTimeRange(m_stage->usdStage()) || m_stage->playbackRange() != stageTimeRange(m_stage->usdStage())));
    connect(author, &QAction::triggered, this, &TimelineBar::setStageRangeToPlaybackRange);
}

void TimelineBar::setStageRangeToPlaybackRange() {
    if (!m_stage || !m_undoManager) return;
    const TimeRange range = m_stage->playbackRange();
    const std::string root = m_stage->usdStage()->GetRootLayer()->GetIdentifier();
    layer_actions::editLayer(*m_undoManager, m_stage, root, tr("Set Stage Range"), [range](const PXR_NS::SdfLayerHandle& layer) {
        layer->SetStartTimeCode(range.start);
        layer->SetEndTimeCode(range.end);
        return true;
    });
    // The stage says it now, so there's nothing left to override.
    m_stage->clearPlaybackRange();
}

}  // namespace usdcc::usd
