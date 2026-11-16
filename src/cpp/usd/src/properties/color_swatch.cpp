#include "usdcc/usd/properties/color_swatch.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/property_style.h"

#include <QColorDialog>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace usdcc::usd::properties {

ColorSwatch::ColorSwatch(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setToolTip(tr("Click to pick a color"));
}

void ColorSwatch::setColor(const QColor& color) {
    m_color = color;
    update();
}

void ColorSwatch::setReadOnly(bool readOnly) {
    m_readOnly = readOnly;
    setCursor(readOnly ? Qt::ArrowCursor : Qt::PointingHandCursor);
    update();
}

QSize ColorSwatch::sizeHint() const { return usdcc::ui::scaled::size(style::kFieldHeight + 8, style::kFieldHeight); }

void ColorSwatch::setOutOfRange(bool outOfRange) {
    if (outOfRange == m_outOfRange) return;
    m_outOfRange = outOfRange;
    update();
}

void ColorSwatch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    shape.addRoundedRect(r, usdcc::ui::scaled::pixels(style::kRadius), usdcc::ui::scaled::pixels(style::kRadius));
    QColor fill = m_color;
    fill.setAlpha(255);  // alpha is edited as a number; the chip shows the color itself
    p.fillPath(shape, m_readOnly ? fill.darker(160) : fill);
    if (m_outOfRange) {
        // A corner triangle, light or dark against the color: more than the chip shows.
        const qreal size = std::min(r.width(), r.height()) * 0.45;
        QPainterPath corner;
        corner.moveTo(r.topRight());
        corner.lineTo(r.topRight() - QPointF(size, 0));
        corner.lineTo(r.topRight() + QPointF(0, size));
        corner.closeSubpath();
        p.save();
        p.setClipPath(shape);
        p.fillPath(corner, fill.lightnessF() > 0.6 ? QColor(0, 0, 0, 170) : QColor(255, 255, 255, 200));
        p.restore();
    }
    p.setPen(QPen(style::border(), 1.0));
    p.drawPath(shape);
}

void ColorSwatch::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || m_readOnly || !rect().contains(event->pos())) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const QColor original = m_color;
    QColorDialog dialog(m_color, window());
    dialog.setWindowTitle(tr("Pick Color"));
    if (m_showAlpha) {
        dialog.setOption(QColorDialog::ShowAlphaChannel);
    }
    Q_EMIT editStarted();
    connect(&dialog, &QColorDialog::currentColorChanged, this, [this](const QColor& c) {
        if (c.isValid() && c != m_color) {
            m_color = c;
            update();
            Q_EMIT colorEdited(c);
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        const QColor chosen = dialog.selectedColor();
        if (chosen.isValid() && chosen != m_color) {
            m_color = chosen;
            update();
            Q_EMIT colorEdited(chosen);
        }
        Q_EMIT editFinished();
    } else {
        m_color = original;
        update();
        Q_EMIT editCanceled();
    }
}

}  // namespace usdcc::usd::properties
