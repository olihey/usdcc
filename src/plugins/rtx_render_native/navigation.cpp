#include "navigation.h"
#include <pxr/base/gf/math.h>
#include <QContextMenuEvent>
#include <QCursor>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace usdcc::rtx {
using namespace PXR_NS;
using namespace usdcc::usd;
namespace {
constexpr double degrees = 180.0 / std::numbers::pi;
GfVec3d row(const GfMatrix4d& matrix, int i) { return GfVec3d(matrix[i][0], matrix[i][1], matrix[i][2]); }
}
void NavigationCamera::seed(const RenderCamera& camera, const std::optional<GfRange3d>& bounds) {
    view = camera;
    view.prim = {};
    auto back = row(camera.cameraToWorld, 2).GetNormalized();
    auto right = row(camera.cameraToWorld, 0).GetNormalized();
    const auto eye = row(camera.cameraToWorld, 3);
    distance = bounds ? GfDot(bounds->GetMidpoint() - eye, -back) : kDefaultOrbitDistance;
    if (!std::isfinite(distance) || distance <= 1e-4) distance = kDefaultOrbitDistance;
    target = eye - back * distance;
    yaw = std::atan2(back[0], back[2]) * degrees;
    pitch = std::asin(std::clamp(back[1], -1.0, 1.0)) * degrees;
    auto axes = orbitCameraAxes(target, distance, yaw, pitch);
    roll = std::atan2(GfDot(right, axes.up), GfDot(right, axes.right));
}
void NavigationCamera::orbit(QPointF delta) {
    yaw = wrapDegrees(yaw - (reversed ? -1.0 : 1.0) * delta.x() * 360);
    pitch = wrapDegrees(pitch + delta.y() * 360);
}
void NavigationCamera::pan(QPointF delta) {
    auto matrix = camera(view.aspect).cameraToWorld;
    const double height = view.orthographic ? view.orthoWidth / std::max(view.aspect, 1e-6)
                                         : 2 * distance * std::tan(view.fovY / degrees / 2);
    target += row(matrix, 0) * (-delta.x() * height) + row(matrix, 1) * (delta.y() * height);
}
void NavigationCamera::zoom(double steps) {
    const double factor = std::pow(.9, std::clamp(steps, -100.0, 100.0));
    if (view.orthographic) view.orthoWidth = std::max(1e-4, view.orthoWidth * factor);
    else distance = std::max(1e-4, distance * factor);
}
void NavigationCamera::frame(const GfRange3d& bounds) {
    const auto framed = frameOrbitCamera(bounds, view.fovY, view.aspect, distance);
    target = framed.target;
    distance = std::max(1e-4, framed.distance);
    if (view.orthographic)
        view.orthoWidth = std::max(1e-4, bounds.GetSize().GetLength() * std::max(1.0, view.aspect));
}
void NavigationCamera::reset() {
    target = GfVec3d(0);
    distance = kDefaultOrbitDistance;
    yaw = kDefaultOrbitYaw;
    pitch = kDefaultOrbitPitch;
    roll = 0;
    view = RenderCamera{};
}
RenderCamera NavigationCamera::camera(double aspect) const {
    auto result = view.atAspect(aspect);
    const auto axes = orbitCameraAxes(target, distance, yaw, pitch);
    const auto right = axes.right * std::cos(roll) + axes.up * std::sin(roll);
    const auto up = axes.up * std::cos(roll) - axes.right * std::sin(roll);
    result.cameraToWorld = GfMatrix4d(1);
    for (int i = 0; i < 3; ++i) {
        result.cameraToWorld[0][i] = right[i];
        result.cameraToWorld[1][i] = up[i];
        result.cameraToWorld[2][i] = -axes.forward[i];
        result.cameraToWorld[3][i] = axes.eye[i];
    }
    const auto clipping = orbitClipping(distance);
    result.nearClip = clipping.nearClip;
    result.farClip = clipping.farClip;
    return result;
}
NavigationInput::NavigationInput(QWindow* value, QObject* owner) : QObject(owner), window(value) {
    window->installEventFilter(this);
}
QRectF NavigationInput::imageRect() const {
    QSize size = imageSize ? imageSize() : window->size();
    if (!size.isValid() || size.isEmpty()) return {};
    size.scale(window->size(), Qt::KeepAspectRatio);
    return QRectF((window->width() - size.width()) / 2.0, (window->height() - size.height()) / 2.0,
                  size.width(), size.height());
}
bool NavigationInput::eventFilter(QObject*, QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers() == Qt::NoModifier && (key->key() == Qt::Key_F || key->key() == Qt::Key_Home)) {
            key->accept(); return true;
        }
    }
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if ((mouse->button() != Qt::LeftButton && mouse->button() != Qt::MiddleButton) ||
            !imageRect().contains(mouse->position())) return false;
        dragging = mouse->button(); previous = mouse->position();
        window->requestActivate();
        window->setMouseGrabEnabled(true);
        if (begin) begin(dragging == Qt::LeftButton);
        return true;
    }
    if (event->type() == QEvent::MouseMove && dragging != Qt::NoButton) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const auto delta = mouse->position() - previous;
        previous = mouse->position();
        const double height = std::max(1.0, imageRect().height());
        const auto screenSize = window->screen() ? window->screen()->geometry().size() : QSize();
        const bool stale = screenSize.isValid() &&
                           (std::abs(delta.x()) > screenSize.width() / 2 || std::abs(delta.y()) > screenSize.height() / 2);
        if (!stale && dragging == Qt::LeftButton && orbit) orbit(delta / height);
        if (!stale && dragging == Qt::MiddleButton && pan) pan(delta / height);
        if (auto* screen = window->screen()) {
            auto rect = screen->geometry();
            auto p = mouse->globalPosition().toPoint();
            if (auto wrapped = wrapCursorAtEdges(p.x(), p.y(), rect.left(), rect.top(), rect.right(), rect.bottom())) {
                const QPoint to(wrapped->x, wrapped->y);
                QCursor::setPos(to); previous = window->mapFromGlobal(to);
            }
        }
        return true;
    }
    if (event->type() == QEvent::MouseButtonRelease && dragging != Qt::NoButton) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == dragging) { dragging = Qt::NoButton; window->setMouseGrabEnabled(false); return true; }
    }
    if (event->type() == QEvent::Hide || event->type() == QEvent::FocusOut) {
        dragging = Qt::NoButton; window->setMouseGrabEnabled(false);
    }
    if (event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (!imageRect().contains(wheel->position())) return false;
        const double steps = wheel->angleDelta().isNull() ? wheel->pixelDelta().y() / 120.0
                                                        : wheel->angleDelta().y() / 120.0;
        if (steps && zoom) { if (begin) begin(false); zoom(steps); }
        return true;
    }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers() == Qt::NoModifier && key->key() == Qt::Key_F && fit) { fit(); return true; }
        if (key->modifiers() == Qt::NoModifier && key->key() == Qt::Key_Home && reset) { reset(); return true; }
    }
    if (event->type() == QEvent::ContextMenu) {
        QMenu menu;
        menu.addAction(tr("Fit to View"), this, [this] { if (fit) fit(); });
        menu.addAction(tr("Reset Camera"), this, [this] { if (reset) reset(); });
        menu.exec(static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
    }
    return false;
}
}  // namespace usdcc::rtx
