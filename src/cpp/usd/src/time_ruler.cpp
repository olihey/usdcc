#include "usdcc/usd/time_ruler.h"
#include "usdcc/ui/theme.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/property_style.h"

#include <QEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

namespace style = properties::style;
using usdcc::ui::scaled::pixels;

namespace {
constexpr int kHeight = 24;
constexpr int kSideMargin = 12;  // the range's ends sit this far in, so their labels fit
constexpr int kMajorTick = 7;
constexpr int kMinorTick = 3;
constexpr int kLabelGap = 14;  // between two frame numbers, at least
constexpr int kTickGap = 5;    // between two ticks, at least
}  // namespace

QString formatFrame(double time) {
    // Two decimals at most, and none that say nothing.
    QString number = QString::number(time, 'f', 2);
    while (number.endsWith(QLatin1Char('0'))) number.chop(1);
    if (number.endsWith(QLatin1Char('.'))) number.chop(1);
    return number == QLatin1String("-0") ? QStringLiteral("0") : number;
}

TimeRuler::TimeRuler(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
    // Focused by a click, for Delete, Ctrl+C and Ctrl+V on the key picked.
    setFocusPolicy(Qt::ClickFocus);
}

QSize TimeRuler::sizeHint() const { return usdcc::ui::scaled::size(320, kHeight); }

QSize TimeRuler::minimumSizeHint() const { return usdcc::ui::scaled::size(80, kHeight); }

void TimeRuler::setRange(const TimeRange& range) {
    if (ordered(range) == m_axis.range()) return;
    m_axis.setRange(range);
    update();
}

void TimeRuler::setPlaybackRange(const TimeRange& range) {
    const TimeRange wanted = ordered(range);
    if (wanted == m_playbackRange) return;
    m_playbackRange = wanted;
    update();
}

void TimeRuler::setCachedRange(const std::optional<TimeRange>& range, bool stale) {
    if (range == m_cachedRange && stale == m_cacheStale) return;
    m_cachedRange = range;
    m_cacheStale = stale;
    update();
}

void TimeRuler::setTime(double time) {
    if (time == m_time) return;
    m_time = time;
    update();
}

void TimeRuler::setKeys(std::vector<double> keys) {
    if (keys == m_keys) return;
    m_keys = std::move(keys);
    if (m_selectedKey && !m_draggingKey &&
        std::none_of(m_keys.begin(), m_keys.end(), [this](double key) { return std::abs(key - *m_selectedKey) < 1e-6; })) {
        m_selectedKey.reset();
    }
    update();
}

void TimeRuler::setSelectedKey(std::optional<double> time) {
    if (time == m_selectedKey) return;
    m_selectedKey = time;
    update();
}

std::optional<double> TimeRuler::keyAt(const QPointF& position) const {
    // The diamonds sit along the bottom: the lower half, a few pixels either side.
    if (position.y() < height() / 2.0) return std::nullopt;
    const TimeAxis& shown = axis();
    std::optional<double> nearest;
    double best = pixels(5) + 1.0;
    for (const double key : m_keys) {
        const double distance = std::abs(shown.pixelAt(key) - position.x());
        if (distance <= best) {
            best = distance;
            nearest = key;
        }
    }
    return nearest;
}

const TimeAxis& TimeRuler::axis() const {
    m_axis.setExtent(pixels(kSideMargin), width() - pixels(kSideMargin));
    return m_axis;
}

void TimeRuler::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::EnabledChange) update();
}

void TimeRuler::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    shape.addRoundedRect(r, pixels(style::kRadius), pixels(style::kRadius));
    p.fillPath(shape, isEnabled() ? style::inset() : style::readOnlyFill());
    p.setPen(QPen(isEnabled() ? style::border() : style::readOnlyBorder(), 1.0));
    p.drawPath(shape);
    if (!isEnabled()) return;  // no stage: no times to show

    const QFontMetricsF metrics(font());
    const TimeAxis& shown = axis();
    const TimeRange range = shown.range();
    // Room for the widest frame number between two labelled ticks.
    const double widest = std::max(metrics.horizontalAdvance(formatFrame(std::floor(range.start))),
                                   metrics.horizontalAdvance(formatFrame(std::ceil(range.end))));
    const TimeAxis::TickSteps steps = shown.tickSteps(widest + pixels(kLabelGap), pixels(kTickGap));

    const double bottom = r.bottom();
    p.setClipPath(shape);
    p.setPen(QPen(style::border(), 1.0));
    if (steps.minor < steps.major) {
        for (const double time : shown.ticks(steps.minor)) {
            const double x = std::round(shown.pixelAt(time)) + 0.5;
            p.drawLine(QPointF(x, bottom - pixels(kMinorTick)), QPointF(x, bottom));
        }
    }
    const double labelHeight = metrics.height();
    for (const double time : shown.ticks(steps.major)) {
        const double x = std::round(shown.pixelAt(time)) + 0.5;
        p.setPen(QPen(style::borderHover(), 1.0));
        p.drawLine(QPointF(x, bottom - pixels(kMajorTick)), QPointF(x, bottom));
        p.setPen(style::textMuted());
        const QString label = formatFrame(time);
        const double w = metrics.horizontalAdvance(label);
        p.drawText(QRectF(x - w / 2.0 - 1.0, r.top() + 1.0, w + 2.0, labelHeight), Qt::AlignCenter, label);
    }

    // What doesn't play is dimmed: the ruler's own colour over it, most of the way.
    {
        QColor veil = style::inset();
        veil.setAlpha(170);
        const double playFrom = std::clamp(shown.pixelAt(m_playbackRange.start), r.left(), r.right());
        const double playTo = std::clamp(shown.pixelAt(m_playbackRange.end), r.left(), r.right());
        if (range.start < m_playbackRange.start) p.fillRect(QRectF(r.left(), r.top(), playFrom - r.left(), r.height()), veil);
        if (range.end > m_playbackRange.end) p.fillRect(QRectF(playTo, r.top(), r.right() - playTo, r.height()), veil);
    }

    // The simulation's cache: a band along the bottom, from its first frame to its last.
    if (m_cachedRange) {
        const double from = std::clamp(shown.pixelAt(m_cachedRange->start), r.left(), r.right());
        const double to = std::clamp(shown.pixelAt(m_cachedRange->end), r.left(), r.right());
        QColor band = m_cacheStale ? style::textMuted() : style::accent();
        band.setAlpha(m_cacheStale ? 140 : 200);
        const double height = pixels(3);
        p.fillRect(QRectF(from, bottom - height, std::max(to - from, 2.0), height), band);
    }

    // The selection's keys (docs/ANIMATION.md phase 2): a diamond each, along the bottom.
    if (!m_keys.empty()) {
        const double size = pixels(4);
        p.setPen(Qt::NoPen);
        QColor key = style::accent();
        key.setAlpha(230);
        p.setBrush(key);
        const auto picked = [this](double time) { return m_selectedKey && std::abs(time - *m_selectedKey) < 1e-6; };
        for (const double key : m_keys) {
            // The one being dragged is drawn where it would go.
            const double time = m_draggingKey && picked(key) ? m_dragTo : key;
            if (!ordered(range).contains(time)) continue;
            const double x = std::round(shown.pixelAt(time)) + 0.5;
            const double s = picked(key) ? size + 1.0 : size;
            const double y = bottom - size - 1.0;
            const QPointF diamond[] = {{x, y - s}, {x + s, y}, {x, y + s}, {x - s, y}};
            p.setPen(picked(key) ? QPen(Qt::white, 1.0) : Qt::NoPen);
            p.drawPolygon(diamond, 4);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::NoBrush);
    }

    // The playhead: a line the ruler's height, with its frame on a plate at the top.
    if (ordered(range).contains(m_time)) {
        const double x = std::round(shown.pixelAt(m_time)) + 0.5;
        const QColor accent = style::accent();
        p.setPen(QPen(accent, std::max(1.0, pixels(2) * 1.0)));
        p.drawLine(QPointF(x, r.top()), QPointF(x, bottom));
        const QString label = formatFrame(m_time);
        const double w = metrics.horizontalAdvance(label) + pixels(8);
        QRectF plate(x - w / 2.0, r.top() + 1.0, w, labelHeight);
        plate.moveLeft(std::clamp(plate.left(), r.left() + 1.0, std::max(r.left() + 1.0, r.right() - 1.0 - w)));
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRoundedRect(plate, pixels(2), pixels(2));
        p.setPen(usdcc::ui::theme::color(usdcc::ui::theme::Token::OnAccent));
        p.drawText(plate, Qt::AlignCenter, label);
    }
}

void TimeRuler::scrubTo(const QPointF& position, bool anyTime) {
    const TimeAxis& shown = axis();
    const TimeRange range = shown.range();
    double time = anyTime ? shown.timeAt(position.x()) : shown.frameAt(position.x());
    // A whole frame inside the range, unless the range holds none.
    if (!anyTime && !range.contains(time)) {
        time = time < range.start ? std::ceil(range.start) : std::floor(range.end);
    }
    time = clampedTo(time, range);
    if (m_scrubbing && time == m_lastScrubbed) return;
    m_lastScrubbed = time;
    Q_EMIT timeScrubbed(time);
}

void TimeRuler::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    // On a key mark: it's picked, and a drag moves it (phase 3). Elsewhere: a scrub.
    if (const std::optional<double> key = keyAt(event->position())) {
        m_selectedKey = key;
        m_draggingKey = true;
        m_dragTo = *key;
        update();
        event->accept();
        return;
    }
    setSelectedKey(std::nullopt);
    Q_EMIT scrubStarted();
    scrubTo(event->position(), event->modifiers().testFlag(Qt::ShiftModifier));
    m_scrubbing = true;
    event->accept();
}

void TimeRuler::mouseMoveEvent(QMouseEvent* event) {
    if (m_draggingKey) {
        const double to = clampedTo(axis().frameAt(event->position().x()), axis().range());
        if (to != m_dragTo) {
            m_dragTo = to;
            update();
        }
        event->accept();
        return;
    }
    if (!m_scrubbing) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    scrubTo(event->position(), event->modifiers().testFlag(Qt::ShiftModifier));
    event->accept();
}

void TimeRuler::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_draggingKey) {
        m_draggingKey = false;
        const double from = m_selectedKey.value_or(m_dragTo);
        if (std::abs(m_dragTo - from) > 1e-6) {
            m_selectedKey = m_dragTo;
            Q_EMIT keyMoved(from, m_dragTo);
        }
        update();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || !m_scrubbing) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_scrubbing = false;
    Q_EMIT scrubFinished();
    event->accept();
}

void TimeRuler::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Paste)) {
        Q_EMIT keyPasteRequested();
        event->accept();
        return;
    }
    if (m_selectedKey) {
        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
            Q_EMIT keyDeleteRequested(*m_selectedKey);
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Copy)) {
            Q_EMIT keyCopyRequested(*m_selectedKey);
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

}  // namespace usdcc::usd
