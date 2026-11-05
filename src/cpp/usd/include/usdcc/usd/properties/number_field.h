#pragma once

#include "usdcc/core/export.h"

#include <QColor>
#include <QPoint>
#include <QString>
#include <QWidget>

class QLineEdit;

namespace usdcc::usd::properties {

// The number control of the attribute editor (docs/ATTRIBUTE_EDITOR.md §4.2),
// painted to match the mockup: a dark rounded field, optionally with a colored
// axis prefix ("X 0.00"), or — in slider mode, which needs a finite range — a
// bar filled up to the value ("0.420").
//
//   Drag mode:   drag horizontally to scrub (Shift = fine, Ctrl = coarse);
//                a click without dragging types a value.
//   Slider mode: press/drag sets the value from the cursor position;
//                a double-click types a value.
//   Both:        Enter commits typed text, Esc cancels typing or a drag in
//                progress (restoring the value it started from).
//
// Edits are reported as editStarted / valueEdited (live, possibly many) /
// editFinished, or editCanceled, so a caller can make one undo step per
// gesture. setValue() never emits anything.
class USDCC_USD_UI_API NumberField : public QWidget {
    Q_OBJECT

public:
    explicit NumberField(QWidget* parent = nullptr);

    void setPrefix(const QString& text, const QColor& color);
    void setSuffix(const QString& suffix);
    void setDecimals(int decimals);
    void setInteger(bool integer);
    // The values allowed at all (a hard limit): everything is clamped to it.
    void setRange(double minimum, double maximum);
    // Scrub sensitivity: value change per pixel dragged.
    void setStep(double step);
    // Turns the field into a slider over [minimum, maximum] (a soft limit):
    // dragging stays inside it, but the value shown is never clamped to it and
    // a typed value may lie outside it.
    void setSliderRange(double minimum, double maximum);
    void setReadOnly(bool readOnly);

    double value() const { return m_value; }
    void setValue(double value);

    QString text() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void editStarted();
    void valueEdited(double value);
    void editFinished();
    void editCanceled();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    double clamped(double value) const;
    double valueAtX(int x) const;
    QRect valueRect() const;
    void applyLive(double value);
    void beginTextEdit();
    void endTextEdit(bool commit);

    double m_value = 0.0;
    double m_minimum;
    double m_maximum;
    double m_step = 0.01;
    int m_decimals = 2;
    bool m_integer = false;
    bool m_slider = false;
    double m_sliderMinimum = 0.0;
    double m_sliderMaximum = 1.0;
    bool m_readOnly = false;
    QString m_prefix;
    QColor m_prefixColor;
    QString m_suffix;

    // Gesture state.
    bool m_pressed = false;
    bool m_dragging = false;
    bool m_hovered = false;
    QPoint m_pressPos;
    double m_pressValue = 0.0;
    double m_dragAccum = 0.0;
    int m_lastX = 0;

    QLineEdit* m_editor = nullptr;
    bool m_textEditing = false;
};

}  // namespace usdcc::usd::properties
