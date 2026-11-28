#include "usdcc/ui/navigation.h"

#include "usdcc/ui/app_settings.h"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QStringList>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace usdcc::ui {

namespace {

using core::MouseBinding;
using core::MouseButton;
using core::SettingDefinition;
using core::Settings;
using core::SettingType;
using core::SettingValue;

constexpr const char* kCategory = "Navigation";
constexpr const char* kSchemeKey = "Navigation/scheme";
constexpr const char* kOrbit3dKey = "Navigation/view3d.orbit";
constexpr const char* kPan3dKey = "Navigation/view3d.pan";
constexpr const char* kZoom3dKey = "Navigation/view3d.zoom";
constexpr const char* kPan2dKey = "Navigation/editor2d.pan";
constexpr const char* kZoom2dKey = "Navigation/editor2d.zoom";
constexpr const char* kZoomSpeedKey = "Navigation/zoomSpeed";
constexpr const char* kInvertZoomKey = "Navigation/invertZoom";
constexpr const char* kZoomToCursorKey = "Navigation/zoomToCursor";
constexpr const char* kTrackpadKey = "Navigation/trackpadScroll";
constexpr const char* kTrackpadZooms = "Zoom";
constexpr const char* kTrackpadNavigates = "Pan and orbit";
// The viewport's Fit to View (ViewportViewPanel::addFitToViewAction()).
constexpr const char* kFrameKey = "Shortcuts/viewport.fitToView";

// A drag has moved once the cursor is this far from where it went down.
constexpr double kClickPixels = 3.0;

MouseBinding binding(const char* text) { return core::parseMouseBinding(text).value_or(MouseBinding{}); }

struct Preset {
    const char* name;
    const char* orbit3d;
    const char* pan3d;
    const char* zoom3d;
    const char* pan2d;
    const char* zoom2d;
};

// docs/NAVIGATION.md §3. usdcc's is first: the default.
constexpr std::array<Preset, 4> kPresets = {{
    {"usdcc", "Alt+Left", "Middle", "Alt+Right", "Middle", "Alt+Right"},
    {"Maya", "Alt+Left", "Alt+Middle", "Alt+Right", "Alt+Middle", "Alt+Right"},
    {"Blender", "Middle", "Shift+Middle", "Ctrl+Middle", "Middle", "Ctrl+Middle"},
    {"3ds Max", "Alt+Middle", "Middle", "Ctrl+Alt+Middle", "Middle", "Ctrl+Alt+Middle"},
}};

NavigationScheme schemeOf(const Preset& preset) {
    NavigationScheme scheme;
    scheme.view3d = {binding(preset.orbit3d), binding(preset.pan3d), binding(preset.zoom3d)};
    scheme.editor2d = {MouseBinding{}, binding(preset.pan2d), binding(preset.zoom2d)};
    return scheme;
}

bool sameBindings(const NavigationScheme& a, const NavigationScheme& b) {
    return a.view3d.orbit == b.view3d.orbit && a.view3d.pan == b.view3d.pan && a.view3d.zoom == b.view3d.zoom &&
           a.editor2d.pan == b.editor2d.pan && a.editor2d.zoom == b.editor2d.zoom;
}

MouseBinding storedBinding(const Settings& settings, const char* key) {
    return core::parseMouseBinding(settings.get<std::string>(key)).value_or(MouseBinding{});
}

// The bindings as the settings have them, defined or not.
NavigationScheme storedScheme(const Settings& settings) {
    NavigationScheme scheme;
    scheme.view3d = {storedBinding(settings, kOrbit3dKey), storedBinding(settings, kPan3dKey),
                     storedBinding(settings, kZoom3dKey)};
    scheme.editor2d = {MouseBinding{}, storedBinding(settings, kPan2dKey), storedBinding(settings, kZoom2dKey)};
    return scheme;
}

bool isBindingKey(const std::string& key) {
    return key == kOrbit3dKey || key == kPan3dKey || key == kZoom3dKey || key == kPan2dKey || key == kZoom2dKey;
}

// Keeps the scheme's name and the five bindings in step, for one registry.
struct SchemeSync {
    core::Signal<const std::string&>::Connection connection;
    bool applying = false;  // the changes are this object's own
};

std::map<const Settings*, SchemeSync>& schemeSyncs() {
    static std::map<const Settings*, SchemeSync> syncs;
    return syncs;
}

void writeScheme(Settings& settings, const NavigationScheme& scheme) {
    settings.setValue(kOrbit3dKey, core::mouseBindingText(scheme.view3d.orbit));
    settings.setValue(kPan3dKey, core::mouseBindingText(scheme.view3d.pan));
    settings.setValue(kZoom3dKey, core::mouseBindingText(scheme.view3d.zoom));
    settings.setValue(kPan2dKey, core::mouseBindingText(scheme.editor2d.pan));
    settings.setValue(kZoom2dKey, core::mouseBindingText(scheme.editor2d.zoom));
}

void syncScheme(Settings& settings, SchemeSync& sync, const std::string& key) {
    if (sync.applying) return;
    if (key == kSchemeKey) {
        // A preset was chosen: the bindings become its own. "Custom" keeps them.
        if (const auto preset = navigationPreset(settings.get<std::string>(kSchemeKey))) {
            sync.applying = true;
            writeScheme(settings, *preset);
            sync.applying = false;
        }
    } else if (isBindingKey(key)) {
        // A binding was changed: the scheme is the preset they now are, or none.
        const NavigationScheme now = storedScheme(settings);
        std::string name = kCustomNavigationScheme;
        for (const Preset& preset : kPresets) {
            if (sameBindings(now, schemeOf(preset))) {
                name = preset.name;
                break;
            }
        }
        sync.applying = true;
        settings.setValue(kSchemeKey, name);
        sync.applying = false;
    }
}

std::string refusesBinding(const SettingValue& value) {
    const std::string* text = std::get_if<std::string>(&value);
    const auto parsed = text ? core::parseMouseBinding(*text) : std::nullopt;
    return parsed ? navigationBindingProblem(*parsed) : std::string();
}

QString tr(const char* text) { return QCoreApplication::translate("usdcc::ui::Navigation", text); }

}  // namespace

const std::vector<std::string>& navigationSchemeNames() {
    static const std::vector<std::string> names = []() {
        std::vector<std::string> result;
        for (const Preset& preset : kPresets) result.emplace_back(preset.name);
        result.emplace_back(kCustomNavigationScheme);
        return result;
    }();
    return names;
}

std::optional<NavigationScheme> navigationPreset(std::string_view name) {
    for (const Preset& preset : kPresets) {
        if (name == preset.name) return schemeOf(preset);
    }
    return std::nullopt;
}

void defineNavigationSettings(Settings& settings) {
    if (settings.contains(kSchemeKey)) return;
    const Preset& standard = kPresets.front();

    settings.define({.key = kSchemeKey,
                     .category = kCategory,
                     .label = "Scheme",
                     .description = "Which mouse buttons orbit, pan and zoom, in every view: usdcc's own, or as in "
                                    "another application. Choosing one sets the bindings below; changing a binding "
                                    "makes the scheme Custom. The wheel always zooms.",
                     .type = SettingType::Choice,
                     .defaultValue = std::string(standard.name),
                     .choices = navigationSchemeNames()});

    const auto bindingSetting = [&settings](const char* key, const char* group, const char* label, const char* text,
                                            const char* description) {
        settings.define({.key = key,
                         .category = kCategory,
                         .group = group,
                         .label = label,
                         .description = description,
                         .type = SettingType::MouseBinding,
                         .defaultValue = std::string(text),
                         .refuses = refusesBinding});
    };
    const char* const view3d = "3D views";
    bindingSetting(kOrbit3dKey, view3d, "Orbit", standard.orbit3d,
                   "The viewport and a render plugin's own view. A drag with this button and these keys turns the "
                   "camera about what it looks at.");
    bindingSetting(kPan3dKey, view3d, "Pan", standard.pan3d,
                   "Moves the camera sideways: what the cursor is on stays under it. A middle button bound without "
                   "keys also answers with keys held that no other binding uses.");
    bindingSetting(kZoom3dKey, view3d, "Zoom", standard.zoom3d,
                   "Dragging right or up moves the camera closer, left or down away.");
    const char* const editor2d = "2D editors";
    bindingSetting(kPan2dKey, editor2d, "Pan", standard.pan2d,
                   "The curve editor, the dope sheet, the UV view and the Material editor.");
    bindingSetting(kZoom2dKey, editor2d, "Zoom", standard.zoom2d,
                   "Zooms about where the drag began: right or up in, left or down out. In the curve editor "
                   "sideways zooms time and up and down the values.");

    const char* const zoom = "Zoom";
    settings.define({.key = kZoomSpeedKey,
                     .category = kCategory,
                     .group = zoom,
                     .label = "Zoom speed",
                     .description = "How far a wheel step and a zoom drag go, in every view. At 1 a wheel step is a "
                                    "tenth closer.",
                     .type = SettingType::Float,
                     .defaultValue = 1.0,
                     .minimum = 0.1,
                     .maximum = 5.0});
    settings.define({.key = kInvertZoomKey,
                     .category = kCategory,
                     .group = zoom,
                     .label = "Invert zoom",
                     .description = "The wheel and a zoom drag go the other way.",
                     .type = SettingType::Bool,
                     .defaultValue = false});
    settings.define({.key = kZoomToCursorKey,
                     .category = kCategory,
                     .group = zoom,
                     .label = "Zoom to cursor in 3D views",
                     .description = "The wheel moves the camera towards what the cursor is on, not towards the "
                                    "middle of the view. The 2D editors always zoom about the cursor.",
                     .type = SettingType::Bool,
                     .defaultValue = false});
    settings.define({.key = kTrackpadKey,
                     .category = kCategory,
                     .group = "Trackpad",
                     .label = "Two-finger scroll",
                     .description = "Zoom: as a wheel. Pan and orbit: it orbits a 3D view, pans it with Shift and "
                                    "zooms it with Ctrl (Cmd on macOS); it pans a 2D editor and zooms it with Ctrl. "
                                    "A pinch always zooms. Windows doesn't tell a trackpad from a wheel, so there it "
                                    "always zooms.",
                     .type = SettingType::Choice,
                     .defaultValue = std::string(kTrackpadZooms),
                     .choices = {kTrackpadZooms, kTrackpadNavigates}});

    SchemeSync& sync = schemeSyncs()[&settings];
    Settings* registry = &settings;
    sync.connection = settings.changed.connect([registry](const std::string& key) {
        syncScheme(*registry, schemeSyncs()[registry], key);
    });
    // A preset saved by name gets the bindings it has now, should a release change them.
    syncScheme(settings, sync, kSchemeKey);
}

NavigationScheme navigationScheme(Settings& settings) {
    defineNavigationSettings(settings);
    return storedScheme(settings);
}

NavigationOptions navigationOptions(Settings& settings) {
    defineNavigationSettings(settings);
    NavigationOptions options;
    options.zoomSpeed = settings.get<double>(kZoomSpeedKey, 1.0);
    options.invertZoom = settings.get<bool>(kInvertZoomKey);
    options.zoomToCursor = settings.get<bool>(kZoomToCursorKey);
    options.trackpadNavigates = settings.get<std::string>(kTrackpadKey) == kTrackpadNavigates;
    return options;
}

MouseBinding mouseBinding(Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
    MouseBinding result;
    switch (button) {
    case Qt::LeftButton: result.button = MouseButton::Left; break;
    case Qt::MiddleButton: result.button = MouseButton::Middle; break;
    case Qt::RightButton: result.button = MouseButton::Right; break;
    default: return result;
    }
    result.ctrl = modifiers.testFlag(Qt::ControlModifier);
    result.shift = modifiers.testFlag(Qt::ShiftModifier);
    result.alt = modifiers.testFlag(Qt::AltModifier);
    result.meta = modifiers.testFlag(Qt::MetaModifier);
    return result;
}

QString mouseBindingLabel(const MouseBinding& binding) {
    if (!binding.isSet()) return {};
    QString button;
    switch (binding.button) {
    case MouseButton::Left: button = tr("Left"); break;
    case MouseButton::Middle: button = tr("Middle"); break;
    case MouseButton::Right: button = tr("Right"); break;
    case MouseButton::None: break;
    }
#ifdef Q_OS_MACOS
    // Qt's Ctrl is the Command key there, and its Meta the Control key.
    QString keys;
    if (binding.meta) keys += QStringLiteral("⌃");
    if (binding.alt) keys += QStringLiteral("⌥");
    if (binding.shift) keys += QStringLiteral("⇧");
    if (binding.ctrl) keys += QStringLiteral("⌘");
    return keys.isEmpty() ? button : keys + QLatin1Char(' ') + button;
#else
    QStringList parts;
    if (binding.ctrl) parts << QStringLiteral("Ctrl");
    if (binding.shift) parts << QStringLiteral("Shift");
    if (binding.alt) parts << QStringLiteral("Alt");
    if (binding.meta) parts << QStringLiteral("Meta");
    parts << button;
    return parts.join(QLatin1Char('+'));
#endif
}

std::string navigationBindingProblem(const MouseBinding& binding) {
    if (binding.button == MouseButton::Left && !binding.alt) {
        return "A left drag without Alt is the tools' and the selection's: add Alt.";
    }
    if (binding.button == MouseButton::Right && !binding.hasModifiers()) {
        return "The right button alone opens menus: add a key.";
    }
    return {};
}

NavigationAction navigationAction(const NavigationBindings& bindings, Qt::MouseButton button,
                                  Qt::KeyboardModifiers modifiers) {
    const MouseBinding pressed = mouseBinding(button, modifiers);
    if (!pressed.isSet()) return NavigationAction::None;
    const std::array<std::pair<const MouseBinding*, NavigationAction>, 3> all = {{
        {&bindings.orbit, NavigationAction::Orbit},
        {&bindings.pan, NavigationAction::Pan},
        {&bindings.zoom, NavigationAction::Zoom},
    }};
    for (const auto& [bound, action] : all) {
        if (*bound == pressed) return action;
    }
    if (pressed.button == MouseButton::Middle) {
        for (const auto& [bound, action] : all) {
            if (bound->button == MouseButton::Middle && !bound->hasModifiers()) return action;
        }
    }
    return NavigationAction::None;
}

double navigationZoomFactor(double steps) {
    return std::pow(kNavigationZoomPerStep, std::clamp(steps, -100.0, 100.0));
}

NavigationInput::NavigationInput(NavigationSpace space, Settings& settings) : m_space(space), m_settings(&settings) {}

NavigationAction NavigationInput::press(Qt::MouseButton button, Qt::KeyboardModifiers modifiers,
                                        const QPointF& position) {
    if (m_dragging != NavigationAction::None) return NavigationAction::None;
    if (button == Qt::RightButton) m_rightPressNavigates = false;
    NavigationAction action = navigationAction(navigationScheme(*m_settings).bindings(m_space), button, modifiers);
    if (action == NavigationAction::Orbit && m_space == NavigationSpace::Editor2D) action = NavigationAction::None;
    if (action != NavigationAction::None) begin(action, button, position);
    return action;
}

NavigationAction NavigationInput::press(const QMouseEvent* event) {
    return press(event->button(), event->modifiers(), event->position());
}

void NavigationInput::begin(NavigationAction action, Qt::MouseButton button, const QPointF& position) {
    m_dragging = action;
    m_button = button;
    m_press = m_previous = position;
    m_moved = false;
    if (button == Qt::RightButton) m_rightPressNavigates = true;
}

NavigationStep NavigationInput::move(const QPointF& position) {
    NavigationStep step;
    if (m_dragging == NavigationAction::None) return step;
    step.action = m_dragging;
    step.drag = true;
    step.delta = position - m_previous;
    step.anchor = m_press;
    m_previous = position;
    if ((position - m_press).manhattanLength() > kClickPixels) m_moved = true;
    if (m_dragging == NavigationAction::Zoom) {
        const NavigationOptions options = navigationOptions(*m_settings);
        const double scale = options.zoomSpeed * (options.invertZoom ? -1.0 : 1.0) / kNavigationZoomDragPixels;
        step.zoomAxes = QPointF(step.delta.x() * scale, -step.delta.y() * scale);
        step.zoom = step.zoomAxes.x() + step.zoomAxes.y();
    }
    return step;
}

NavigationStep NavigationInput::move(const QMouseEvent* event) { return move(event->position()); }

bool NavigationInput::release(Qt::MouseButton button) {
    if (m_dragging == NavigationAction::None || button != m_button) return false;
    m_dragging = NavigationAction::None;
    return true;
}

bool NavigationInput::release(const QMouseEvent* event) { return release(event->button()); }

bool NavigationInput::takeContextMenu(QEvent* event) const {
    if (event->type() != QEvent::ContextMenu || !m_rightPressNavigates) return false;
    if (static_cast<QContextMenuEvent*>(event)->reason() != QContextMenuEvent::Mouse) return false;
    event->accept();
    return true;
}

NavigationStep NavigationInput::wheel(const QWheelEvent* event) {
    return wheel(event->angleDelta(), event->pixelDelta(), event->phase(), event->modifiers(), event->position());
}

NavigationStep NavigationInput::wheel(QPoint angleDelta, QPoint pixelDelta, Qt::ScrollPhase phase,
                                      Qt::KeyboardModifiers modifiers, const QPointF& position) {
    NavigationStep step;
    const NavigationOptions options = navigationOptions(*m_settings);
    const double direction = options.zoomSpeed * (options.invertZoom ? -1.0 : 1.0);
    // A wheel turns in notches and says nothing else; a trackpad scrolls in pixels, in phases.
    const bool trackpad = phase != Qt::NoScrollPhase || !pixelDelta.isNull();
    step.anchor = position;

    if (trackpad && options.trackpadNavigates) {
        const QPointF pixels = pixelDelta.isNull() ? QPointF(angleDelta) * (kNavigationScrollPixels / 120.0)
                                                   : QPointF(pixelDelta);
        if (pixels.isNull()) return step;
        if (modifiers.testFlag(Qt::ControlModifier)) {
            step.action = NavigationAction::Zoom;
            step.zoom = pixels.y() / kNavigationScrollPixels * direction;
            step.zoomAxes = QPointF(step.zoom, step.zoom);
        } else if (m_space == NavigationSpace::View3D && !modifiers.testFlag(Qt::ShiftModifier)) {
            step.action = NavigationAction::Orbit;
            step.delta = pixels;
        } else {
            step.action = NavigationAction::Pan;
            step.delta = pixels;
        }
        return step;
    }

    double turned = angleDelta.y();
    // Shift or Alt held turns the wheel sideways on some systems.
    if (turned == 0.0 && (modifiers & (Qt::ShiftModifier | Qt::AltModifier))) turned = angleDelta.x();
    double steps = turned / 120.0;
    if (angleDelta.isNull()) steps = pixelDelta.y() / kNavigationScrollPixels;
    if (steps == 0.0) return step;
    step.action = NavigationAction::Zoom;
    step.zoom = steps * direction;
    step.zoomAxes = QPointF(step.zoom, step.zoom);
    step.modifiers = modifiers & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    return step;
}

NavigationStep NavigationInput::gesture(const QNativeGestureEvent* event) {
    if (event->gestureType() != Qt::ZoomNativeGesture) return {};
    return pinch(event->value(), event->position());
}

NavigationStep NavigationInput::gesture(const QEvent* event) {
    if (event->type() != QEvent::NativeGesture) return {};
    return gesture(static_cast<const QNativeGestureEvent*>(event));
}

NavigationStep NavigationInput::pinch(double value, const QPointF& position) {
    NavigationStep step;
    // The fingers moved apart by `value` of their distance: everything is that much bigger.
    if (!std::isfinite(value) || value <= -0.9 || value == 0.0) return step;
    step.action = NavigationAction::Zoom;
    step.zoom = std::log1p(value) / -std::log(kNavigationZoomPerStep);
    step.zoomAxes = QPointF(step.zoom, step.zoom);
    step.anchor = position;
    return step;
}

bool isFrameKey(const QKeyEvent* event, const Settings& settings) {
    const QKeySequence frame = settings.contains(kFrameKey) ? settingShortcut(kFrameKey, settings)
                                                             : QKeySequence(Qt::Key_F);
    if (frame.isEmpty()) return false;
    const Qt::KeyboardModifiers modifiers =
        event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    return QKeySequence(QKeyCombination(modifiers, static_cast<Qt::Key>(event->key()))) == frame;
}

bool claimFrameKey(QEvent* event, const Settings& settings) {
    if (event->type() != QEvent::ShortcutOverride || !isFrameKey(static_cast<QKeyEvent*>(event), settings)) return false;
    event->accept();
    return true;
}

}  // namespace usdcc::ui
