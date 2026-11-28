#pragma once
#include "usdcc/ui/navigation.h"
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
    // The stage's up axis, which the orbit is about (usdcc::usd::orbitCameraAxes()).
    void setZUp(bool value) { zUp = value; }
    usdcc::usd::RenderCamera camera(double aspect) const;
    void beginOrbit() { reversed = usdcc::usd::orbitIsUpsideDown(pitch); }
private:
    usdcc::usd::RenderCamera view;
    PXR_NS::GfVec3d target{0};
    double distance = 10, yaw = 45, pitch = 20, roll = 0;
    bool reversed = false;
    bool zUp = false;
};

// Input in logical coordinates normalized by the displayed image's height.
// Letterbox clicks do nothing; no gesture picks or changes scene selection.
// Which drag orbits, pans and zooms is Settings > Navigation's (docs/NAVIGATION.md); a
// left drag orbits whatever the scheme, as this view has no tools.
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
    // One step of the scheme's, from a drag, the wheel or a pinch.
    void navigate(const usdcc::ui::NavigationStep& step);
    QWindow* window;
    usdcc::ui::NavigationInput navigation{usdcc::ui::NavigationSpace::View3D};
};
}  // namespace usdcc::rtx
