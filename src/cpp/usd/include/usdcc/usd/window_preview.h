#pragma once

#include "usdcc/core/export.h"

#include <QImage>

class QWidget;

namespace usdcc::usd {

// A picture of `window` as it looks, viewports included, for a session's preview
// (docs/SESSIONS.md §6.2): the widgets as they draw themselves, with each viewport's
// last frame in its place (QWidget::grab() alone leaves the viewports black). Nothing
// that covers the window is in it, and it works while the window is minimised or
// the screen is locked.
USDCC_USD_UI_API QImage captureWindowPreview(QWidget* window);

}  // namespace usdcc::usd
