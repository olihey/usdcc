#pragma once

#include <QColor>
#include <QWidget>

namespace usdcc::usd::properties {

// A color chip (the mockup's Base Color square). Clicking it opens a
// QColorDialog; picking in the dialog is reported live (colorEdited), and the
// dialog's OK/Cancel end the edit with editFinished/editCanceled.
//
// Works in display (sRGB-encoded) colors: USD colors are linear, so the
// ColorEditor converts both ways (see linearToDisplay()).
class ColorSwatch : public QWidget {
    Q_OBJECT

public:
    explicit ColorSwatch(QWidget* parent = nullptr);

    QColor color() const { return m_color; }
    void setColor(const QColor& color);  // emits nothing
    void setShowAlpha(bool showAlpha) { m_showAlpha = showAlpha; }
    void setReadOnly(bool readOnly);

    QSize sizeHint() const override;

    // Linear <-> sRGB transfer for one channel in [0, 1].
    static double linearToDisplay(double linear);
    static double displayToLinear(double display);

Q_SIGNALS:
    void editStarted();
    void colorEdited(const QColor& color);
    void editFinished();
    void editCanceled();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QColor m_color = Qt::white;
    bool m_showAlpha = false;
    bool m_readOnly = false;
};

}  // namespace usdcc::usd::properties
