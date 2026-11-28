// Navigation (docs/NAVIGATION.md): the schemes and what a press does in them, the
// Navigation settings keeping the scheme's name and its bindings in step, the steps
// NavigationInput makes of drags, the wheel, a trackpad and a pinch, the dialog's editor for
// a mouse binding, and the curve editor and dope sheet going by all of it. Runs in
// usdcc_ui_tests (offscreen); settings stay in memory (test_settings.h).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/core/mouse_binding.h"
#include "usdcc/core/settings.h"
#include "usdcc/ui/navigation.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/settings_dialog.h"
#include "usdcc/usd/curve_editor_view_panel.h"
#include "usdcc/usd/dope_sheet_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/time_ruler.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/xformOp.h>

#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QAction>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include <cmath>
#include <memory>
#include <string>

using namespace usdcc::ui;
using usdcc::core::MouseBinding;
using usdcc::core::MouseButton;
using usdcc::core::Settings;

namespace {

void settle() {
    for (int i = 0; i < 8; ++i) QApplication::processEvents();
}

bool isNear(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) < tolerance; }

struct Scale {
    QTemporaryDir folder;
    UiScaleController controller{*static_cast<QApplication*>(QCoreApplication::instance()),
                                 folder.filePath(QStringLiteral("ui.ini"))};
};

void mouse(QWidget* widget, QEvent::Type type, const QPointF& position, Qt::MouseButton button, Qt::MouseButtons buttons,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, position, widget->mapToGlobal(position), button, buttons, modifiers);
    QApplication::sendEvent(widget, &event);
}

void drag(QWidget* widget, const QPointF& from, const QPointF& to, Qt::MouseButton button,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    mouse(widget, QEvent::MouseButtonPress, from, button, button, modifiers);
    mouse(widget, QEvent::MouseMove, (from + to) / 2.0, Qt::NoButton, button, modifiers);
    mouse(widget, QEvent::MouseMove, to, Qt::NoButton, button, modifiers);
    mouse(widget, QEvent::MouseButtonRelease, to, button, Qt::NoButton, modifiers);
}

void wheel(QWidget* widget, const QPointF& position, int notches, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWheelEvent event(position, widget->mapToGlobal(position), QPoint(), QPoint(0, 120 * notches), Qt::NoButton, modifiers,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(widget, &event);
}

template <typename T>
T* withKey(QWidget* root, const QString& key, const QString& objectName = {}) {
    for (T* widget : root->findChildren<T*>()) {
        if (widget->property("settingKey").toString() == key &&
            (objectName.isEmpty() || widget->objectName() == objectName)) {
            return widget;
        }
    }
    return nullptr;
}

std::string text(const Settings& settings, const char* key) { return settings.get<std::string>(key); }

}  // namespace

USDCC_TEST(mouse_binding_text) {
    using usdcc::core::mouseBindingText;
    using usdcc::core::parseMouseBinding;
    const auto alt = parseMouseBinding("Alt+Left");
    CHECK(alt && alt->button == MouseButton::Left && alt->alt && !alt->ctrl && !alt->shift && !alt->meta, "Alt+Left parses");
    CHECK(mouseBindingText(*parseMouseBinding(" alt + SHIFT+ctrl+middle ")) == "Ctrl+Shift+Alt+Middle",
          "any case and order, written in one");
    CHECK(parseMouseBinding("") && !parseMouseBinding("")->isSet() && mouseBindingText({}).empty(),
          "nothing is the binding to nothing");
    CHECK(!parseMouseBinding("Alt") && !parseMouseBinding("Alt+Left+Right") && !parseMouseBinding("Hyper+Left") &&
              !parseMouseBinding("Left+"),
          "modifiers alone, two buttons and unknown parts aren't bindings");
    CHECK(mouseBinding(Qt::RightButton, Qt::AltModifier | Qt::KeypadModifier) == *parseMouseBinding("Alt+Right"),
          "a press as a binding: only the four keys count");
    CHECK(!mouseBinding(Qt::BackButton, Qt::NoModifier).isSet(), "and only the three buttons");
}

USDCC_TEST(navigation_schemes) {
    CHECK(navigationSchemeNames() == std::vector<std::string>({"usdcc", "Maya", "Blender", "3ds Max", "Custom"}),
          "the presets, then Custom");
    CHECK(!navigationPreset("Custom") && !navigationPreset("Houdini"), "Custom and unknown names are no preset");

    const NavigationScheme own = *navigationPreset("usdcc");
    CHECK(navigationAction(own.view3d, Qt::LeftButton, Qt::AltModifier) == NavigationAction::Orbit &&
              navigationAction(own.view3d, Qt::MiddleButton, Qt::NoModifier) == NavigationAction::Pan &&
              navigationAction(own.view3d, Qt::RightButton, Qt::AltModifier) == NavigationAction::Zoom,
          "usdcc: Alt+Left orbits, Middle pans, Alt+Right zooms");
    CHECK(navigationAction(own.view3d, Qt::LeftButton, Qt::NoModifier) == NavigationAction::None &&
              navigationAction(own.view3d, Qt::RightButton, Qt::NoModifier) == NavigationAction::None &&
              navigationAction(own.view3d, Qt::LeftButton, Qt::AltModifier | Qt::ShiftModifier) == NavigationAction::None,
          "a left drag, a right click and other keys are the view's");
    CHECK(navigationAction(own.view3d, Qt::MiddleButton, Qt::AltModifier) == NavigationAction::Pan,
          "the middle button bound without keys answers with a key held");

    const NavigationScheme maya = *navigationPreset("Maya");
    CHECK(navigationAction(maya.view3d, Qt::MiddleButton, Qt::AltModifier) == NavigationAction::Pan &&
              navigationAction(maya.view3d, Qt::MiddleButton, Qt::NoModifier) == NavigationAction::None &&
              navigationAction(maya.editor2d, Qt::MiddleButton, Qt::AltModifier) == NavigationAction::Pan,
          "Maya: Alt+Middle pans, the middle button alone doesn't");

    const NavigationScheme blender = *navigationPreset("Blender");
    CHECK(navigationAction(blender.view3d, Qt::MiddleButton, Qt::NoModifier) == NavigationAction::Orbit &&
              navigationAction(blender.view3d, Qt::MiddleButton, Qt::ShiftModifier) == NavigationAction::Pan &&
              navigationAction(blender.view3d, Qt::MiddleButton, Qt::ControlModifier) == NavigationAction::Zoom &&
              navigationAction(blender.view3d, Qt::LeftButton, Qt::AltModifier) == NavigationAction::None,
          "Blender: Middle orbits, with Shift pans, with Ctrl zooms");
    CHECK(navigationAction(blender.editor2d, Qt::MiddleButton, Qt::NoModifier) == NavigationAction::Pan &&
              navigationAction(blender.editor2d, Qt::MiddleButton, Qt::ControlModifier) == NavigationAction::Zoom,
          "and in a 2D editor pans, with Ctrl zooms");

    const NavigationScheme max = *navigationPreset("3ds Max");
    CHECK(navigationAction(max.view3d, Qt::MiddleButton, Qt::AltModifier) == NavigationAction::Orbit &&
              navigationAction(max.view3d, Qt::MiddleButton, Qt::NoModifier) == NavigationAction::Pan &&
              navigationAction(max.view3d, Qt::MiddleButton, Qt::ControlModifier | Qt::AltModifier) == NavigationAction::Zoom,
          "3ds Max: an exact binding comes before the bare middle button's");

    for (const std::string& name : navigationSchemeNames()) {
        const auto preset = navigationPreset(name);
        if (!preset) continue;
        for (const MouseBinding* binding : {&preset->view3d.orbit, &preset->view3d.pan, &preset->view3d.zoom,
                                            &preset->editor2d.pan, &preset->editor2d.zoom}) {
            CHECK(navigationBindingProblem(*binding).empty(), "%s binds nothing that is refused", name.c_str());
        }
    }
    CHECK(!navigationBindingProblem(*usdcc::core::parseMouseBinding("Left")).empty() &&
              !navigationBindingProblem(*usdcc::core::parseMouseBinding("Shift+Left")).empty() &&
              navigationBindingProblem(*usdcc::core::parseMouseBinding("Alt+Shift+Left")).empty(),
          "the left button needs Alt");
    CHECK(!navigationBindingProblem(*usdcc::core::parseMouseBinding("Right")).empty() &&
              navigationBindingProblem(*usdcc::core::parseMouseBinding("Shift+Right")).empty() &&
              navigationBindingProblem({}).empty(),
          "the right button a key; nothing is fine");
    CHECK(isNear(navigationZoomFactor(1.0), 0.9) && isNear(navigationZoomFactor(-1.0), 1.0 / 0.9) &&
              isNear(navigationZoomFactor(0.0), 1.0),
          "a step in is nine tenths");
}

USDCC_TEST(navigation_settings_follow_the_scheme) {
    Settings settings;
    defineNavigationSettings(settings);
    CHECK(text(settings, "Navigation/scheme") == "usdcc" && text(settings, "Navigation/view3d.orbit") == "Alt+Left" &&
              text(settings, "Navigation/view3d.pan") == "Middle" && text(settings, "Navigation/editor2d.zoom") == "Alt+Right",
          "usdcc's own to start with");
    CHECK(settings.definition("Navigation/view3d.orbit")->type == usdcc::core::SettingType::MouseBinding,
          "the bindings are mouse bindings");

    CHECK(settings.setValue("Navigation/scheme", std::string("Blender")), "choosing a preset");
    CHECK(text(settings, "Navigation/view3d.orbit") == "Middle" && text(settings, "Navigation/view3d.pan") == "Shift+Middle" &&
              text(settings, "Navigation/view3d.zoom") == "Ctrl+Middle" && text(settings, "Navigation/editor2d.pan") == "Middle" &&
              text(settings, "Navigation/editor2d.zoom") == "Ctrl+Middle",
          "sets the bindings");
    CHECK(text(settings, "Navigation/scheme") == "Blender", "and stays the scheme");
    const NavigationScheme scheme = navigationScheme(settings);
    CHECK(navigationAction(scheme.view3d, Qt::MiddleButton, Qt::NoModifier) == NavigationAction::Orbit,
          "navigationScheme() is what the bindings say");

    CHECK(settings.setValue("Navigation/view3d.zoom", std::string("alt+right")), "changing a binding");
    CHECK(text(settings, "Navigation/view3d.zoom") == "Alt+Right" && text(settings, "Navigation/scheme") == "Custom",
          "is stored in one form, and the scheme is Custom");
    CHECK(text(settings, "Navigation/view3d.orbit") == "Middle", "the other bindings stay");
    CHECK(settings.setValue("Navigation/view3d.zoom", std::string("Ctrl+Middle")) && text(settings, "Navigation/scheme") == "Blender",
          "back to a preset's bindings is that preset again");

    settings.setValue("Navigation/view3d.pan", std::string(""));
    CHECK(text(settings, "Navigation/scheme") == "Custom" && !navigationScheme(settings).view3d.pan.isSet(), "a binding to nothing");
    CHECK(settings.setValue("Navigation/scheme", std::string("Custom")) && text(settings, "Navigation/view3d.orbit") == "Middle",
          "choosing Custom keeps the bindings");

    CHECK(!settings.setValue("Navigation/view3d.orbit", std::string("Left")) && text(settings, "Navigation/view3d.orbit") == "Middle",
          "a left drag without Alt is refused");
    CHECK(!settings.setValue("Navigation/editor2d.zoom", std::string("Right")), "and the right button alone");
    CHECK(!settings.setValue("Navigation/view3d.orbit", std::string("Alt+Wheel")), "and what's no binding");

    settings.resetCategory("Navigation");
    CHECK(text(settings, "Navigation/scheme") == "usdcc" && text(settings, "Navigation/view3d.orbit") == "Alt+Left" &&
              text(settings, "Navigation/view3d.pan") == "Middle",
          "Reset Page is usdcc's again");

    const NavigationOptions options = navigationOptions(settings);
    CHECK(options.zoomSpeed == 1.0 && !options.invertZoom && !options.zoomToCursor && !options.trackpadNavigates,
          "the options' defaults");

    // A preset saved by name, with bindings saved from another release: the preset's.
    auto backend = std::make_unique<usdcc::core::MemorySettingsBackend>();
    backend->write(*settings.definition("Navigation/scheme"), std::string("Maya"));
    backend->write(*settings.definition("Navigation/view3d.pan"), std::string("Shift+Middle"));
    settings.setBackend(std::move(backend));
    CHECK(text(settings, "Navigation/scheme") == "Maya" && text(settings, "Navigation/view3d.pan") == "Alt+Middle",
          "a saved preset has its bindings as they are now");
}

USDCC_TEST(navigation_input_steps) {
    Settings settings;
    NavigationInput view(NavigationSpace::View3D, settings);
    NavigationInput editor(NavigationSpace::Editor2D, settings);

    CHECK(view.press(Qt::LeftButton, Qt::NoModifier, QPointF(10, 10)) == NavigationAction::None &&
              view.dragging() == NavigationAction::None && !view.move(QPointF(20, 20)),
          "a press the scheme doesn't bind is no drag");
    CHECK(view.press(Qt::LeftButton, Qt::AltModifier, QPointF(100, 100)) == NavigationAction::Orbit, "Alt+Left orbits");
    CHECK(view.press(Qt::MiddleButton, Qt::NoModifier, QPointF(100, 100)) == NavigationAction::None &&
              view.dragging() == NavigationAction::Orbit,
          "another button during it isn't another drag");
    CHECK(!view.moved(), "not moved yet");
    NavigationStep step = view.move(QPointF(101, 102));
    CHECK(step.action == NavigationAction::Orbit && step.drag && step.delta == QPointF(1, 2) && !view.moved(),
          "a nudge is a step, and still a click");
    step = view.move(QPointF(111, 97));
    CHECK(step.delta == QPointF(10, -5) && view.moved(), "each move is the step since the last");
    view.setPosition(QPointF(500, 500));
    CHECK(view.move(QPointF(503, 500)).delta == QPointF(3, 0), "after the cursor was put elsewhere, from there");
    CHECK(!view.release(Qt::MiddleButton) && view.dragging() == NavigationAction::Orbit, "another button's release leaves it");
    CHECK(view.release(Qt::LeftButton) && view.dragging() == NavigationAction::None && view.moved(),
          "its own ends it; it had moved");

    CHECK(editor.press(Qt::LeftButton, Qt::AltModifier, QPointF()) == NavigationAction::None, "nothing orbits in a 2D editor");
    CHECK(editor.press(Qt::RightButton, Qt::AltModifier, QPointF(50, 60)) == NavigationAction::Zoom, "Alt+Right zooms");
    step = editor.move(QPointF(50 + 24, 60 - 48));
    CHECK(step.action == NavigationAction::Zoom && isNear(step.zoomAxes.x(), 1.0) && isNear(step.zoomAxes.y(), 2.0) &&
              isNear(step.zoom, 3.0) && step.anchor == QPointF(50, 60) && step.modifiers == Qt::NoModifier,
          "right and up are in, a step every 24 pixels, about where it began: %g %g", step.zoomAxes.x(), step.zoomAxes.y());
    settings.setValue("Navigation/zoomSpeed", 2.0);
    settings.setValue("Navigation/invertZoom", true);
    step = editor.move(QPointF(50, 60 - 48));
    CHECK(isNear(step.zoomAxes.x(), 2.0) && isNear(step.zoom, 2.0), "the speed and the direction are the settings': %g", step.zoom);
    editor.release(Qt::RightButton);

    // The wheel.
    step = view.wheel(QPoint(0, 120), QPoint(), Qt::NoScrollPhase, Qt::ControlModifier, QPointF(7, 8));
    CHECK(step.action == NavigationAction::Zoom && !step.drag && isNear(step.zoom, -2.0) && step.anchor == QPointF(7, 8) &&
              step.modifiers == Qt::ControlModifier && step.zoomAxes == QPointF(step.zoom, step.zoom),
          "a notch is a step about the cursor, with the wheel's keys: %g", step.zoom);
    settings.reset("Navigation/zoomSpeed");
    settings.reset("Navigation/invertZoom");
    CHECK(isNear(view.wheel(QPoint(0, -60), QPoint(), Qt::NoScrollPhase, Qt::NoModifier, QPointF()).zoom, -0.5),
          "half a notch back is half a step out");
    CHECK(isNear(view.wheel(QPoint(120, 0), QPoint(), Qt::NoScrollPhase, Qt::ShiftModifier, QPointF()).zoom, 1.0),
          "a wheel that Shift turned sideways still zooms");
    CHECK(!view.wheel(QPoint(120, 0), QPoint(), Qt::NoScrollPhase, Qt::NoModifier, QPointF()) &&
              !view.wheel(QPoint(), QPoint(), Qt::NoScrollPhase, Qt::NoModifier, QPointF()),
          "a sideways wheel and an event that says nothing don't");

    // A trackpad: a wheel unless Settings > Navigation > Trackpad says otherwise.
    step = view.wheel(QPoint(0, 60), QPoint(0, 20), Qt::ScrollUpdate, Qt::NoModifier, QPointF());
    CHECK(step.action == NavigationAction::Zoom && isNear(step.zoom, 0.5), "a trackpad's scroll zooms as a wheel");
    settings.setValue("Navigation/trackpadScroll", std::string("Pan and orbit"));
    step = view.wheel(QPoint(40, 60), QPoint(12, 20), Qt::ScrollUpdate, Qt::NoModifier, QPointF());
    CHECK(step.action == NavigationAction::Orbit && step.delta == QPointF(12, 20) && !step.drag, "or orbits a 3D view");
    CHECK(view.wheel(QPoint(), QPoint(12, 20), Qt::ScrollUpdate, Qt::ShiftModifier, QPointF()).action == NavigationAction::Pan,
          "pans it with Shift");
    step = view.wheel(QPoint(), QPoint(0, 80), Qt::ScrollUpdate, Qt::ControlModifier, QPointF());
    CHECK(step.action == NavigationAction::Zoom && isNear(step.zoom, 2.0) && step.modifiers == Qt::NoModifier,
          "and zooms it with Ctrl, which is then no key of the wheel's");
    step = editor.wheel(QPoint(), QPoint(12, 20), Qt::ScrollUpdate, Qt::NoModifier, QPointF());
    CHECK(step.action == NavigationAction::Pan && step.delta == QPointF(12, 20), "it pans a 2D editor");
    CHECK(view.wheel(QPoint(0, 120), QPoint(), Qt::NoScrollPhase, Qt::NoModifier, QPointF()).action == NavigationAction::Zoom,
          "a wheel still zooms");

    // A pinch.
    step = view.pinch(1.0 / 0.9 - 1.0, QPointF(3, 4));
    CHECK(step.action == NavigationAction::Zoom && isNear(step.zoom, 1.0) && step.anchor == QPointF(3, 4),
          "fingers a ninth further apart are a step in: %g", step.zoom);
    CHECK(view.pinch(-0.1, QPointF()).zoom < 0.0 && !view.pinch(0.0, QPointF()), "closer together is out; none is none");

    // The right button's menu: not when its press began a drag, whether the system sends
    // the event with the press or after the release.
    QContextMenuEvent byMouse(QContextMenuEvent::Mouse, QPoint(5, 5), QPoint(5, 5));
    QContextMenuEvent byKey(QContextMenuEvent::Keyboard, QPoint(5, 5), QPoint(5, 5));
    byMouse.ignore();
    NavigationInput fresh(NavigationSpace::Editor2D, settings);
    CHECK(!fresh.takeContextMenu(&byMouse) && !byMouse.isAccepted(), "a menu opens when nothing navigated");
    editor.press(Qt::RightButton, Qt::AltModifier, QPointF(5, 5));
    CHECK(editor.takeContextMenu(&byMouse) && byMouse.isAccepted(), "not with the press that zooms");
    editor.release(Qt::RightButton);
    CHECK(editor.takeContextMenu(&byMouse) && !editor.takeContextMenu(&byKey), "nor after its release; the keyboard's still does");
    editor.press(Qt::RightButton, Qt::NoModifier, QPointF(5, 5));
    CHECK(!editor.takeContextMenu(&byMouse), "the right button alone opens it again");

    // A view's own drag, and one cut short.
    view.begin(NavigationAction::Orbit, Qt::LeftButton, QPointF(1, 1));
    CHECK(view.dragging() == NavigationAction::Orbit && !view.moved(), "begin() starts a drag the scheme doesn't name");
    view.cancel();
    CHECK(view.dragging() == NavigationAction::None && !view.release(Qt::LeftButton), "cancel() ends it");
}

USDCC_TEST(navigation_frame_key) {
    Settings settings;
    QKeyEvent f(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
    QKeyEvent shiftF(QEvent::KeyPress, Qt::Key_F, Qt::ShiftModifier);
    QKeyEvent g(QEvent::KeyPress, Qt::Key_G, Qt::NoModifier);
    CHECK(isFrameKey(&f, settings) && !isFrameKey(&shiftF, settings) && !isFrameKey(&g, settings), "F frames, until it is set");
    settings.define({.key = "Shortcuts/viewport.fitToView",
                     .category = "Shortcuts",
                     .type = usdcc::core::SettingType::Shortcut,
                     .defaultValue = std::string("F")});
    settings.setValue("Shortcuts/viewport.fitToView", std::string("G"));
    CHECK(isFrameKey(&g, settings) && !isFrameKey(&f, settings), "then the Fit to view shortcut does");
    QKeyEvent claimed(QEvent::ShortcutOverride, Qt::Key_G, Qt::NoModifier);
    claimed.ignore();
    CHECK(claimFrameKey(&claimed, settings) && claimed.isAccepted(), "and a view claims it before the viewport's action");
    CHECK(!claimFrameKey(&g, settings), "a key press isn't an override");
    settings.setValue("Shortcuts/viewport.fitToView", std::string(""));
    CHECK(!isFrameKey(&g, settings) && !isFrameKey(&f, settings), "no key, no framing");
}

USDCC_TEST(navigation_settings_page) {
    TestSettings test;
    Scale scale;
    Settings settings;
    defineNavigationSettings(settings);
    SettingsDialog dialog(nullptr, settings);
    dialog.resize(900, 700);
    dialog.show();
    settle();
    CHECK(dialog.categories().contains(QStringLiteral("Navigation")), "a Navigation page");
    dialog.showCategory(QStringLiteral("Navigation"));
    settle();

    auto* scheme = withKey<QComboBox>(&dialog, QStringLiteral("Navigation/scheme"));
    auto* pan = withKey<QPushButton>(&dialog, QStringLiteral("Navigation/view3d.pan"), QStringLiteral("settingMouseBinding"));
    auto* orbit = withKey<QPushButton>(&dialog, QStringLiteral("Navigation/view3d.orbit"), QStringLiteral("settingMouseBinding"));
    auto* note = withKey<QLabel>(&dialog, QStringLiteral("Navigation/view3d.pan"), QStringLiteral("settingConflict"));
    auto* orbitNote = withKey<QLabel>(&dialog, QStringLiteral("Navigation/view3d.orbit"), QStringLiteral("settingConflict"));
    CHECK(scheme && pan && orbit && note && orbitNote, "the scheme is a choice, a binding a button with a note");
    if (!scheme || !pan || !orbit || !note || !orbitNote) return;
    CHECK(scheme->currentText() == QStringLiteral("usdcc") && pan->text() == mouseBindingLabel(*usdcc::core::parseMouseBinding("Middle")),
          "showing usdcc's: \"%s\"", qPrintable(pan->text()));
    CHECK(note->isHidden(), "nothing to say at first");

    const QPointF at(pan->width() / 2.0, pan->height() / 2.0);
    // A click arms it; the next press is the binding.
    mouse(pan, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouse(pan, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    CHECK(text(settings, "Navigation/view3d.pan") == "Middle" && pan->text() != mouseBindingLabel(*usdcc::core::parseMouseBinding("Middle")),
          "a click only asks for the press: \"%s\"", qPrintable(pan->text()));
    mouse(pan, QEvent::MouseButtonPress, at, Qt::MiddleButton, Qt::MiddleButton, Qt::ShiftModifier);
    mouse(pan, QEvent::MouseButtonRelease, at, Qt::MiddleButton, Qt::NoButton, Qt::ShiftModifier);
    CHECK(text(settings, "Navigation/view3d.pan") == "Shift+Middle", "the press with its keys is the binding: %s",
          text(settings, "Navigation/view3d.pan").c_str());
    CHECK(pan->text() == mouseBindingLabel(*usdcc::core::parseMouseBinding("Shift+Middle")), "shown: \"%s\"", qPrintable(pan->text()));
    CHECK(scheme->currentText() == QStringLiteral("Custom"), "and the scheme is Custom");

    // What the definition refuses is said, and not taken.
    mouse(pan, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouse(pan, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    CHECK(text(settings, "Navigation/view3d.pan") == "Shift+Middle" && !note->isHidden() && note->text().contains(QStringLiteral("Alt")),
          "a left drag without Alt is refused, with the reason: \"%s\"", qPrintable(note->text()));
    mouse(pan, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouse(pan, QEvent::MouseButtonPress, at, Qt::RightButton, Qt::RightButton, Qt::AltModifier);
    CHECK(text(settings, "Navigation/view3d.pan") == "Alt+Right" && !note->isHidden() && note->text().contains(QStringLiteral("Zoom")) &&
              !orbitNote->isVisible(),
          "the right button is taken; the reason gives way to who else has it: \"%s\"", qPrintable(note->text()));
    auto* zoomNote = withKey<QLabel>(&dialog, QStringLiteral("Navigation/view3d.zoom"), QStringLiteral("settingConflict"));
    auto* zoom2dNote = withKey<QLabel>(&dialog, QStringLiteral("Navigation/editor2d.zoom"), QStringLiteral("settingConflict"));
    CHECK(zoomNote && !zoomNote->isHidden() && zoom2dNote && zoom2dNote->isHidden(),
          "both sides say so; the same binding in the 2D editors is no conflict");

    // Escape leaves it; Backspace binds nothing.
    mouse(pan, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    QTest::keyClick(pan, Qt::Key_Escape);
    CHECK(text(settings, "Navigation/view3d.pan") == "Alt+Right" && dialog.isVisible(), "Escape leaves it, and the dialog open");
    QTest::keyClick(pan, Qt::Key_Backspace);
    CHECK(text(settings, "Navigation/view3d.pan").empty() && pan->text() == QStringLiteral("None") && note->isHidden(),
          "Backspace binds nothing");

    // A preset from the choice.
    Q_EMIT scheme->activated(scheme->findText(QStringLiteral("Maya")));
    settle();
    CHECK(text(settings, "Navigation/scheme") == "Maya" && scheme->currentText() == QStringLiteral("Maya"), "Maya, chosen");
    CHECK(pan->text() == mouseBindingLabel(*usdcc::core::parseMouseBinding("Alt+Middle")) &&
              orbit->text() == mouseBindingLabel(*usdcc::core::parseMouseBinding("Alt+Left")),
          "a preset fills the bindings in: \"%s\"", qPrintable(pan->text()));
}

namespace {

using namespace PXR_NS;

// A box with its size keyed at 0, 10 and 20.
struct Animated {
    usdcc::usd::StageManager stages;
    std::shared_ptr<usdcc::usd::UndoManager> undo = std::make_shared<usdcc::usd::UndoManager>();
    usdcc::usd::StageRefPtr stage = stages.createStage();

    Animated() {
        const UsdGeomCube cube = UsdGeomCube::Define(stage->usdStage(), SdfPath("/Box"));
        const UsdAttribute size = cube.GetSizeAttr();
        size.Set(1.0, 0.0);
        size.Set(3.0, 10.0);
        size.Set(2.0, 20.0);
        stages.setSelectedPaths(stage, {SdfPath("/Box")});
    }
};

}  // namespace

USDCC_TEST(navigation_curve_editor) {
    TestSettings test;
    Scale scale;
    Animated scene;
    usdcc::usd::CurveEditorViewPanel panel(&scene.stages, scene.stage, scene.undo);
    panel.resize(900, 500);
    panel.show();
    QTest::qWait(200);
    settle();
    usdcc::usd::CurveView* view = panel.view();
    Settings& settings = Settings::instance();

    // The middle button pans both ways: what was under the cursor still is.
    const QPointF from(300, 200);
    const QPointF to(360, 170);
    const double time = view->axis().timeAt(from.x());
    const double value = view->valueAt(from.y());
    drag(view, from, to, Qt::MiddleButton);
    CHECK(isNear(view->axis().timeAt(to.x()), time, 1e-6) && isNear(view->valueAt(to.y()), value, 1e-6),
          "a middle drag carries the time and the value along");
    CHECK(panel.ruler()->range() == view->axis().range(), "and the ruler shows the same times");

    // The wheel zooms both about the cursor, a tenth a notch.
    const usdcc::usd::TimeRange times = view->axis().range();
    const std::pair<double, double> values = view->valueRange();
    const double under = view->axis().timeAt(to.x());
    const double valueUnder = view->valueAt(to.y());
    wheel(view, to, 1);
    CHECK(isNear((view->axis().range().end - view->axis().range().start) / (times.end - times.start), 0.9, 1e-6) &&
              isNear((view->valueRange().second - view->valueRange().first) / (values.second - values.first), 0.9, 1e-6),
          "a notch in shows nine tenths of the time and of the values");
    CHECK(isNear(view->axis().timeAt(to.x()), under, 1e-6) && isNear(view->valueAt(to.y()), valueUnder, 1e-6), "about the cursor");
    std::pair<double, double> before = view->valueRange();
    usdcc::usd::TimeRange timesBefore = view->axis().range();
    wheel(view, to, 1, Qt::ControlModifier);
    CHECK(view->valueRange() == before && view->axis().range().end - view->axis().range().start < timesBefore.end - timesBefore.start,
          "with Ctrl the time only");
    timesBefore = view->axis().range();
    wheel(view, to, -1, Qt::ShiftModifier);
    CHECK(view->axis().range() == timesBefore && view->valueRange().second - view->valueRange().first > before.second - before.first,
          "with Shift the values only");

    // Alt+Right drags: sideways the time, up the values, about where the drag began.
    timesBefore = view->axis().range();
    before = view->valueRange();
    const double anchored = view->axis().timeAt(from.x());
    drag(view, from, from + QPointF(48, 0), Qt::RightButton, Qt::AltModifier);
    CHECK(isNear((view->axis().range().end - view->axis().range().start) / (timesBefore.end - timesBefore.start), 0.81, 1e-6) &&
              view->valueRange() == before && isNear(view->axis().timeAt(from.x()), anchored, 1e-6),
          "48 pixels to the right is two steps into the time, the values as they were");
    timesBefore = view->axis().range();
    drag(view, from, from + QPointF(0, 24), Qt::RightButton, Qt::AltModifier);
    CHECK(view->axis().range() == timesBefore &&
              isNear((view->valueRange().second - view->valueRange().first) / (before.second - before.first), 1.0 / 0.9, 1e-6),
          "24 pixels down is a step out of the values");

    // The press that zooms opens no menu, whichever side of the drag the system asks on.
    int menus = 0;
    QObject::connect(view, &QWidget::customContextMenuRequested, view, [&menus]() { ++menus; });
    mouse(view, QEvent::MouseButtonPress, from, Qt::RightButton, Qt::RightButton, Qt::AltModifier);
    QContextMenuEvent withPress(QContextMenuEvent::Mouse, from.toPoint(), view->mapToGlobal(from.toPoint()));
    QApplication::sendEvent(view, &withPress);
    mouse(view, QEvent::MouseButtonRelease, from, Qt::RightButton, Qt::NoButton, Qt::AltModifier);
    QApplication::sendEvent(view, &withPress);
    CHECK(menus == 0, "an Alt+Right press is no menu's (%d)", menus);

    // Another scheme: Maya pans with Alt+Middle, and the middle button alone does nothing.
    settings.setValue("Navigation/scheme", std::string("Maya"));
    timesBefore = view->axis().range();
    drag(view, from, to, Qt::MiddleButton);
    CHECK(view->axis().range() == timesBefore, "Maya: the middle button alone doesn't pan");
    drag(view, from, to, Qt::MiddleButton, Qt::AltModifier);
    CHECK(!(view->axis().range() == timesBefore), "Alt+Middle does");
    // Blender: Ctrl+Middle zooms.
    settings.setValue("Navigation/scheme", std::string("Blender"));
    timesBefore = view->axis().range();
    drag(view, from, from + QPointF(24, 0), Qt::MiddleButton, Qt::ControlModifier);
    CHECK(isNear((view->axis().range().end - view->axis().range().start) / (timesBefore.end - timesBefore.start), 0.9, 1e-6),
          "Blender: Ctrl+Middle zooms");

    // A left drag is still the editor's own: a box, which moves nothing.
    timesBefore = view->axis().range();
    drag(view, QPointF(20, 20), QPointF(60, 60), Qt::LeftButton);
    CHECK(view->axis().range() == timesBefore, "a left drag is the editor's");
}

// The key that frames: the viewport's Fit to View has it for the whole application, and the
// view with the focus takes it first.
USDCC_TEST(navigation_frame_key_is_the_focused_views) {
    TestSettings test;
    Scale scale;
    Animated scene;
    QMainWindow window;
    auto* panel = new usdcc::usd::CurveEditorViewPanel(&scene.stages, scene.stage, scene.undo);
    window.setCentralWidget(panel);
    int fits = 0;
    auto* fit = new QAction(QStringLiteral("Fit to View"), &window);
    fit->setShortcut(QKeySequence(Qt::Key_F));
    fit->setShortcutContext(Qt::ApplicationShortcut);
    window.addAction(fit);
    QObject::connect(fit, &QAction::triggered, &window, [&fits]() { ++fits; });
    window.resize(900, 500);
    window.show();
    window.activateWindow();
    QTest::qWait(200);
    settle();

    usdcc::usd::CurveView* view = panel->view();
    const usdcc::usd::TimeRange framed = view->axis().range();
    drag(view, QPointF(300, 200), QPointF(500, 200), Qt::MiddleButton);
    CHECK(!(view->axis().range() == framed), "panned away");
    view->setFocus();
    settle();
    QTest::keyClick(view, Qt::Key_F);
    settle();
    CHECK(fits == 0 && view->axis().range() == framed, "F frames the curves while they have the focus (%d)", fits);
    view->clearFocus();
    window.setFocus();
    settle();
    QTest::keyClick(&window, Qt::Key_F);
    settle();
    CHECK(fits == 1, "and is the viewport's elsewhere (%d)", fits);
}

USDCC_TEST(navigation_dope_sheet) {
    TestSettings test;
    Scale scale;
    Animated scene;
    usdcc::usd::DopeSheetViewPanel panel(&scene.stages, scene.stage, scene.undo);
    panel.resize(900, 400);
    panel.show();
    QTest::qWait(200);
    settle();
    usdcc::usd::DopeSheetView* view = panel.view();

    const QPointF from(300, 60);
    const QPointF to(380, 60);
    const double time = view->axis().timeAt(from.x());
    drag(view, from, to, Qt::MiddleButton);
    CHECK(isNear(view->axis().timeAt(to.x()), time, 1e-6) && panel.ruler()->range() == view->axis().range(),
          "a middle drag carries the time along, and the ruler with it");

    usdcc::usd::TimeRange before = view->axis().range();
    wheel(view, to, 1);
    CHECK(view->axis().range() == before, "the wheel alone is the rows'");
    const double under = view->axis().timeAt(to.x());
    wheel(view, to, 1, Qt::ControlModifier);
    CHECK(isNear((view->axis().range().end - view->axis().range().start) / (before.end - before.start), 0.9, 1e-6) &&
              isNear(view->axis().timeAt(to.x()), under, 1e-6),
          "with Ctrl it zooms the time about the cursor, a tenth a notch, as in the curve editor");

    before = view->axis().range();
    drag(view, from, from + QPointF(24, -300), Qt::RightButton, Qt::AltModifier);
    CHECK(isNear((view->axis().range().end - view->axis().range().start) / (before.end - before.start), 0.9, 1e-6),
          "an Alt+Right drag zooms the time by its sideways part alone");
}
