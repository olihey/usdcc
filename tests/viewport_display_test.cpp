// The viewport's grid and background (docs/VIEWPORT_GRID.md) and selection style
// (docs/VIEWPORT_SELECTION.md): the Settings > Viewport defaults, a viewport's own
// overrides of them (kept when a default changes, followed otherwise), the options menu
// and the Display dialog, and the overrides
// carried by a duplicate and a session. The viewports are never shown, so nothing here
// needs a GL context. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/core/settings.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/viewport_display.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QComboBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QToolButton>

#include <memory>
#include <string>

using usdcc::core::Settings;
using usdcc::usd::SelectionStyle;
using usdcc::usd::ViewportDisplayOverrides;
using usdcc::usd::ViewportViewPanel;

namespace {
QAction* menuAction(QMenu& menu, const QString& name) {
    for (QAction* action : menu.actions()) {
        if (action->objectName() == name) return action;
    }
    return nullptr;
}
}  // namespace

USDCC_TEST(viewport_display_settings) {
    TestSettings testSettings;
    usdcc::usd::defineViewportDisplaySettings();
    Settings& settings = Settings::instance();
    CHECK(settings.contains(usdcc::usd::kViewportShowGridKey) && settings.contains(usdcc::usd::kViewportGridSizeKey) &&
              settings.contains(usdcc::usd::kViewportGridStepKey) && settings.contains(usdcc::usd::kViewportBackgroundKey),
          "the four settings are defined");
    CHECK(settings.definition(usdcc::usd::kViewportBackgroundKey)->category == "Viewport", "on the Viewport page");
    const auto defaults = usdcc::usd::defaultViewportDisplay();
    CHECK(defaults == usdcc::usd::HydraViewportWindow::DisplayOptions{}, "the built-in defaults: grid on, 20 by 1, #1c1c1e");
    CHECK(!settings.setValue(usdcc::usd::kViewportGridStepKey, 0.0), "a step of 0 doesn't fit");
    CHECK(settings.setValue(usdcc::usd::kViewportBackgroundKey, std::string("#336699")), "a color");
    CHECK(usdcc::usd::defaultViewportDisplay().background == QColor(0x33, 0x66, 0x99), "the default follows its setting");

    // The selection's three.
    CHECK(settings.contains(usdcc::usd::kViewportSelectionStyleKey) && settings.contains(usdcc::usd::kViewportSelectionColorKey) &&
              settings.contains(usdcc::usd::kViewportSelectionWidthKey),
          "the selection settings are defined");
    CHECK(defaults.selectionStyle == SelectionStyle::Solid && defaults.selectionColor == QColor("#ffa600") &&
              defaults.selectionWidth == 2,
          "Solid, orange, 2px by default");
    CHECK(settings.definition(usdcc::usd::kViewportSelectionStyleKey)->choices ==
              std::vector<std::string>({"Solid", "Outline", "Wireframe"}),
          "the style's choices");
    CHECK(settings.setValue(usdcc::usd::kViewportSelectionStyleKey, std::string("Wireframe")), "a style");
    CHECK(!settings.setValue(usdcc::usd::kViewportSelectionStyleKey, std::string("Dotted")), "not a style");
    CHECK(!settings.setValue(usdcc::usd::kViewportSelectionWidthKey, 9), "a width over 8 doesn't fit");
    CHECK(settings.setValue(usdcc::usd::kViewportSelectionWidthKey, 5), "a width");
    CHECK(settings.setValue(usdcc::usd::kViewportSelectionColorKey, std::string("#33ccff")), "a selection color");
    const auto selection = usdcc::usd::defaultViewportDisplay();
    CHECK(selection.selectionStyle == SelectionStyle::Wireframe && selection.selectionWidth == 5 &&
              selection.selectionColor == QColor("#33ccff"),
          "the defaults follow their settings");
}

USDCC_TEST(viewport_selection_style_names) {
    for (SelectionStyle style : {SelectionStyle::Solid, SelectionStyle::Outline, SelectionStyle::Wireframe}) {
        CHECK(usdcc::usd::selectionStyleFromName(QString::fromLatin1(usdcc::usd::selectionStyleName(style))) == style,
              "%s round trips", usdcc::usd::selectionStyleName(style));
        CHECK(usdcc::usd::selectionStyleFromName(usdcc::usd::selectionStyleLabel(style)) == style,
              "and its label (the setting's value) reads back");
    }
    CHECK(!usdcc::usd::selectionStyleFromName(QStringLiteral("dotted")), "not a style");
}

USDCC_TEST(viewport_display_overrides_json) {
    ViewportDisplayOverrides overrides;
    CHECK(overrides.isEmpty() && overrides.toJson().isEmpty(), "none: nothing saved");
    overrides.gridStep = 0.5;
    overrides.background = QColor(Qt::white);
    const QJsonObject json = overrides.toJson();
    CHECK(json.size() == 2 && json.value("background").toString() == "#ffffff", "only what's set");
    CHECK(ViewportDisplayOverrides::fromJson(json) == overrides, "round trip");
    const ViewportDisplayOverrides bad = ViewportDisplayOverrides::fromJson(
        QJsonObject{{"showGrid", "yes"}, {"gridSize", -3.0}, {"gridStep", 0.0}, {"background", "not a color"}});
    CHECK(bad.isEmpty(), "what doesn't fit is ignored");
    usdcc::usd::HydraViewportWindow::DisplayOptions defaults;
    const auto applied = overrides.applyTo(defaults);
    CHECK(applied.gridStep == 0.5 && applied.background == QColor(Qt::white) && applied.gridSize == defaults.gridSize &&
              applied.showGrid == defaults.showGrid,
          "applied over the defaults");

    // The selection's.
    ViewportDisplayOverrides selection;
    selection.selectionStyle = SelectionStyle::Outline;
    selection.selectionColor = QColor("#33ccff");
    selection.selectionWidth = 3;
    CHECK(!selection.isEmpty(), "selection overrides aren't none");
    const QJsonObject selectionJson = selection.toJson();
    CHECK(selectionJson.value("selectionStyle").toString() == "outline" && selectionJson.value("selectionColor").toString() == "#33ccff" &&
              selectionJson.value("selectionWidth").toInt() == 3,
          "saved as named");
    CHECK(ViewportDisplayOverrides::fromJson(selectionJson) == selection, "selection round trip");
    const ViewportDisplayOverrides badSelection = ViewportDisplayOverrides::fromJson(
        QJsonObject{{"selectionStyle", "dotted"}, {"selectionColor", "nope"}, {"selectionWidth", 9}});
    CHECK(badSelection.isEmpty(), "a style, color or width that doesn't fit is ignored");
    CHECK(ViewportDisplayOverrides::fromJson(QJsonObject{{"selectionWidth", 2.5}}).isEmpty(), "a width is whole");
    const auto selectionApplied = selection.applyTo(defaults);
    CHECK(selectionApplied.selectionStyle == SelectionStyle::Outline && selectionApplied.selectionWidth == 3 &&
              selectionApplied.background == defaults.background,
          "selection applied over the defaults");
}

USDCC_TEST(viewport_display_panel) {
    TestSettings testSettings;
    QTemporaryDir dir;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       dir.filePath(QStringLiteral("ui.ini")));
    usdcc::usd::StageManager stages;
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    auto panel = std::make_unique<ViewportViewPanel>(&stages, nullptr, toolSet, nullptr);
    Settings& settings = Settings::instance();
    auto* window = panel->hydraWindow();

    CHECK(panel->displayOverrides().isEmpty(), "a new viewport overrides nothing");
    CHECK(window->displayOptions() == usdcc::usd::defaultViewportDisplay(), "and shows the defaults");

    // Not overridden: follows the setting.
    settings.setValue(usdcc::usd::kViewportGridSizeKey, 50.0);
    settings.setValue(usdcc::usd::kViewportBackgroundKey, std::string("#202830"));
    CHECK(window->displayOptions().gridSize == 50.0 && window->displayOptions().background == QColor("#202830"),
          "a changed default reaches the viewport");

    // Overridden: kept, whatever the setting becomes.
    int changes = 0;
    QObject::connect(panel.get(), &ViewportViewPanel::displayChanged, [&changes]() { ++changes; });
    ViewportDisplayOverrides overrides;
    overrides.gridSize = 5.0;
    overrides.background = QColor("#ff0000");
    panel->setDisplayOverrides(overrides);
    CHECK(changes == 1, "displayChanged() fired: %d", changes);
    settings.setValue(usdcc::usd::kViewportGridSizeKey, 70.0);
    settings.setValue(usdcc::usd::kViewportGridStepKey, 2.0);
    CHECK(window->displayOptions().gridSize == 5.0 && window->displayOptions().background == QColor("#ff0000"),
          "an override stays");
    CHECK(window->displayOptions().gridStep == 2.0, "the step, not overridden, follows");

    // The options menu (populateOptionsMenu() is protected: reached through a subclass).
    struct Opener : ViewportViewPanel {
        static void populate(ViewportViewPanel* p, QMenu* m) { (p->*(&Opener::populateOptionsMenu))(m); }
    };
    {
        QMenu menu;
        Opener::populate(panel.get(), &menu);
        QAction* showGrid = menuAction(menu, "showGridAction");
        QAction* defaults = menuAction(menu, "displayDefaultsAction");
        CHECK(showGrid && showGrid->isCheckable() && showGrid->isChecked(), "Show Grid, ticked");
        CHECK(defaults && defaults->isEnabled(), "Use Settings Defaults, there's something to drop");
        if (showGrid) showGrid->trigger();
        CHECK(panel->displayOverrides().showGrid == false && !window->displayOptions().showGrid, "Show Grid turns it off here");
        settings.setValue(usdcc::usd::kViewportShowGridKey, true);
        CHECK(!window->displayOptions().showGrid, "and it stays off");

        // The Selection submenu: radio entries, Solid ticked, a choice overrides.
        QMenu* selection = menu.findChild<QMenu*>("selectionStyleMenu");
        CHECK(selection && selection->actions().size() == 3, "a Selection submenu with three styles");
        QAction* solid = menuAction(*selection, "selectionStyle_solid");
        QAction* outline = menuAction(*selection, "selectionStyle_outline");
        CHECK(solid && solid->isChecked() && outline && !outline->isChecked(), "Solid ticked");
        if (outline) outline->trigger();
        CHECK(panel->displayOverrides().selectionStyle == SelectionStyle::Outline &&
                  window->displayOptions().selectionStyle == SelectionStyle::Outline,
              "Outline chosen here");
        settings.setValue(usdcc::usd::kViewportSelectionStyleKey, std::string("Wireframe"));
        CHECK(window->displayOptions().selectionStyle == SelectionStyle::Outline, "and it stays");
        settings.setValue(usdcc::usd::kViewportSelectionColorKey, std::string("#00ff00"));
        CHECK(window->displayOptions().selectionColor == QColor("#00ff00"), "the color, not overridden, follows");
    }

    // Duplicate and session carry the overrides.
    std::unique_ptr<usdcc::ui::ViewPanel> copy(panel->duplicate());
    auto* viewportCopy = static_cast<ViewportViewPanel*>(copy.get());
    CHECK(viewportCopy->displayOverrides() == panel->displayOverrides(), "the duplicate has the same overrides");
    CHECK(viewportCopy->hydraWindow()->displayOptions() == window->displayOptions(), "and shows the same");
    const QJsonObject state = panel->saveSessionState();
    CHECK(state.contains("display"), "the session has them");
    auto restored = std::make_unique<ViewportViewPanel>(&stages, nullptr, toolSet, nullptr);
    restored->restoreSessionState(state);
    CHECK(restored->displayOverrides() == panel->displayOverrides(), "a restored viewport has them back");
    restored->restoreSessionState(QJsonObject{});
    CHECK(restored->displayOverrides().isEmpty(), "an older session without them: none");

    // The dialog: an edit overrides; Default drops it.
    QDialog* dialog = panel->showDisplayDialog();
    CHECK(dialog && dialog == panel->showDisplayDialog(), "one dialog per viewport");
    auto* step = dialog->findChild<QDoubleSpinBox*>("displayGridStep");
    auto* resetStep = dialog->findChild<QToolButton*>("resetGridStep");
    auto* resetSize = dialog->findChild<QToolButton*>("resetGridSize");
    auto* showGrid = dialog->findChild<QCheckBox*>("displayShowGrid");
    CHECK(step && resetStep && resetSize && showGrid, "the dialog's editors");
    {
        // Enter in a spin box must not reach a button: no default, none that becomes one.
        bool anyDefault = false;
        for (QPushButton* button : dialog->findChildren<QPushButton*>()) {
            anyDefault = anyDefault || button->isDefault() || button->autoDefault();
        }
        CHECK(!anyDefault, "no default button: Enter doesn't open the color picker");
        if (step && !anyDefault) {  // with a default button, Enter would block in the picker's exec()
            const bool wasVisible = dialog->isVisible();
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(step, &enter);
            CHECK(dialog->isVisible() == wasVisible, "Enter in Grid step leaves the dialog as it was");
        }
    }
    if (step && resetStep && resetSize && showGrid) {
        CHECK(step->value() == 2.0 && resetStep->isHidden() && !resetSize->isHidden() && !showGrid->isChecked(),
              "it shows the viewport's values, Default where overridden");
        step->setValue(0.25);
        CHECK(panel->displayOverrides().gridStep == 0.25 && window->displayOptions().gridStep == 0.25, "an edit overrides");
        CHECK(!resetStep->isHidden(), "Default shows");
        resetSize->click();
        CHECK(!panel->displayOverrides().gridSize && window->displayOptions().gridSize == 70.0,
              "Default drops the override: the setting's 70 again");
        panel->setDisplayOverrides({});
        CHECK(showGrid->isChecked() && step->value() == 2.0, "the dialog follows a change made elsewhere");
    }
    auto* style = dialog->findChild<QComboBox*>("displaySelectionStyle");
    auto* width = dialog->findChild<QSpinBox*>("displaySelectionWidth");
    auto* resetStyle = dialog->findChild<QToolButton*>("resetSelectionStyle");
    auto* resetWidth = dialog->findChild<QToolButton*>("resetSelectionWidth");
    CHECK(style && width && resetStyle && resetWidth && dialog->findChild<QPushButton*>("displaySelectionColor"),
          "the selection rows");
    if (style && width && resetStyle && resetWidth) {
        CHECK(style->currentText() == "Wireframe" && resetStyle->isHidden() && width->isEnabled(),
              "the setting's style, not overridden; the width is used");
        style->setCurrentIndex(style->findText("Solid"));
        CHECK(panel->displayOverrides().selectionStyle == SelectionStyle::Solid && !resetStyle->isHidden(), "a style overrides");
        CHECK(!width->isEnabled(), "Solid has no lines: no width");
        style->setCurrentIndex(style->findText("Outline"));
        width->setValue(6);
        CHECK(panel->displayOverrides().selectionWidth == 6 && window->displayOptions().selectionWidth == 6 && !resetWidth->isHidden(),
              "a width overrides");
        resetWidth->click();
        CHECK(!panel->displayOverrides().selectionWidth && width->value() == 2, "Default: the setting's width again");
        resetStyle->click();
        CHECK(!panel->displayOverrides().selectionStyle && style->currentText() == "Wireframe", "Default: the setting's style");
    }
    CHECK(dialog->windowTitle().startsWith("Viewport Display"), "the dialog is Display now");
    dialog->close();
}
