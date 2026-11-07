#pragma once
#include "usdcc/usd/render_view_panel.h"
#include "usdcc/usd/orbit_camera.h"
#include <QObject>
#include <QPointF>
#include <QWindow>
#include <functional>

namespace usdcc::rtx {
// Panel-local viewing state; no stage, selection, or undo writes.
class NavigationCamera {
public:
    void seed(const usdcc::usd::RenderCamera& camera, const std::optional<PXR_NS::GfRange3d>& bounds);
    void orbit(QPointF delta);
    void pan(QPointF delta);
    void zoom(double steps);
    void frame(const PXR_NS::GfRange3d& bounds);
    void reset();
    void setAspect(double aspect) { view = view.atAspect(aspect); }
    usdcc::usd::RenderCamera camera(double aspect) const;
    void beginOrbit() { reversed = usdcc::usd::orbitIsUpsideDown(pitch); }
private:
    usdcc::usd::RenderCamera view;
    PXR_NS::GfVec3d target{0};
    double distance = 10, yaw = 45, pitch = 20, roll = 0;
    bool reversed = false;
};

// Input in logical coordinates normalized by the displayed image's height.
// Letterbox clicks do nothing; no gesture picks or changes scene selection.
class NavigationInput final : public QObject {
public:
    NavigationInput(QWindow* window, QObject* owner);
    std::function<QSize()> imageSize;
    std::function<void(bool)> begin;
    std::function<void(QPointF)> orbit, pan;
    std::function<void(double)> zoom;
    std::function<void()> fit, reset;
protected:
    bool eventFilter(QObject*, QEvent*) override;
private:
    QRectF imageRect() const;
    QWindow* window;
    QPointF previous;
    Qt::MouseButton dragging = Qt::NoButton;
};
}  // namespace usdcc::rtx
