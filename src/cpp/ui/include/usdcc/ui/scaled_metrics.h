#pragma once

#include "usdcc/ui/scale_controller.h"

#include <QLayout>
#include <QSize>
#include <QWidget>

namespace usdcc::ui::scaled {

inline int pixels(int base) { return UiScaleController::instance().pixels(base); }
inline QSize size(int width, int height) { return {pixels(width), pixels(height)}; }

// Keep each explicitly authored metric bound to its original value. This also
// handles controls created after the user has already changed the scale.
template <typename T>
void fixedHeight(T* widget, int base) {
    UiScaleController::instance().bind(widget, "height", [widget, base]() { widget->setFixedHeight(pixels(base)); });
}
template <typename T>
void fixedWidth(T* widget, int base) {
    UiScaleController::instance().bind(widget, "width", [widget, base]() { widget->setFixedWidth(pixels(base)); });
}
template <typename T>
void minimumWidth(T* widget, int base) {
    UiScaleController::instance().bind(widget, "minimumWidth", [widget, base]() { widget->setMinimumWidth(pixels(base)); });
}
template <typename T>
void fixedSize(T* widget, int width, int height) {
    fixedWidth(widget, width);
    fixedHeight(widget, height);
}
template <typename T>
void iconSize(T* widget, int width, int height) {
    UiScaleController::instance().bind(widget, "icon", [widget, width, height]() { widget->setIconSize(size(width, height)); });
}
template <typename T>
void margins(T* target, int left, int top, int right, int bottom) {
    UiScaleController::instance().bind(target, "margins", [=]() {
        target->setContentsMargins(pixels(left), pixels(top), pixels(right), pixels(bottom));
    });
}
inline void spacing(QLayout* layout, int base) {
    UiScaleController::instance().bind(layout, "spacing", [=]() { layout->setSpacing(pixels(base)); });
}

}  // namespace usdcc::ui::scaled
