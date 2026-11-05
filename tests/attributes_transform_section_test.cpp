// The Attribute Editor's Transform section is there for every xformable prim, with
// no ops too, as its + is how they're added. Runs in usdcc_ui_tests (Qt's offscreen
// platform).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUndoStack>

#include <memory>

using usdcc::usd::AttributesViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

// Transform's +, by its tooltip: a rebuild replaces it, so look each time.
QToolButton* addButton(AttributesViewPanel& panel) {
    for (QToolButton* button : panel.findChildren<QToolButton*>()) {
        if (button->toolTip() == QStringLiteral("Add an op to the stack")) return button;
    }
    return nullptr;
}

bool emptyNoteVisible(AttributesViewPanel& panel) {
    for (QLabel* label : panel.findChildren<QLabel*>()) {
        if (label->text().startsWith(QStringLiteral("No transform ops")) && label->isVisible()) return true;
    }
    return false;
}

QAction* entry(QMenu* menu, const QString& text) {
    for (QAction* action : menu->actions()) {
        if (QString(action->text()).remove(QLatin1Char('&')) == text) return action;
    }
    return nullptr;
}
}  // namespace

USDCC_TEST(attributes_transform_section_without_ops) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    const UsdPrim xform = s->DefinePrim(SdfPath("/Bare"), TfToken("Xform"));
    s->DefinePrim(SdfPath("/Cube"), TfToken("Cube"));
    s->DefinePrim(SdfPath("/Scope"), TfToken("Scope"));
    s->DefinePrim(SdfPath("/Typeless"));

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.show();
    settle();

    stages.setSelectedPaths(stage, {SdfPath("/Bare")});
    settle();
    QToolButton* add = addButton(panel);
    CHECK(add && add->isVisible() && add->isEnabled(), "an Xform with no ops has Transform, with its +");
    CHECK(emptyNoteVisible(panel), "and says how to add one");

    stages.setSelectedPaths(stage, {SdfPath("/Cube")});
    settle();
    add = addButton(panel);
    CHECK(add && add->isVisible(), "so does any other xformable prim");

    for (const char* path : {"/Scope", "/Typeless"}) {
        stages.setSelectedPaths(stage, {SdfPath(path)});
        settle();
        CHECK(!addButton(panel), "a prim that can't have a transform doesn't: %s", path);
    }

    // The + does what it's there for.
    stages.setSelectedPaths(stage, {SdfPath("/Bare")});
    settle();
    add = addButton(panel);
    CHECK(add && add->menu(), "its menu");
    if (!add || !add->menu()) return;
    QAction* translate = entry(add->menu(), QStringLiteral("Translate"));
    CHECK(translate, "offers Translate");
    if (!translate) return;
    translate->trigger();
    settle();
    bool resets = false;
    CHECK(UsdGeomXformable(xform).GetOrderedXformOps(&resets).size() == 1, "which adds an op");
    CHECK(!emptyNoteVisible(panel), "and the note goes");
    undo->stack(stage)->undo();
    settle();
    CHECK(UsdGeomXformable(xform).GetOrderedXformOps(&resets).empty(), "undone");
    add = addButton(panel);
    CHECK(add && add->isVisible() && emptyNoteVisible(panel), "the section stays, empty again");

    // A search still hides a section with nothing to show, and clearing it brings it back.
    QLineEdit* search = nullptr;
    for (QLineEdit* edit : panel.findChildren<QLineEdit*>()) {
        if (edit->placeholderText() == QStringLiteral("Search attributes")) search = edit;
    }
    CHECK(search, "the search field");
    if (!search) return;
    search->setText(QStringLiteral("visibility"));
    settle();
    CHECK(!addButton(panel)->isVisible(), "a search hides the empty section");
    search->clear();
    settle();
    CHECK(addButton(panel)->isVisible(), "and it comes back");
}
