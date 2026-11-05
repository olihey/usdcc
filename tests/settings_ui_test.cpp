// The Qt side of the preferences (docs/SETTINGS.md): the usdcc.ini backend,
// the accent colour reaching the stylesheets and palette, the Settings
// dialog's pages and editors, and the tool shortcuts — including real key
// presses through the window system, where a text field must keep its
// letters. Runs in usdcc_ui_tests (offscreen); settings stay in temporary
// files and memory (test_settings.h).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/core/settings.h"
#include "usdcc/tools/select_tool.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/settings_dialog.h"
#include "usdcc/ui/theme.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/tool_rail.h"
#include "usdcc/usd/viewport_tool_bars.h"

#include <DockManager.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <memory>
#include <string>

using usdcc::core::SettingDefinition;
using usdcc::core::Settings;
using usdcc::core::SettingType;

namespace {

QApplication& application() { return *static_cast<QApplication*>(QCoreApplication::instance()); }

void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 60) QApplication::processEvents();
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

// A key press and release on the focus widget (or `window`), the way QTest
// sends them: through ShortcutOverride and the shortcut map first, as a real
// keyboard's would.
void type(QWidget* window, Qt::Key key) {
    QWidget* target = QApplication::focusWidget() ? QApplication::focusWidget() : window;
    QTest::keyClick(target, key);
    QApplication::processEvents();
}

}  // namespace

USDCC_TEST(settings_ini_backend) {
    QTemporaryDir temp;
    const QString file = temp.filePath(QStringLiteral("settings.ini"));
    const std::vector<SettingDefinition> definitions = {
        {.key = "A/flag", .category = "A", .type = SettingType::Bool, .defaultValue = false},
        {.key = "A/count", .category = "A", .type = SettingType::Int, .defaultValue = 1},
        {.key = "A/ratio", .category = "A", .type = SettingType::Float, .defaultValue = 0.5},
        {.key = "A/name", .category = "A", .type = SettingType::String, .defaultValue = std::string()},
        {.key = "A/color", .category = "A", .type = SettingType::Color, .defaultValue = std::string("#000000")},
        {.key = "A/key", .category = "A", .type = SettingType::Shortcut, .defaultValue = std::string("W")},
    };
    {
        Settings settings;
        settings.setBackend(std::make_unique<usdcc::ui::QSettingsBackend>(file));
        for (const auto& definition : definitions) settings.define(definition);
        settings.setValue("A/flag", true);
        settings.setValue("A/count", 7);
        settings.setValue("A/ratio", 0.25);
        settings.setValue("A/name", std::string("a, b; c"));
        settings.setValue("A/color", std::string("#E5484D"));
        settings.setValue("A/key", usdcc::ui::shortcutText(QKeySequence(Qt::CTRL | Qt::Key_Comma)));
    }
    Settings settings;
    settings.setBackend(std::make_unique<usdcc::ui::QSettingsBackend>(file));
    for (const auto& definition : definitions) settings.define(definition);
    CHECK(settings.get<bool>("A/flag"), "a bool survives the INI file");
    CHECK(settings.get<int>("A/count") == 7, "an int survives (got %d)", settings.get<int>("A/count"));
    CHECK(settings.get<double>("A/ratio") == 0.25, "a float survives");
    CHECK(settings.get<std::string>("A/name") == "a, b; c", "a string with separators survives (got \"%s\")",
          settings.get<std::string>("A/name").c_str());
    CHECK(usdcc::ui::settingColor("A/color", settings) == QColor(0xe5, 0x48, 0x4d), "a color survives");
    CHECK(usdcc::ui::settingShortcut("A/key", settings) == QKeySequence(Qt::CTRL | Qt::Key_Comma),
          "a shortcut with a comma survives (got \"%s\")", settings.get<std::string>("A/key").c_str());
    settings.reset("A/count");
    Settings again;
    again.setBackend(std::make_unique<usdcc::ui::QSettingsBackend>(file));
    for (const auto& definition : definitions) again.define(definition);
    CHECK(again.get<int>("A/count") == 1, "a reset value is gone from the file");
}

USDCC_TEST(settings_accent_color) {
    TestSettings testSettings;
    QTemporaryDir temp;
    usdcc::ui::UiScaleController scale(application(), temp.filePath(QStringLiteral("ui.ini")));
    usdcc::ui::theme::apply(application());
    QWidget window;
    ads::CDockManager docks(&window);
    usdcc::ui::theme::applyToDockManager(&docks);

    const QString tokens = QStringLiteral("a: @accent; b: rgba(@accent-rgb, 25%); c: @accent-hover;");
    const QString resolved = usdcc::ui::theme::resolveTokens(tokens, QColor(0x10, 0x20, 0x30));
    CHECK(resolved.startsWith(QStringLiteral("a: #102030; b: rgba(16, 32, 48, 25%); c: #")) &&
              !resolved.contains(QLatin1Char('@')),
          "tokens resolve: %s", qPrintable(resolved));

    auto& settings = Settings::instance();
    CHECK(settings.contains(usdcc::ui::theme::accentColorKey), "apply() defines the accent setting");
    CHECK(usdcc::ui::theme::accent() == QColor(0x3d, 0x8b, 0xfd), "the default accent is the theme's blue");
    CHECK(application().styleSheet().contains(QStringLiteral("#3d8bfd")) &&
              !application().styleSheet().contains(QStringLiteral("@accent")),
          "the app stylesheet has the accent and no unresolved token");

    settings.setValue(usdcc::ui::theme::accentColorKey, std::string("#e5484d"));
    CHECK(usdcc::ui::theme::accent() == QColor(0xe5, 0x48, 0x4d), "accent() follows the setting");
    CHECK(application().styleSheet().contains(QStringLiteral("#e5484d")) &&
              application().styleSheet().contains(QStringLiteral("229, 72, 77")) &&
              !application().styleSheet().contains(QStringLiteral("selection-background-color: #3d8bfd")),
          "the app stylesheet is reapplied with the new accent");
    CHECK(docks.styleSheet().contains(QStringLiteral("229, 72, 77")), "so is the dock stylesheet");
    CHECK(application().palette().color(QPalette::Highlight) == QColor(0xe5, 0x48, 0x4d),
          "and the palette's highlight");

    scale.setPercent(150);
    CHECK(application().styleSheet().contains(QStringLiteral("#e5484d")), "a scale change keeps the accent");
    scale.setPercent(100);
    settings.reset(usdcc::ui::theme::accentColorKey);
    CHECK(application().styleSheet().contains(QStringLiteral("selection-background-color: #3d8bfd")),
          "reset brings the blue back");
}

USDCC_TEST(settings_dialog) {
    TestSettings testSettings;
    QTemporaryDir temp;
    usdcc::ui::UiScaleController scale(application(), temp.filePath(QStringLiteral("ui.ini")));
    Settings settings;
    settings.define({.key = "Plugin/level", .category = "My Plugin", .label = "Level", .type = SettingType::Int,
                     .defaultValue = 3, .minimum = 1, .maximum = 10});
    settings.define({.key = "Appearance/accent", .category = "Appearance", .group = "Theme", .label = "Accent",
                     .description = "The accent.", .type = SettingType::Color, .defaultValue = std::string("#3d8bfd")});
    settings.define({.key = "Shortcuts/a", .category = "Shortcuts", .group = "Tools", .label = "Tool A",
                     .type = SettingType::Shortcut, .defaultValue = std::string("W")});
    settings.define({.key = "Shortcuts/b", .category = "Shortcuts", .group = "Tools", .label = "Tool B",
                     .type = SettingType::Shortcut, .defaultValue = std::string("E")});

    usdcc::ui::SettingsDialog dialog(nullptr, settings);
    CHECK(dialog.categories() == QStringList({"Appearance", "Shortcuts", "My Plugin"}),
          "known categories first, then others: %s", qPrintable(dialog.categories().join(", ")));
    CHECK(dialog.currentCategory() == QStringLiteral("Appearance"), "opens on the first page");
    CHECK(dialog.showCategory(QStringLiteral("My Plugin")) && !dialog.showCategory(QStringLiteral("Viewport")),
          "pages can be picked by name, and categories without settings have none");

    auto* level = withKey<QSpinBox>(&dialog, QStringLiteral("Plugin/level"));
    auto* levelReset = withKey<QToolButton>(&dialog, QStringLiteral("Plugin/level"), QStringLiteral("settingReset"));
    CHECK(level && levelReset, "an int setting gets a spin box and a reset button");
    if (!level || !levelReset) return;
    CHECK(level->value() == 3 && level->minimum() == 1 && level->maximum() == 10, "with its value and range");
    CHECK(levelReset->isHidden(), "reset is hidden while the value is the default");
    level->setValue(7);
    CHECK(settings.get<int>("Plugin/level") == 7, "editing applies immediately");
    CHECK(!levelReset->isHidden(), "and shows reset");
    settings.setValue("Plugin/level", 5);
    CHECK(level->value() == 5, "the editor follows changes made elsewhere");
    levelReset->click();
    CHECK(settings.get<int>("Plugin/level") == 3 && level->value() == 3 && levelReset->isHidden(), "reset works");

    // Typing a value and pressing Enter commits it and keeps the dialog open.
    dialog.show();
    settle();
    level->setFocus();
    level->selectAll();
    QTest::keyClicks(level, QStringLiteral("9"));
    QTest::keyClick(level, Qt::Key_Return);
    QApplication::processEvents();
    CHECK(settings.get<int>("Plugin/level") == 9, "Enter commits the typed value (got %d)",
          settings.get<int>("Plugin/level"));
    CHECK(dialog.isVisible(), "and doesn't close the dialog");
    QTest::keyClick(level, Qt::Key_Enter, Qt::KeypadModifier);
    CHECK(dialog.isVisible(), "nor does the keypad's Enter");
    settings.reset("Plugin/level");

    auto* accent = withKey<QToolButton>(&dialog, QStringLiteral("Appearance/accent"), QStringLiteral("settingColorButton"));
    CHECK(accent && accent->text() == QStringLiteral("#3d8bfd"), "a color gets a swatch button with its value");
    settings.setValue("Appearance/accent", std::string("#00ff00"));
    CHECK(accent && accent->text() == QStringLiteral("#00ff00"), "which follows the setting");

    auto* toolA = withKey<QKeySequenceEdit>(&dialog, QStringLiteral("Shortcuts/a"));
    auto* conflictA = withKey<QLabel>(&dialog, QStringLiteral("Shortcuts/a"), QStringLiteral("settingConflict"));
    auto* conflictB = withKey<QLabel>(&dialog, QStringLiteral("Shortcuts/b"), QStringLiteral("settingConflict"));
    CHECK(toolA && conflictA && conflictB, "a shortcut gets a key sequence editor and a conflict note");
    if (!toolA || !conflictA || !conflictB) return;
    CHECK(toolA->keySequence() == QKeySequence(Qt::Key_W), "showing its key");
    CHECK(conflictA->isHidden() && conflictB->isHidden(), "no conflicts at first");
    toolA->setKeySequence(QKeySequence(Qt::Key_E));
    CHECK(settings.get<std::string>("Shortcuts/a") == "E", "a new key is stored as its portable text");
    CHECK(!conflictA->isHidden() && conflictA->text().contains(QStringLiteral("Tool B")) && !conflictB->isHidden(),
          "both sides of a conflict are flagged: \"%s\"", qPrintable(conflictA->text()));
    dialog.showCategory(QStringLiteral("Shortcuts"));
    for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Reset Page to Defaults")) button->click();
    }
    CHECK(settings.get<std::string>("Shortcuts/a") == "W" && conflictA->isHidden(), "Reset Page resets the page");

    // A plugin defining a setting while the dialog is open gets a page.
    settings.define({.key = "Other/on", .category = "Other", .label = "On", .type = SettingType::Bool,
                     .defaultValue = true});
    settle();
    CHECK(dialog.categories().contains(QStringLiteral("Other")), "a new category appears");
    CHECK(dialog.currentCategory() == QStringLiteral("Shortcuts"), "and the current page stays");
    auto* on = withKey<QCheckBox>(&dialog, QStringLiteral("Other/on"));
    CHECK(on && on->isChecked(), "a bool gets a check box");
    if (on) on->setChecked(false);
    CHECK(!settings.get<bool>("Other/on", true), "which applies immediately");

    // A folder: text to edit plus a Browse button, and the "next start" note.
    settings.define({.key = "Other/folder", .category = "Other", .label = "Folder", .type = SettingType::Folder,
                     .defaultValue = std::string("C:/a"), .appliesAtNextStart = true});
    settle();
    auto* folder = withKey<QWidget>(&dialog, QStringLiteral("Other/folder"), QStringLiteral("settingEditor"));
    auto* folderEdit = folder ? folder->findChild<QLineEdit*>() : nullptr;
    auto* browse = folder ? folder->findChild<QToolButton*>(QStringLiteral("settingBrowse")) : nullptr;
    auto* nextStart = withKey<QLabel>(&dialog, QStringLiteral("Other/folder"), QStringLiteral("settingNextStart"));
    CHECK(folderEdit && browse && nextStart, "a folder gets a field, a Browse button and a note");
    if (!folderEdit || !nextStart) return;
    CHECK(folderEdit->text() == QStringLiteral("C:/a"), "showing its value");
    CHECK(nextStart->isHidden(), "no note while it's what this session started with");
    folderEdit->setText(QStringLiteral("C:/b"));
    Q_EMIT folderEdit->editingFinished();
    CHECK(settings.get<std::string>("Other/folder") == "C:/b", "editing stores it");
    CHECK(!nextStart->isHidden(), "and says when it takes effect");
    settings.reset("Other/folder");
    CHECK(nextStart->isHidden() && folderEdit->text() == QStringLiteral("C:/a"), "reset takes both back");
}

USDCC_TEST(settings_tool_shortcuts) {
    TestSettings testSettings;
    QTemporaryDir temp;
    usdcc::ui::UiScaleController scale(application(), temp.filePath(QStringLiteral("ui.ini")));
    usdcc::usd::StageManager stages;
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    QMainWindow window;
    auto* field = new QLineEdit;
    window.setCentralWidget(field);
    new usdcc::usd::ViewportToolBars(&window, toolSet, &stages);
    window.show();
    window.activateWindow();
    settle();

    auto& settings = Settings::instance();
    CHECK(settings.get<std::string>(usdcc::usd::toolShortcutKey("select")) == "Q" &&
              settings.get<std::string>(usdcc::usd::toolShortcutKey("move")) == "W" &&
              settings.get<std::string>(usdcc::usd::toolShortcutKey("rotate")) == "E" &&
              settings.get<std::string>(usdcc::usd::toolShortcutKey("scale")) == "R",
          "the tool shortcuts default to Q, W, E, R");

    auto* rail = window.findChild<usdcc::usd::ToolRail*>();
    QAction* move = nullptr;
    for (QAction* action : rail ? rail->actions() : QList<QAction*>()) {
        if (action->data().toString() == QStringLiteral("move")) move = action;
    }
    CHECK(move && move->shortcut() == QKeySequence(Qt::Key_W), "the rail's Move action has W");
    CHECK(move && move->toolTip() == QStringLiteral("Move (W)"), "and says so in its tooltip");

    window.setFocus();
    field->clearFocus();
    type(&window, Qt::Key_W);
    CHECK(toolSet->activeId() == "move", "W picks Move");
    type(&window, Qt::Key_E);
    CHECK(toolSet->activeId() == "rotate", "E picks Rotate");
    type(&window, Qt::Key_R);
    CHECK(toolSet->activeId() == "scale", "R picks Scale");
    type(&window, Qt::Key_Q);
    CHECK(toolSet->activeId() == "select", "Q picks Select");

    field->setFocus();
    settle();
    type(&window, Qt::Key_W);
    CHECK(field->text() == QStringLiteral("w") && toolSet->activeId() == "select",
          "in a text field W types a letter instead (text \"%s\")", qPrintable(field->text()));
    field->clearFocus();
    window.setFocus();

    settings.setValue(usdcc::usd::toolShortcutKey("move"), std::string("T"));
    CHECK(move && move->shortcut() == QKeySequence(Qt::Key_T) && move->toolTip() == QStringLiteral("Move (T)"),
          "a new key rebinds the action and its tooltip");
    type(&window, Qt::Key_W);
    CHECK(toolSet->activeId() == "select", "W no longer picks Move");
    type(&window, Qt::Key_T);
    CHECK(toolSet->activeId() == "move", "T does");

    // A plugin's tool gets a Shortcut setting too, with no key until one is set.
    toolSet->addTool("measure", "Measure", "", std::make_unique<usdcc::tools::SelectTool>());
    settle();
    const usdcc::core::SettingDefinition* measure = settings.definition(usdcc::usd::toolShortcutKey("measure"));
    CHECK(measure && measure->label == "Measure tool" && settings.get<std::string>(measure->key).empty(),
          "a plugin tool's shortcut setting, empty at first");
    CHECK(settings.get<std::string>(usdcc::usd::toolShortcutKey("move")) == "T", "the rebuilt rail keeps Move on T");
    settings.setValue(usdcc::usd::toolShortcutKey("measure"), std::string("M"));
    type(&window, Qt::Key_M);
    CHECK(toolSet->activeId() == "measure", "M picks the plugin's tool");
    type(&window, Qt::Key_T);
    CHECK(toolSet->activeId() == "move", "and T still picks Move after the rail was rebuilt");
}
