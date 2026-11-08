#include "usdcc/usd/window_preview.h"

#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <QPainter>
#include <QPixmap>
#include <QWidget>

namespace usdcc::usd {

QImage captureWindowPreview(QWidget* window) {
    if (!window) return {};
    // The widgets draw themselves, whatever covers the window or wherever it is
    // (minimised, on a locked screen). A viewport is a native window of its own that
    // grab() leaves black, so its last frame goes in its place.
    QImage image = window->grab().toImage();
    if (image.isNull()) return image;
    QPainter painter(&image);
    const QPoint origin = window->mapToGlobal(QPoint(0, 0));
    for (ViewportViewPanel* panel : window->findChildren<ViewportViewPanel*>()) {
        HydraViewportWindow* viewport = panel->hydraWindow();
        if (!panel->isVisible() || !viewport || !viewport->isExposed()) continue;
        const QImage frame = viewport->grabFrame();
        if (frame.isNull()) continue;
        const QRect where(viewport->mapToGlobal(QPoint(0, 0)) - origin, viewport->size());
        painter.drawImage(where, frame);
    }
    return image;
}

}  // namespace usdcc::usd
