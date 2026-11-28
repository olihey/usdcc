#pragma once

// How every view is moved around with the mouse (docs/NAVIGATION.md): the scheme that says
// which button with which modifiers orbits, pans and zooms, the Navigation settings it is
// chosen and edited in, and NavigationInput, which turns a view's mouse, wheel and pinch
// events into steps of the same size everywhere.
//
// A view keeps its own math (its camera, its time axis) and nothing about buttons:
//
//   usdcc::ui::NavigationInput m_navigation{usdcc::ui::NavigationSpace::Editor2D};
//
//   void mousePressEvent(QMouseEvent* event) override {
//       if (m_navigation.press(event) != usdcc::ui::NavigationAction::None) return;
//       ...                                             // the view's own clicks
//   }
//   void mouseMoveEvent(QMouseEvent* event) override { navigate(m_navigation.move(event)); }
//   void mouseReleaseEvent(QMouseEvent* event) override { m_navigation.release(event); }
//   void wheelEvent(QWheelEvent* event) override { navigate(m_navigation.wheel(event)); }

#include "usdcc/core/export.h"
#include "usdcc/core/mouse_binding.h"
#include "usdcc/core/settings.h"

#include <QPoint>
#include <QPointF>
#include <QString>
#include <Qt>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QNativeGestureEvent;
class QWheelEvent;

namespace usdcc::ui {

enum class NavigationAction { None, Orbit, Pan, Zoom };

// The two kinds of view a scheme binds separately, as the applications it follows do: a
// 3D view (the viewport, a render plugin's own view) and a flat one (the curve editor,
// the dope sheet, the UV view, the Material editor), which has nothing to orbit.
enum class NavigationSpace { View3D, Editor2D };

struct NavigationBindings {
    core::MouseBinding orbit;
    core::MouseBinding pan;
    core::MouseBinding zoom;
};

struct NavigationScheme {
    NavigationBindings view3d;
    NavigationBindings editor2d;  // its orbit is never used
    const NavigationBindings& bindings(NavigationSpace space) const {
        return space == NavigationSpace::View3D ? view3d : editor2d;
    }
};

// What applies whatever the scheme: Settings > Navigation > Zoom and Trackpad.
struct NavigationOptions {
    double zoomSpeed = 1.0;          // how many steps a wheel notch is
    bool invertZoom = false;         // the wheel and a zoom drag go the other way
    bool zoomToCursor = false;       // 3D views: the wheel zooms towards what the cursor is on
    bool trackpadNavigates = false;  // a two-finger scroll pans and orbits; else it zooms
};

// The scheme named when the bindings are no preset's.
inline constexpr const char* kCustomNavigationScheme = "Custom";
// The presets ("usdcc", "Maya", "Blender", "3ds Max"), then "Custom".
USDCC_UI_API const std::vector<std::string>& navigationSchemeNames();
// A preset's bindings; nothing for "Custom" or an unknown name.
USDCC_UI_API std::optional<NavigationScheme> navigationPreset(std::string_view name);

// Defines the Navigation settings in `settings`, once: the scheme, its five bindings and
// the options. The bindings are the ones in use: choosing a preset writes its bindings to
// them, and changing one names the scheme after the preset they now are, or "Custom".
// navigationScheme() and navigationOptions() call it, so only the app's start does, to
// have the page in the dialog before a view exists.
USDCC_UI_API void defineNavigationSettings(core::Settings& settings = core::Settings::instance());
USDCC_UI_API NavigationScheme navigationScheme(core::Settings& settings = core::Settings::instance());
USDCC_UI_API NavigationOptions navigationOptions(core::Settings& settings = core::Settings::instance());

// A press as a binding: the modifiers that count are Ctrl, Shift, Alt and Meta.
USDCC_UI_API core::MouseBinding mouseBinding(Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
// A binding as the system writes it: "Alt+Left", and "⌥ Left" on macOS. "" for none.
USDCC_UI_API QString mouseBindingLabel(const core::MouseBinding& binding);
// Why a navigation binding is refused, "" if it isn't: the left button without Alt is the
// tools' and the selection's, and the right button alone opens menus.
USDCC_UI_API std::string navigationBindingProblem(const core::MouseBinding& binding);

// What a press does in `bindings`. The binding with exactly these modifiers; failing
// that, the middle button's binding that has none, so that it keeps working with a key
// held (Alt+Middle pans in the usdcc scheme as Middle does).
USDCC_UI_API NavigationAction navigationAction(const NavigationBindings& bindings, Qt::MouseButton button,
                                               Qt::KeyboardModifiers modifiers);

// One zoom step is a wheel notch: everything is this much closer, or its range this much
// smaller. The same in every view.
inline constexpr double kNavigationZoomPerStep = 0.9;
// How much of the distance, or of the range shown, is left after `steps`: below 1 zooms in.
USDCC_UI_API double navigationZoomFactor(double steps);
// A zoom drag this many pixels to the right or up is one step in.
inline constexpr double kNavigationZoomDragPixels = 24.0;
// A trackpad scroll this many pixels, where it zooms, is one step.
inline constexpr double kNavigationScrollPixels = 40.0;

// A piece of navigation for a view to carry out.
struct NavigationStep {
    NavigationAction action = NavigationAction::None;
    bool drag = false;  // from a button drag; else from the wheel, a scroll or a pinch
    // Orbit and Pan: how far the cursor moved, in pixels, right and down. What is under
    // it follows it in a pan.
    QPointF delta;
    // Zoom: the steps, positive in. With the speed and the direction of the settings.
    double zoom = 0.0;
    // Zoom, for a view whose two axes zoom apart (the curve editor): the steps across and
    // up. The wheel and a pinch give both the same; a drag gives its horizontal part to
    // the first and its vertical part to the second.
    QPointF zoomAxes;
    // Zoom: what stays where it is. The cursor for the wheel and a pinch; where the drag
    // began.
    QPointF anchor;
    // The wheel's, for a view that reads them (Ctrl: time only in the curve editor). None
    // for a drag, whose modifiers are its binding's, and for a trackpad's Ctrl+scroll.
    Qt::KeyboardModifiers modifiers;

    explicit operator bool() const { return action != NavigationAction::None; }
};

// One view's navigation: which drag is going on, and its events as steps.
class USDCC_UI_API NavigationInput {
public:
    explicit NavigationInput(NavigationSpace space, core::Settings& settings = core::Settings::instance());

    NavigationSpace space() const { return m_space; }

    // A button went down: the drag it begins by the scheme, None if it's no navigation
    // (and the view's to use), or while another drag goes on.
    NavigationAction press(Qt::MouseButton button, Qt::KeyboardModifiers modifiers, const QPointF& position);
    NavigationAction press(const QMouseEvent* event);
    // Begins a drag the scheme doesn't name: a view's own, such as the left drag on
    // nothing that orbits a view without tools.
    void begin(NavigationAction action, Qt::MouseButton button, const QPointF& position);
    // The drag going on.
    NavigationAction dragging() const { return m_dragging; }
    // The cursor moved during a drag: the step since the last move. None without a drag.
    NavigationStep move(const QPointF& position);
    NavigationStep move(const QMouseEvent* event);
    // The cursor was put somewhere else (wrapped at the screen's edge): the next move
    // counts from there.
    void setPosition(const QPointF& position) { m_previous = position; }
    // A button went up: whether that ended the drag.
    bool release(Qt::MouseButton button);
    bool release(const QMouseEvent* event);
    // Ends the drag without a release, as when the view loses the mouse.
    void cancel() { m_dragging = NavigationAction::None; }
    // Whether the last drag, going on or ended, moved further than a click does. A view
    // that opens the right button's menu itself, on the release, opens it only if not.
    bool moved() const { return m_moved; }
    // For a view that gets QEvent::ContextMenu, in its event(): takes the event, so that no
    // menu opens, when it is the mouse's and the right button's last press began a drag.
    // It comes with the press on some systems and with the release on others, so a click
    // with a binding's keys opens none either. True if it was taken.
    bool takeContextMenu(QEvent* event) const;

    // A wheel event: a zoom step, or with Settings > Navigation > Trackpad a trackpad's
    // scroll as an orbit, pan or zoom. None for an event that says nothing.
    NavigationStep wheel(const QWheelEvent* event);
    NavigationStep wheel(QPoint angleDelta, QPoint pixelDelta, Qt::ScrollPhase phase, Qt::KeyboardModifiers modifiers,
                         const QPointF& position);
    // A pinch as a zoom step about the cursor; None for any other gesture.
    NavigationStep gesture(const QNativeGestureEvent* event);
    NavigationStep pinch(double value, const QPointF& position);
    // event()'s QEvent::NativeGesture as gesture(); None for any other event.
    NavigationStep gesture(const QEvent* event);

private:
    NavigationSpace m_space;
    core::Settings* m_settings;
    NavigationAction m_dragging = NavigationAction::None;
    Qt::MouseButton m_button = Qt::NoButton;
    QPointF m_press;
    QPointF m_previous;
    bool m_moved = false;
    bool m_rightPressNavigates = false;  // the right button's last press began a drag
};

// Whether a key press is the one that frames: Shortcuts > Fit to view (F), which the
// viewport has as an action and the other views read here.
USDCC_UI_API bool isFrameKey(const QKeyEvent* event, const core::Settings& settings = core::Settings::instance());
// For a view's event(): takes the frame key's QEvent::ShortcutOverride, so that the key
// press reaches the view with the focus and not the viewport's application-wide action.
// True if it was that.
USDCC_UI_API bool claimFrameKey(QEvent* event, const core::Settings& settings = core::Settings::instance());

}  // namespace usdcc::ui
