#pragma once

#include "usdcc/core/export.h"

#include <QColor>
#include <QWidget>

namespace usdcc::usd::properties {

// A color chip (the mockup's Base Color square). Clicking it opens a
// QColorDialog; picking in the dialog is reported live (colorEdited), and the
// dialog's OK/Cancel end the edit with editFinished/editCanceled.
//
// Works in the color picking space's 0-1 values: the numeric editor converts the
// attribute's linear values to it and back (ColorManagement::toPicker(),
// docs/COLOR_MANAGEMENT.md §7).
class USDCC_USD_UI_API ColorSwatch : public QWidget {
    Q_OBJECT

public:
    explicit ColorSwatch(QWidget* parent = nullptr);

    QColor color() const { return m_color; }
    void setColor(const QColor& color);  // emits nothing
    void setShowAlpha(bool showAlpha) { m_showAlpha = showAlpha; }
    void setReadOnly(bool readOnly);
    // The color has values the swatch can't show (above 1, an emission's or a light's):
    // a corner mark says so, and the tooltip.
    void setOutOfRange(bool outOfRange);
    bool isOutOfRange() const { return m_outOfRange; }

    QSize sizeHint() const override;

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
    bool m_outOfRange = false;
};

}  // namespace usdcc::usd::properties
