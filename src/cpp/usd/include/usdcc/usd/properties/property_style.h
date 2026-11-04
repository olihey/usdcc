#pragma once

#include "usdcc/ui/theme.h"

#include <QColor>

namespace usdcc::usd::properties {

// Colors for the custom-painted property widgets (NumberField, ColorSwatch,
// section headers). Same tokens as theme/dark.qss — kept in sync by hand, as
// the stylesheet's header comment asks; the accent is the setting's
// (usdcc::ui::theme::accent()).
namespace style {
inline QColor inset() { return QColor(0x14, 0x14, 0x16); }
// A field that can't be edited: flatter than inset(), nearer the panel, with
// a fainter border (dark.qss, QLineEdit:read-only).
inline QColor readOnlyFill() { return QColor(0x22, 0x22, 0x25); }
inline QColor readOnlyBorder() { return QColor(255, 255, 255, 13); }  // 5%
inline QColor border() { return QColor(255, 255, 255, 20); }        // 8%
inline QColor borderHover() { return QColor(255, 255, 255, 46); }   // 18%
inline QColor accent() { return usdcc::ui::theme::accent(); }
inline QColor sliderFill() {
    QColor color = accent();
    color.setAlpha(90);
    return color;
}
inline QColor text() { return QColor(255, 255, 255, 191); }         // 75%
inline QColor textMuted() { return QColor(255, 255, 255, 102); }    // 40%
inline QColor axisX() { return QColor(0xff, 0x5f, 0x56); }
inline QColor axisY() { return QColor(0x4f, 0xd6, 0x71); }
inline QColor axisZ() { return QColor(0x3d, 0x8b, 0xfd); }  // stays blue, whatever the accent
inline QColor axisW() { return QColor(255, 255, 255, 128); }
constexpr int kRadius = 3;
constexpr int kFieldHeight = 20;
}  // namespace style

}  // namespace usdcc::usd::properties
