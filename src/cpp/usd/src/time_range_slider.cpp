#include "usdcc/usd/time_range_slider.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/property_style.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

namespace style = properties::style;
using usdcc::ui::scaled::pixels;

namespace {
constexpr int kHeight = 12;
constexpr int kSideMargin = 12;  // as the ruler's, so a frame sits at the same x in both
constexpr int kGrip = 6;         // how near an end of the bar grabs that end
constexpr int kEndWidth = 3;
}  // namespace

TimeRangeSlider::TimeRangeSlider(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
}

QSize TimeRangeSlider::sizeHint() const { return usdcc::ui::scaled::size(320, kHeight); }

QSize TimeRangeSlider::minimumSizeHint() const { return usdcc::ui::scaled::size(80, kHeight); }

void TimeRangeSlider::setFullRange(const TimeRange& range) {
    if (ordered(range) == m_axis.range()) return;
    m_axis.setRange(range);
    update();
}

void TimeRangeSlider::setRange(const TimeRange& range) {
    const TimeRange wanted = ordered(range);
    if (wanted == m_range) return;
    m_range = wanted;
    update();
}

const TimeAxis& TimeRangeSlider::axis() const {
    m_axis.setExtent(pixels(kSideMargin), width() - pixels(kSideMargin));
    return m_axis;
}

void TimeRangeSlider::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::EnabledChange) update();
}

QRectF TimeRangeSlider::barRect() const {
    const TimeAxis& shown = axis();
    const TimeRange full = shown.range();
    const double left = shown.pixelAt(std::clamp(m_range.start, full.start, full.end));
    const double right = shown.pixelAt(std::clamp(m_range.end, full.start, full.end));
    // Wide enough to grab, whatever the range.
    const double width = std::max(right - left, static_cast<double>(pixels(2 * kEndWidth + 2)));
    return QRectF(left, 1.5, width, height() - 3.0);
}

void TimeRangeSlider::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double radius = pixels(style::kRadius);
    const TimeAxis& shown = axis();
    const QRectF track(shown.left() - 1.0, 2.5, shown.right() - shown.left() + 2.0, height() - 5.0);
    p.setPen(QPen(isEnabled() ? style::border() : style::readOnlyBorder(), 1.0));
    p.setBrush(isEnabled() ? style::inset() : style::readOnlyFill());
    p.drawRoundedRect(track, radius, radius);
    if (!isEnabled()) return;

    const QRectF bar = barRect();
    const bool active = m_dragging != Part::None || m_hover != Part::None;
    QColor fill = style::accent();
    fill.setAlpha(active ? 150 : 100);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(bar, radius, radius);
    // The ends, which are what's dragged to change the range.
    const auto drawEnd = [&](double x, Part part) {
        QColor color = style::accent();
        if (m_dragging != part && m_hover != part) color.setAlpha(200);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(x, bar.top(), pixels(kEndWidth), bar.height()), 1.0, 1.0);
    };
    drawEnd(bar.left(), Part::Start);
    drawEnd(bar.right() - pixels(kEndWidth), Part::End);
}

TimeRangeSlider::Part TimeRangeSlider::partAt(const QPointF& position) const {
    const QRectF bar = barRect();
    const double grip = pixels(kGrip);
    const double x = position.x();
    if (x < bar.left() - grip || x > bar.right() + grip) return Part::None;
    // A short bar: whichever end is nearer.
    if (bar.width() <= 2 * grip) return x < bar.center().x() ? Part::Start : Part::End;
    if (x <= bar.left() + grip) return Part::Start;
    if (x >= bar.right() - grip) return Part::End;
    return Part::Middle;
}

void TimeRangeSlider::mousePressEvent(QMouseEvent* event) {
    const Part part = event->button() == Qt::LeftButton ? partAt(event->position()) : Part::None;
    if (part == Part::None) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = part;
    m_pressTime = axis().timeAt(event->position().x());
    m_pressRange = m_range;
    update();
    event->accept();
}

void TimeRangeSlider::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging == Part::None) {
        const Part hover = partAt(event->position());
        if (hover != m_hover) {
            m_hover = hover;
            setCursor(hover == Part::Middle ? Qt::OpenHandCursor
                      : hover == Part::None ? Qt::ArrowCursor
                                            : Qt::SizeHorCursor);
            update();
        }
        return;
    }
    const TimeAxis& shown = axis();
    const TimeRange full = shown.range();
    const double first = std::ceil(full.start);
    const double last = std::max(first, std::floor(full.end));
    const double moved = shown.timeAt(event->position().x()) - m_pressTime;
    TimeRange wanted = m_pressRange;
    if (m_dragging == Part::Start) {
        wanted.start = std::clamp(snappedToFrame(m_pressRange.start + moved), first, std::max(first, m_pressRange.end - 1.0));
    } else if (m_dragging == Part::End) {
        wanted.end = std::clamp(snappedToFrame(m_pressRange.end + moved), std::min(last, m_pressRange.start + 1.0), last);
    } else {
        // Both ends, by the same whole number of frames, as far as the track goes.
        const double shift = std::clamp(snappedToFrame(moved), std::min(0.0, first - m_pressRange.start),
                                        std::max(0.0, last - m_pressRange.end));
        wanted.start += shift;
        wanted.end += shift;
    }
    if (wanted != m_range) Q_EMIT rangeEdited(wanted);
    event->accept();
}

void TimeRangeSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || m_dragging == Part::None) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = Part::None;
    m_hover = partAt(event->position());
    update();
    event->accept();
}

void TimeRangeSlider::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    if (m_hover != Part::None && m_dragging == Part::None) {
        m_hover = Part::None;
        unsetCursor();
        update();
    }
}

}  // namespace usdcc::usd
