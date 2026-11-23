#pragma once

#include "usdcc/ui/theme.h"

#include <QColor>

namespace usdcc::usd::properties {

// Colors for the custom-painted property widgets (NumberField, ColorSwatch,
// section headers). The theme's (theme.h). The axes' stay as
// they are in every theme.
namespace style {
inline QColor inset() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::Inset); }
// A field that can't be edited: flatter than inset(), nearer the panel, with
// a fainter border (app.qss, QLineEdit:read-only).
inline QColor readOnlyFill() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::SurfaceReadOnly); }
inline QColor readOnlyBorder() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::Overlay, 13); }  // 5%
inline QColor border() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::Overlay, 20); }        // 8%
inline QColor borderHover() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::Overlay, 46); }   // 18%
inline QColor accent() { return usdcc::ui::theme::accent(); }
inline QColor sliderFill() {
    QColor color = accent();
    color.setAlpha(90);
    return color;
}
inline QColor text() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::Ink, 191); }         // 75%
inline QColor textMuted() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::Ink, 102); }    // 40%
inline QColor axisX() { return QColor(0xff, 0x5f, 0x56); }
inline QColor axisY() { return QColor(0x4f, 0xd6, 0x71); }
inline QColor axisZ() { return QColor(0x3d, 0x8b, 0xfd); }  // stays blue, whatever the accent
inline QColor axisW() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::Ink, 128); }
constexpr int kRadius = 3;
constexpr int kFieldHeight = 20;
}  // namespace style

}  // namespace usdcc::usd::properties
