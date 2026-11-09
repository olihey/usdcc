#include "usdcc/usd/properties/number_field.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/properties/property_style.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace usdcc::usd::properties {

namespace {
constexpr int kPadding = 6;
constexpr int kDragThreshold = 3;
}  // namespace

NumberField::NumberField(QWidget* parent)
    : QWidget(parent),
      m_minimum(std::numeric_limits<double>::lowest()),
      m_maximum(std::numeric_limits<double>::max()) {
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
}

void NumberField::setPrefix(const QString& text, const QColor& color) {
    m_prefix = text;
    m_prefixColor = color;
    updateGeometry();
    update();
}

void NumberField::setSuffix(const QString& suffix) {
    m_suffix = suffix;
    update();
}

void NumberField::setDecimals(int decimals) {
    m_decimals = std::max(0, decimals);
    update();
}

void NumberField::setInteger(bool integer) {
    m_integer = integer;
    if (integer) {
        m_step = std::max(m_step, 0.1);
    }
    update();
}

void NumberField::setRange(double minimum, double maximum) {
    m_minimum = std::min(minimum, maximum);
    m_maximum = std::max(minimum, maximum);
    m_value = clamped(m_value);
    update();
}

void NumberField::setStep(double step) { m_step = step > 0.0 ? step : m_step; }

void NumberField::setSliderRange(double minimum, double maximum) {
    m_slider = std::isfinite(minimum) && std::isfinite(maximum) && maximum > minimum;
    m_sliderMinimum = minimum;
    m_sliderMaximum = maximum;
    update();
}

void NumberField::setReadOnly(bool readOnly) {
    m_readOnly = readOnly;
    if (readOnly && m_textEditing) {
        endTextEdit(false);
    }
    update();
}

void NumberField::setValue(double value) {
    m_value = clamped(value);
    update();
}

double NumberField::clamped(double value) const {
    double v = std::clamp(value, m_minimum, m_maximum);
    if (m_integer) {
        v = std::round(v);
    }
    return v;
}

QString NumberField::text() const {
    const QString number = m_integer ? QString::number(static_cast<long long>(std::llround(m_value)))
                                     : QString::number(m_value, 'f', m_decimals);
    return number + m_suffix;
}

QSize NumberField::sizeHint() const { return usdcc::ui::scaled::size(72, style::kFieldHeight); }

QSize NumberField::minimumSizeHint() const { return usdcc::ui::scaled::size(36, style::kFieldHeight); }

QRect NumberField::valueRect() const {
    int left = usdcc::ui::scaled::pixels(kPadding);
    if (!m_prefix.isEmpty()) {
        left += fontMetrics().horizontalAdvance(m_prefix) + usdcc::ui::scaled::pixels(5);
    }
    return rect().adjusted(left, 0, -usdcc::ui::scaled::pixels(kPadding), 0);
}

double NumberField::valueAtX(int x) const {
    const QRect r = rect().adjusted(1, 0, -1, 0);
    const double f = std::clamp(static_cast<double>(x - r.left()) / std::max(1, r.width()), 0.0, 1.0);
    return m_sliderMinimum + f * (m_sliderMaximum - m_sliderMinimum);
}

void NumberField::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    shape.addRoundedRect(r, usdcc::ui::scaled::pixels(style::kRadius), usdcc::ui::scaled::pixels(style::kRadius));
    p.fillPath(shape, m_readOnly ? style::readOnlyFill() : style::inset());

    if (m_slider) {
        const double f = (m_value - m_sliderMinimum) / (m_sliderMaximum - m_sliderMinimum);
        QRectF fill = r;
        fill.setWidth(r.width() * std::clamp(f, 0.0, 1.0));
        p.save();
        p.setClipPath(shape);
        p.fillRect(fill, style::sliderFill());
        p.restore();
    }

    const bool active = m_dragging || m_textEditing || hasFocus();
    p.setPen(QPen(active                     ? style::accent()
                  : m_readOnly               ? style::readOnlyBorder()
                  : m_hovered                ? style::borderHover()
                                             : style::border(),
                  1.0));
    p.drawPath(shape);

    if (!m_prefix.isEmpty()) {
        p.setPen(m_readOnly ? style::textMuted() : m_prefixColor);
        p.drawText(rect().adjusted(usdcc::ui::scaled::pixels(kPadding), 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, m_prefix);
    }
    if (!m_textEditing) {
        p.setPen(m_readOnly ? style::textMuted() : style::text());
        p.drawText(valueRect(), Qt::AlignVCenter | Qt::AlignLeft,
                   fontMetrics().elidedText(text(), Qt::ElideRight, valueRect().width()));
    }
}

void NumberField::applyLive(double value) {
    const double v = clamped(value);
    if (v == m_value) {
        return;
    }
    m_value = v;
    update();
    Q_EMIT valueEdited(v);
}

void NumberField::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || m_readOnly || m_textEditing) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    m_pressed = true;
    m_pressPos = event->pos();
    m_lastX = event->pos().x();
    m_pressValue = m_value;
    m_dragAccum = 0.0;
    if (m_slider) {
        m_dragging = true;
        Q_EMIT editStarted();
        applyLive(valueAtX(event->pos().x()));
    }
}

void NumberField::mouseMoveEvent(QMouseEvent* event) {
    if (!m_pressed) {
        return;
    }
    if (m_slider) {
        applyLive(valueAtX(event->pos().x()));
        return;
    }
    if (!m_dragging) {
        if (std::abs(event->pos().x() - m_pressPos.x()) < kDragThreshold) {
            return;
        }
        m_dragging = true;
        m_lastX = event->pos().x();
        setCursor(Qt::SizeHorCursor);
        Q_EMIT editStarted();
    }
    const int dx = event->pos().x() - m_lastX;
    m_lastX = event->pos().x();
    double multiplier = 1.0;
    if (event->modifiers() & Qt::ShiftModifier) {
        multiplier = 0.1;
    } else if (event->modifiers() & Qt::ControlModifier) {
        multiplier = 10.0;
    }
    m_dragAccum += dx * m_step * multiplier;
    applyLive(m_pressValue + m_dragAccum);
}

void NumberField::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !m_pressed) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_pressed = false;
    unsetCursor();
    if (m_dragging) {
        m_dragging = false;
        update();
        Q_EMIT editFinished();
    } else if (!m_slider) {
        beginTextEdit();
    }
}

void NumberField::mouseDoubleClickEvent(QMouseEvent* event) {
    if (m_slider && !m_readOnly && event->button() == Qt::LeftButton) {
        beginTextEdit();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void NumberField::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && m_dragging) {
        m_dragging = false;
        m_pressed = false;
        unsetCursor();
        m_value = m_pressValue;
        update();
        Q_EMIT editCanceled();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !m_readOnly) {
        beginTextEdit();
        return;
    }
    QWidget::keyPressEvent(event);
}

void NumberField::wheelEvent(QWheelEvent* event) {
    // Never change a value by accident while scrolling the attribute panel.
    event->ignore();
}

void NumberField::enterEvent(QEnterEvent*) {
    m_hovered = true;
    update();
}

void NumberField::leaveEvent(QEvent*) {
    m_hovered = false;
    update();
}

void NumberField::resizeEvent(QResizeEvent*) {
    if (m_editor) {
        m_editor->setGeometry(valueRect());
    }
}

void NumberField::beginTextEdit() {
    if (m_readOnly || m_textEditing) {
        return;
    }
    if (!m_editor) {
        m_editor = new QLineEdit(this);
        m_editor->setFrame(false);
        m_editor->setStyleSheet(QStringLiteral(
            "QLineEdit { background: transparent; border: none; padding: 0; min-height: 0; color: rgba(255,255,255,90%); }"));
        m_editor->installEventFilter(this);
        connect(m_editor, &QLineEdit::returnPressed, this, [this]() { endTextEdit(true); });
    }
    m_textEditing = true;
    m_editor->setGeometry(valueRect());
    m_editor->setText(m_integer ? QString::number(static_cast<long long>(std::llround(m_value)))
                                : QString::number(m_value, 'g', 12));
    m_editor->show();
    m_editor->setFocus(Qt::OtherFocusReason);
    m_editor->selectAll();
    update();
}

void NumberField::endTextEdit(bool commit) {
    if (!m_textEditing) {
        return;
    }
    m_textEditing = false;
    const QString typed = m_editor->text().trimmed();
    // Enter or Esc: the focus stays on this field, rather than Qt moving it on when the
    // typing box it's in goes. (Leaving by Tab or a click, the focus has already gone.)
    if (m_editor->hasFocus()) setFocus(Qt::OtherFocusReason);
    m_editor->hide();
    update();
    if (!commit) {
        return;
    }
    bool ok = false;
    QString normalized = typed;
    normalized.replace(QLatin1Char(','), QLatin1Char('.'));
    if (normalized.endsWith(m_suffix) && !m_suffix.isEmpty()) {
        normalized.chop(m_suffix.size());
    }
    const double parsed = QLocale::c().toDouble(normalized.trimmed(), &ok);
    if (!ok) {
        return;
    }
    const double v = clamped(parsed);
    if (v == m_value) {
        return;
    }
    Q_EMIT editStarted();
    m_value = v;
    update();
    Q_EMIT valueEdited(v);
    Q_EMIT editFinished();
}

bool NumberField::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_editor) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            endTextEdit(false);
            return true;
        }
        if (event->type() == QEvent::FocusOut) {
            endTextEdit(true);
        }
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace usdcc::usd::properties
