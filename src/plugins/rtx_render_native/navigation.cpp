#include "navigation.h"
#include <pxr/base/gf/math.h>
#include <QContextMenuEvent>
#include <QCursor>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
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
    // About the stage's up axis, so an upright camera seeds without roll and orbits level.
    const auto angles = orbitAnglesLookingFrom(back, 0.0, zUp);
    yaw = angles.yaw;
    pitch = angles.pitch;
    auto axes = orbitCameraAxes(target, distance, yaw, pitch, zUp);
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
    const double factor = usdcc::ui::navigationZoomFactor(steps);
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
    const auto axes = orbitCameraAxes(target, distance, yaw, pitch, zUp);
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
void NavigationInput::navigate(const usdcc::ui::NavigationStep& step) {
    const double height = std::max(1.0, imageRect().height());
    switch (step.action) {
        case usdcc::ui::NavigationAction::Orbit: if (orbit) orbit(step.delta / height); break;
        case usdcc::ui::NavigationAction::Pan: if (pan) pan(step.delta / height); break;
        case usdcc::ui::NavigationAction::Zoom: if (step.zoom && zoom) zoom(step.zoom); break;
        case usdcc::ui::NavigationAction::None: break;
    }
}
bool NavigationInput::eventFilter(QObject*, QEvent* event) {
    using usdcc::ui::NavigationAction;
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (usdcc::ui::isFrameKey(key) || (key->modifiers() == Qt::NoModifier && key->key() == Qt::Key_Home)) {
            key->accept(); return true;
        }
    }
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (!imageRect().contains(mouse->position())) return false;
        // One drag at a time: another button during it isn't another.
        if (navigation.dragging() != NavigationAction::None) return true;
        NavigationAction action = navigation.press(mouse);
        if (action == NavigationAction::None && mouse->button() == Qt::LeftButton) {
            action = NavigationAction::Orbit;
            navigation.begin(action, Qt::LeftButton, mouse->position());
        }
        if (action == NavigationAction::None) return false;
        window->requestActivate();
        window->setMouseGrabEnabled(true);
        if (begin) begin(action == NavigationAction::Orbit);
        return true;
    }
    if (event->type() == QEvent::MouseMove && navigation.dragging() != NavigationAction::None) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const usdcc::ui::NavigationStep step = navigation.move(mouse);
        const auto screenSize = window->screen() ? window->screen()->geometry().size() : QSize();
        const bool stale = screenSize.isValid() && (std::abs(step.delta.x()) > screenSize.width() / 2 ||
                                                    std::abs(step.delta.y()) > screenSize.height() / 2);
        if (!stale) navigate(step);
        if (auto* screen = window->screen()) {
            auto rect = screen->geometry();
            auto p = mouse->globalPosition().toPoint();
            if (auto wrapped = wrapCursorAtEdges(p.x(), p.y(), rect.left(), rect.top(), rect.right(), rect.bottom())) {
                const QPoint to(wrapped->x, wrapped->y);
                QCursor::setPos(to); navigation.setPosition(QPointF(window->mapFromGlobal(to)));
            }
        }
        return true;
    }
    if (event->type() == QEvent::MouseButtonRelease && navigation.dragging() != NavigationAction::None) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (navigation.release(mouse)) {
            window->setMouseGrabEnabled(false);
            return true;
        }
    }
    if (event->type() == QEvent::Hide || event->type() == QEvent::FocusOut) {
        navigation.cancel(); window->setMouseGrabEnabled(false);
    }
    if (event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (!imageRect().contains(wheel->position())) return false;
        if (const usdcc::ui::NavigationStep step = navigation.wheel(wheel)) {
            if (begin) begin(step.action == NavigationAction::Orbit);
            navigate(step);
        }
        return true;
    }
    if (event->type() == QEvent::NativeGesture) {
        // A pinch zooms, as the wheel does.
        if (const usdcc::ui::NavigationStep step = navigation.gesture(event)) {
            if (begin) begin(false);
            navigate(step);
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (usdcc::ui::isFrameKey(key) && fit) { fit(); return true; }
        if (key->modifiers() == Qt::NoModifier && key->key() == Qt::Key_Home && reset) { reset(); return true; }
    }
    if (event->type() == QEvent::ContextMenu) {
        // Not for a right button that navigates (Alt+Right zooms in the usdcc scheme).
        if (navigation.takeContextMenu(event)) return true;
        QMenu menu;
        menu.addAction(tr("Fit to View"), this, [this] { if (fit) fit(); });
        menu.addAction(tr("Reset Camera"), this, [this] { if (reset) reset(); });
        menu.exec(static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
    }
    return false;
}
}  // namespace usdcc::rtx
