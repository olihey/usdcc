#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/time_axis.h"

#include <QWidget>

namespace usdcc::usd {

// The timeline's range slider (docs/ANIMATION.md §5.5): a bar for the playback range
// inside a track for everything it may cover. Dragging an end of the bar moves that end;
// dragging its middle moves both. Whole frames, at least one frame long. Knows no stage.
class USDCC_USD_UI_API TimeRangeSlider : public QWidget {
    Q_OBJECT

public:
    explicit TimeRangeSlider(QWidget* parent = nullptr);

    // The track: what the bar can be dragged over.
    void setFullRange(const TimeRange& range);
    TimeRange fullRange() const { return m_axis.range(); }
    // The bar. Not clamped to the track: what lies outside it isn't drawn.
    void setRange(const TimeRange& range);
    TimeRange range() const { return m_range; }

    // The bar, in the widget's pixels.
    QRectF barRect() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    // While dragging: the range wanted. The bar doesn't move until setRange() says so.
    void rangeEdited(const usdcc::usd::TimeRange& range);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    enum class Part { None, Start, End, Middle };
    Part partAt(const QPointF& position) const;
    // The track's time to pixels, for the width it has now.
    const TimeAxis& axis() const;

    mutable TimeAxis m_axis;
    TimeRange m_range{0.0, 1.0};
    Part m_dragging = Part::None;
    Part m_hover = Part::None;
    double m_pressTime = 0.0;  // under the cursor at the press
    TimeRange m_pressRange;
};

}  // namespace usdcc::usd
