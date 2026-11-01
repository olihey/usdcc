#pragma once

#include <QColor>

namespace usdcc::usd::properties {

// Colors for the custom-painted property widgets (NumberField, ColorSwatch,
// section headers). Same tokens as theme/dark.qss — kept in sync by hand, as
// the stylesheet's header comment asks.
namespace style {
inline QColor inset() { return QColor(0x14, 0x14, 0x16); }
inline QColor border() { return QColor(255, 255, 255, 20); }        // 8%
inline QColor borderHover() { return QColor(255, 255, 255, 46); }   // 18%
inline QColor accent() { return QColor(0x3d, 0x8b, 0xfd); }
inline QColor sliderFill() { return QColor(0x3d, 0x8b, 0xfd, 90); }
inline QColor text() { return QColor(255, 255, 255, 191); }         // 75%
inline QColor textMuted() { return QColor(255, 255, 255, 102); }    // 40%
inline QColor axisX() { return QColor(0xff, 0x5f, 0x56); }
inline QColor axisY() { return QColor(0x4f, 0xd6, 0x71); }
inline QColor axisZ() { return QColor(0x3d, 0x8b, 0xfd); }
inline QColor axisW() { return QColor(255, 255, 255, 128); }
constexpr int kRadius = 3;
constexpr int kFieldHeight = 20;
}  // namespace style

}  // namespace usdcc::usd::properties
