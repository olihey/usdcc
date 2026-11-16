// Edit > Selection (docs/SELECTION_MENU.md): the menu's entries, what they select, and the
// By Name dialog. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/usd/select_by_name_dialog.h"
#include "usdcc/usd/selection_menu.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

#include <QAction>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QWidget>

#include <string>
#include <vector>

using usdcc::usd::SelectByNameDialog;
using usdcc::usd::SelectionMenu;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// The entry's label, without its & or the count after the tab.
QAction* entry(QMenu* menu, const QString& text) {
    if (!menu) return nullptr;
    for (QAction* action : menu->actions()) {
        if (action->text().section(QLatin1Char('\t'), 0, 0).remove(QLatin1Char('&')) == text) return action;
    }
    return nullptr;
}
QMenu* submenu(QMenu* menu, const QString& text) {
    QAction* action = entry(menu, text);
    return action ? action->menu() : nullptr;
}

std::string joined(const std::vector<SdfPath>& paths) {
    std::string text;
    for (const SdfPath& path : paths) text += (text.empty() ? "" : " ") + path.GetString();
    return text;
}

StageRefPtr fill(StageManager& manager) {
    const StageRefPtr stage = manager.createStage();
    const UsdStageRefPtr usd = stage->usdStage();
    usd->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    usd->DefinePrim(SdfPath("/World/Cube"), TfToken("Cube"));
    usd->DefinePrim(SdfPath("/World/Ball"), TfToken("Sphere"));
    usd->DefinePrim(SdfPath("/World/ball_2"), TfToken("Sphere"));
    UsdShadeMaterialBindingAPI::Apply(usd->GetPrimAtPath(SdfPath("/World/Cube")));
    UsdCollectionAPI::Apply(usd->GetPrimAtPath(SdfPath("/World")), TfToken("ferns"));
    manager.setCurrentStage(stage);
    return stage;
}

}  // namespace

USDCC_TEST(selection_menu_greyed_with_no_stage) {
    StageManager manager;
    QWidget window;
    SelectionMenu menu(&manager, &window);
    menu.rebuild();
    for (const char* name : {"None", "All", "Invert", "By Type", "By Schema", "By Name..."}) {
        QAction* action = entry(&menu, QString::fromLatin1(name));
        CHECK(action && !action->isEnabled(), "%s is there, greyed", name);
    }
}

USDCC_TEST(selection_menu_none_all_invert) {
    StageManager manager;
    QWidget window;
    const StageRefPtr stage = fill(manager);
    SelectionMenu menu(&manager, &window);
    menu.rebuild();
    CHECK(!entry(&menu, QStringLiteral("None"))->isEnabled(), "None greyed with nothing selected");
    entry(&menu, QStringLiteral("All"))->trigger();
    CHECK(joined(manager.selectedPaths(stage)) == "/World /World/Cube /World/Ball /World/ball_2", "All: [%s]",
          joined(manager.selectedPaths(stage)).c_str());

    manager.setSelectedPaths(stage, {SdfPath("/World/Cube")});
    menu.rebuild();
    entry(&menu, QStringLiteral("Invert"))->trigger();
    CHECK(joined(manager.selectedPaths(stage)) == "/World /World/Ball /World/ball_2", "Invert: [%s]",
          joined(manager.selectedPaths(stage)).c_str());
    menu.rebuild();
    QAction* none = entry(&menu, QStringLiteral("None"));
    CHECK(none->isEnabled(), "None with a selection");
    none->trigger();
    CHECK(manager.selectedPaths(stage).empty(), "None clears it");
}

USDCC_TEST(selection_menu_by_type_and_schema) {
    StageManager manager;
    QWidget window;
    const StageRefPtr stage = fill(manager);
    SelectionMenu menu(&manager, &window);
    menu.rebuild();

    QMenu* byType = submenu(&menu, QStringLiteral("By Type"));
    QAction* sphere = entry(submenu(byType, QStringLiteral("Geometry")), QStringLiteral("Sphere"));
    CHECK(sphere && sphere->isEnabled() && sphere->text().endsWith(QStringLiteral("\t2")),
          "Geometry > Sphere, as the Create menu groups it, with its count: [%s]", sphere ? qPrintable(sphere->text()) : "");
    QAction* cone = entry(submenu(byType, QStringLiteral("Geometry")), QStringLiteral("Cone"));
    CHECK(cone && !cone->isEnabled() && !cone->text().contains(QLatin1Char('\t')), "a type nothing has is listed, greyed");
    CHECK(entry(byType, QStringLiteral("Xform")) && entry(byType, QStringLiteral("Prim (no type)")), "the top level too");
    if (sphere) sphere->trigger();
    CHECK(joined(manager.selectedPaths(stage)) == "/World/Ball /World/ball_2", "selects the spheres");

    QMenu* bySchema = submenu(&menu, QStringLiteral("By Schema"));
    QAction* binding = nullptr;
    QAction* collection = nullptr;
    for (QAction* category : bySchema->actions()) {
        if (!binding) binding = entry(category->menu(), QStringLiteral("MaterialBindingAPI"));
        if (!collection) collection = entry(category->menu(), QStringLiteral("CollectionAPI"));
    }
    CHECK(binding && binding->isEnabled() && collection && collection->isEnabled(), "schemas, in their categories");
    if (collection) collection->trigger();
    CHECK(joined(manager.selectedPaths(stage)) == "/World", "selects the prims with it: [%s]",
          joined(manager.selectedPaths(stage)).c_str());
}

USDCC_TEST(select_by_name_dialog_matches) {
    StageManager manager;
    const StageRefPtr stage = fill(manager);
    SelectByNameDialog dialog(stage->usdStage());
    dialog.patternField()->setText(QString());
    CHECK(!dialog.selectButton()->isEnabled(), "nothing to select with no text");

    dialog.containsButton()->setChecked(true);
    dialog.matchCaseCheck()->setChecked(false);
    dialog.patternField()->setText(QStringLiteral("ball"));
    CHECK(joined(dialog.matches()) == "/World/Ball /World/ball_2", "contains, ignoring case: [%s]", joined(dialog.matches()).c_str());
    CHECK(dialog.statusLabel()->text() == QStringLiteral("2 prims match"), "says how many: [%s]", qPrintable(dialog.statusLabel()->text()));
    CHECK(dialog.selectButton()->isEnabled(), "Select works");
    dialog.matchCaseCheck()->setChecked(true);
    CHECK(joined(dialog.matches()) == "/World/ball_2", "matching case");
    dialog.patternField()->setText(QStringLiteral("."));
    CHECK(dialog.matches().empty() && !dialog.selectButton()->isEnabled(), "a dot is just a dot here");

    dialog.regexButton()->setChecked(true);
    dialog.matchCaseCheck()->setChecked(false);
    dialog.patternField()->setText(QStringLiteral("^ball_\\d+$"));
    CHECK(joined(dialog.matches()) == "/World/ball_2", "a regular expression: [%s]", joined(dialog.matches()).c_str());
    dialog.patternField()->setText(QStringLiteral("c|w"));
    CHECK(joined(dialog.matches()) == "/World /World/Cube", "anywhere in the name: [%s]", joined(dialog.matches()).c_str());
    dialog.patternField()->setText(QStringLiteral("("));
    CHECK(dialog.matches().empty() && !dialog.selectButton()->isEnabled() &&
              dialog.statusLabel()->text().startsWith(QStringLiteral("Not a valid regular expression")),
          "an invalid one says so: [%s]", qPrintable(dialog.statusLabel()->text()));

    QString error;
    CHECK(SelectByNameDialog::matching(stage->usdStage(), QStringLiteral("cube"), SelectByNameDialog::Mode::Contains, false, &error)
                  .size() == 1 && error.isEmpty(),
          "matching() without the dialog");
}
