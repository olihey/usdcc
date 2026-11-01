// scale_controller: the Ctrl+Numpad+/- shortcut (reserved over editors, counted
// once when delivered through a window like a real key press), the bounds and
// the View-menu actions, drift-free stylesheet and widget metrics across scale
// changes, bindings for widgets created later, and persistence. Runs in
// usdcc_ui_tests (a QApplication on Qt's offscreen platform); its settings
// live in a temporary directory, never in the user's usdcc.ini.
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/theme.h"
#include "usdcc/ui/title_bar.h"
#include "usdcc/usd/properties/color_swatch.h"
#include "usdcc/usd/properties/number_field.h"

#include <DockManager.h>

#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
#include <QWindow>

using usdcc::ui::UiScaleController;

namespace {

constexpr auto kKeypadControl = Qt::ControlModifier | Qt::KeypadModifier;

QApplication& application() { return *static_cast<QApplication*>(QCoreApplication::instance()); }

void press(QObject* target, int key, Qt::KeyboardModifiers modifiers) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(target, &event);
}

}  // namespace

USDCC_TEST(scale_controller) {
    QTemporaryDir temp;
    CHECK(temp.isValid(), "temporary settings directory");
    const QString settingsFile = temp.filePath(QStringLiteral("ui.ini"));
    {
        UiScaleController scale(application(), settingsFile);
        usdcc::ui::theme::apply(application());
        QWidget window;
        usdcc::ui::TitleBar title(&window);
        QLineEdit editor(&window);
        QDoubleSpinBox spin(&window);
        spin.setValue(42);
        QWidget floating;
        QLineEdit floatingEditor(&floating);
        usdcc::usd::properties::NumberField number;
        usdcc::usd::properties::ColorSwatch swatch;
        ads::CDockManager docks(&window);
        usdcc::ui::theme::applyToDockManager(&docks);
        const QString baseDockStyle = docks.styleSheet();
        const QString baseAppStyle = application().styleSheet();
        const QSize baseNumber = number.sizeHint();

        // The shortcut is reserved before an editor can claim it.
        QKeyEvent reserve(QEvent::ShortcutOverride, Qt::Key_Plus, kKeypadControl);
        reserve.ignore();
        QApplication::sendEvent(&editor, &reserve);
        CHECK(reserve.isAccepted(), "Ctrl+Numpad+ is reserved over a focused line edit");
        CHECK(scale.percent() == 100, "starts at 100%% (got %d)", scale.percent());

        press(&editor, Qt::Key_Plus, kKeypadControl);
        CHECK(scale.percent() == 110, "Ctrl+Numpad+ in a line edit: 110%% (got %d)", scale.percent());
        press(&spin, Qt::Key_Plus, kKeypadControl);
        CHECK(scale.percent() == 120 && spin.value() == 42,
              "Ctrl+Numpad+ in a spin box scales and leaves its value alone (%d%%, value %g)", scale.percent(),
              spin.value());
        press(&floatingEditor, Qt::Key_Minus, kKeypadControl);
        CHECK(scale.percent() == 110, "Ctrl+Numpad- in another top-level window: 110%% (got %d)", scale.percent());
        press(&editor, Qt::Key_Plus, Qt::ControlModifier);
        press(&editor, Qt::Key_Minus, Qt::KeypadModifier);
        press(&editor, Qt::Key_Plus, kKeypadControl | Qt::AltModifier);
        CHECK(scale.percent() == 110, "other modifier combinations are not the shortcut (got %d)", scale.percent());

        // A real key press arrives at the window first and is then forwarded to
        // the focus widget: it must count once, and the editor must not see it.
        window.show();
        editor.setFocus();
        QApplication::processEvents();
        QKeyEvent windowPress(QEvent::KeyPress, Qt::Key_Plus, kKeypadControl, QStringLiteral("+"));
        QApplication::sendEvent(window.windowHandle(), &windowPress);
        CHECK(scale.percent() == 120, "a key press delivered through the window counts once: 120%% (got %d)",
              scale.percent());
        CHECK(editor.text().isEmpty(), "the editor did not receive the '+' (text \"%s\")", qPrintable(editor.text()));
        window.hide();

        scale.setPercent(200);
        CHECK(title.height() == 52, "title bar 26 -> 52 px at 200%% (got %d)", title.height());
        CHECK(number.sizeHint() == baseNumber * 2, "NumberField size hint doubles");
        CHECK(swatch.sizeHint().height() == 40, "ColorSwatch height 20 -> 40 px (got %d)", swatch.sizeHint().height());
        CHECK(application().styleSheet().contains(QStringLiteral("font-size: 22px")),
              "app stylesheet font sizes scale (11px -> 22px)");
        CHECK(docks.styleSheet() != baseDockStyle, "dock stylesheet scales");

        QWidget late;
        usdcc::ui::scaled::fixedWidth(&late, 72);
        CHECK(late.width() == 144, "a widget bound later starts at the current scale (got %d)", late.width());
        usdcc::ui::scaled::fixedWidth(&late, 80);
        scale.setPercent(150);
        CHECK(late.width() == 120, "rebinding a metric replaces the old binding (got %d)", late.width());

        const QString original =
            QStringLiteral("/* 20px */ X { padding: 4px; border: 1px; image: url(icon20px.png); }");
        CHECK(scale.styleSheet(original) ==
                  QStringLiteral("/* 20px */ X { padding: 6px; border: 1px; image: url(icon20px.png); }"),
              "stylesheet scaling skips comments, url()s and 1px hairlines: %s", qPrintable(scale.styleSheet(original)));
        CHECK(scale.pixels(-1) == -1, "negative Qt sentinels pass through");

        for (int i = 0; i < 15; ++i) {
            scale.setPercent(200);
            scale.setPercent(80);
            scale.setPercent(100);
        }
        CHECK(application().styleSheet() == baseAppStyle, "app stylesheet is back to the original, no drift");
        CHECK(docks.styleSheet() == baseDockStyle, "dock stylesheet is back to the original, no drift");
        CHECK(title.height() == 26, "title bar back to 26 px (got %d)", title.height());
        CHECK(number.sizeHint() == baseNumber, "NumberField size hint back to the original");
        usdcc::ui::theme::applyToDockManager(&docks);
        CHECK(docks.styleSheet() == baseDockStyle, "reapplying the dock theme doesn't grow the stylesheet");

        QMenu view;
        scale.addMenu(&view);
        QMenu* menu = view.actions().last()->menu();
        scale.setPercent(2000);
        CHECK(scale.percent() == 200, "clamped to 200%% (got %d)", scale.percent());
        CHECK(!menu->actions()[0]->isEnabled(), "Increase is disabled at the maximum");
        scale.setPercent(0);
        CHECK(scale.percent() == 80, "clamped to 80%% (got %d)", scale.percent());
        CHECK(!menu->actions()[1]->isEnabled(), "Decrease is disabled at the minimum");
        menu->actions().last()->trigger();
        CHECK(scale.percent() == 100, "Reset to 100%% (got %d)", scale.percent());
        scale.setPercent(140);
    }
    {
        UiScaleController restored(application(), settingsFile);
        CHECK(restored.percent() == 140, "the scale is restored from the settings (got %d)", restored.percent());
        QWidget late;
        usdcc::ui::scaled::fixedHeight(&late, 20);
        CHECK(late.height() == 28, "a new binding uses the restored scale (got %d)", late.height());
    }
    {
        QSettings invalid(settingsFile, QSettings::IniFormat);
        invalid.setValue(QStringLiteral("Appearance/uiScale"), QStringLiteral("invalid"));
    }
    {
        UiScaleController restored(application(), settingsFile);
        CHECK(restored.percent() == 100, "an unreadable saved value falls back to 100%% (got %d)", restored.percent());
    }
}
