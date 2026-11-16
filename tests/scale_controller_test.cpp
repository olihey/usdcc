// scale_controller: the Ctrl+Numpad+/- shortcut (reserved over editors, counted
// once when delivered through a window like a real key press), the bounds and
// the View-menu actions, drift-free stylesheet and widget metrics across scale
// changes, bindings for widgets created later, and persistence. Runs in
// usdcc_ui_tests (a QApplication on Qt's offscreen platform); its settings
// live in a temporary directory, never in the user's usdcc.ini.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/theme.h"
#include "usdcc/ui/title_bar.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/properties/color_swatch.h"
#include "usdcc/usd/properties/number_field.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/tool_rail.h"
#include "usdcc/usd/undo_manager.h"

#include <DockManager.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QSettings>
#include <QStringList>
#include <QStyle>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

using usdcc::ui::UiScaleController;

namespace {

constexpr auto kKeypadControl = Qt::ControlModifier | Qt::KeypadModifier;

QApplication& application() { return *static_cast<QApplication*>(QCoreApplication::instance()); }

void waitMs(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

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
        const int clearIconSize = editor.style()->pixelMetric(QStyle::PM_LineEditIconSize, nullptr, &editor);
        CHECK(clearIconSize == 32, "line edit clear button 16 -> 32 px (got %d)", clearIconSize);
        const QIcon clearIcon = editor.style()->standardIcon(QStyle::SP_LineEditClearButton, nullptr, &editor);
        CHECK(!clearIcon.pixmap(QSize(32, 32), 2.0).isNull(), "the clear icon renders at any size");
        for (const int percent : {100, 150, 200}) {
            scale.setPercent(percent);
            QLineEdit search;
            search.setClearButtonEnabled(true);
            search.setText(QStringLiteral("layer"));
            search.resize(scale.pixels(200), search.sizeHint().height());
            search.show();
            waitMs(300);  // the clear button fades in
            const QImage shot = search.grab().toImage();
            QRect icon;
            for (int y = 0; y < shot.height(); ++y) {
                for (int x = shot.width() / 2; x < shot.width(); ++x) {
                    if (qRed(shot.pixel(x, y)) > 80) icon |= QRect(x, y, 1, 1);
                }
            }
            const int top = icon.top();
            const int bottom = shot.height() - 1 - icon.bottom();
            const int right = shot.width() - 1 - icon.right();
            CHECK(!icon.isEmpty() && qAbs(top - bottom) <= 1 && qAbs(right - top) <= 1,
                  "clear icon gaps at %d%%: top %d, bottom %d, right %d", percent, top, bottom, right);
        }
        scale.setPercent(200);

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

USDCC_TEST(scale_controller_tool_rail_round_trip) {
    // The main window's tool rail is as wide after 100% -> 200% -> 100% as it was at the
    // start, and it takes its own stylesheet rules from the start, not only after the first
    // restyle (a scale change), which once widened it from 24 to 36 px.
    QTemporaryDir temp;
    CHECK(temp.isValid(), "temporary settings directory");
    UiScaleController scale(application(), temp.filePath(QStringLiteral("ui.ini")));
    usdcc::ui::theme::apply(application());

    QMainWindow window;
    auto* rail = new usdcc::usd::ToolRail(&window);
    for (const char* tool : {"Select", "Move", "Rotate", "Scale"}) rail->addTool(QString::fromLatin1(tool), QString::fromLatin1(tool), {});
    window.addToolBar(Qt::LeftToolBarArea, rail);
    window.setCentralWidget(new QWidget);
    window.resize(400, 300);
    window.show();
    QApplication::processEvents();

    const auto metrics = [rail]() {
        const QMargins m = rail->layout()->contentsMargins();
        return QStringLiteral("width %1, spacing %2, margins %3 %4 %5 %6")
            .arg(rail->sizeHint().width())
            .arg(rail->layout()->spacing())
            .arg(m.left())
            .arg(m.top())
            .arg(m.right())
            .arg(m.bottom());
    };
    const QString before = metrics();
    CHECK(rail->sizeHint().width() == 24, "the rail is as wide as its 24 px buttons (%s)", qPrintable(before));
    scale.setPercent(200);
    QApplication::processEvents();
    CHECK(rail->sizeHint().width() == 48, "twice as wide at 200%% (%s)", qPrintable(metrics()));
    scale.setPercent(100);
    QApplication::processEvents();
    CHECK(metrics() == before, "back at 100%%: as at the start (%s, was %s)", qPrintable(metrics()), qPrintable(before));

    // A rule for the rail by its name applies to a new rail at once.
    const QString sheet = application().styleSheet();
    application().setStyleSheet(sheet + QStringLiteral("\nQToolBar#toolRail { spacing: 5px; }"));
    auto* styled = new usdcc::usd::ToolRail(&window);
    CHECK(styled->layout()->spacing() == 5, "a new rail's spacing from its own rule (got %d)", styled->layout()->spacing());
    delete styled;
    application().setStyleSheet(sheet);
}

USDCC_TEST(scale_controller_panel_round_trip) {
    // After 100% -> 200% -> 100%, a panel's stage combo and the title bar's workspace
    // button are the size they were at the start: the combo is sized by its "No stage"
    // placeholder, not by whatever it held when first shown, and the button's height
    // comes from the theme, which the theme's QToolButton min-height can't undercut.
    TestSettings settings;
    QTemporaryDir temp;
    CHECK(temp.isValid(), "temporary directory");
    UiScaleController scale(application(), temp.filePath(QStringLiteral("ui.ini")));
    usdcc::ui::theme::apply(application());

    usdcc::usd::StageManager stages;
    auto undo = std::make_shared<usdcc::usd::UndoManager>();
    QWidget window;
    window.resize(320, 240);
    auto* windowLayout = new QVBoxLayout(&window);
    auto* workspace = new QToolButton(&window);
    workspace->setObjectName(QStringLiteral("titleBarWorkspaceButton"));
    workspace->setToolButtonStyle(Qt::ToolButtonTextOnly);
    workspace->setPopupMode(QToolButton::InstantPopup);
    workspace->setMenu(new QMenu(workspace));
    workspace->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    workspace->setText(QStringLiteral("Layout"));
    windowLayout->addWidget(workspace);
    ads::CDockManager docks(&window);
    windowLayout->addWidget(&docks, 1);
    usdcc::ui::theme::applyToDockManager(&docks);
    auto* panel = new usdcc::usd::AttributesViewPanel(&stages, nullptr, undo, QStringLiteral("Attributes"), &window);
    docks.addDockWidget(ads::CenterDockWidgetArea, panel);
    window.show();
    QApplication::processEvents();

    QComboBox* combo = nullptr;
    for (QComboBox* candidate : panel->findChildren<QComboBox*>()) {
        if (candidate->placeholderText() == "No stage") combo = candidate;
    }
    CHECK(combo != nullptr, "the panel's stage combo");
    if (!combo) return;
    const QSize emptyHint = combo->sizeHint();
    CHECK(workspace->height() == 22, "the workspace button is 22 px tall from the start (got %d)", workspace->height());

    const QString path = temp.filePath(QStringLiteral("a_stage_with_a_rather_long_file_name.usda"));
    {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly), "the stage file");
        file.write("#usda 1.0\n");
    }
    CHECK(stages.openStage(path.toStdString()) != nullptr, "the stage opens");
    QApplication::processEvents();
    CHECK(combo->count() == 1 && combo->sizeHint() == emptyHint, "the combo's size hint doesn't follow its stage name (%dx%d, was %dx%d)",
          combo->sizeHint().width(), combo->sizeHint().height(), emptyHint.width(), emptyHint.height());
    const QSize comboSize = combo->size();
    const auto toolbarState = [combo]() {
        QStringList parts;
        QWidget* bar = combo->parentWidget();
        const QMargins m = bar->layout()->contentsMargins();
        parts << QStringLiteral("bar %1 m%2,%3 sp%4").arg(bar->width()).arg(m.left()).arg(m.right()).arg(bar->layout()->spacing());
        for (QWidget* child : bar->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            parts << QStringLiteral("%6%1 x%2 w%3 hint%4 min%5")
                         .arg(QString::fromLatin1(child->metaObject()->className()))
                         .arg(child->x())
                         .arg(child->width())
                         .arg(child->sizeHint().width())
                         .arg(child->minimumSizeHint().width())
                         .arg(child->isVisible() ? QString() : QStringLiteral("(hidden) "));
        }
        return parts.join(QStringLiteral(" | "));
    };
    const QString before = toolbarState();

    scale.setPercent(200);
    waitMs(100);
    CHECK(workspace->height() == 44, "44 px at 200%% (got %d)", workspace->height());
    scale.setPercent(100);
    waitMs(100);
    CHECK(combo->sizeHint() == emptyHint && combo->size() == comboSize, "back at 100%%: the combo as before (%s; was %s)",
          qPrintable(toolbarState()), qPrintable(before));
    CHECK(workspace->height() == 22, "and the workspace button (got %d)", workspace->height());
}
