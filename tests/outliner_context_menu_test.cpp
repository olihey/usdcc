// The Outliner's context menu (docs/OUTLINER_CONTEXT_MENU.md): each prim
// action and its undo and redo (prim_actions.h), the menu's entries for
// different prims and what they do (prim_context_menu.h), instance proxies
// refused, and Prim Info. Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QMenu>
#include <QUndoStack>

using usdcc::usd::PrimMenuContext;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
namespace prim_actions = usdcc::usd::prim_actions;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// /World/A, /World/B (cubes), /World/Group (invisible) with /World/Group/C,
// /Looks with a "look" variant set (red, blue), /Payloaded with a payload of
// a cube, and /inst, an instance of /src.
struct TestStage {
    StageManager manager;
    UndoManager undo;
    SdfLayerRefPtr asset = SdfLayer::CreateAnonymous("asset.usda");
    StageRefPtr stage = manager.createStage();

    TestStage() {
        asset->ImportFromString("#usda 1.0\ndef Xform \"Asset\"\n{\n    def Cube \"body\"\n    {\n    }\n}\n");
        const UsdStageRefPtr& s = stage->usdStage();
        s->DefinePrim(SdfPath("/World"), TfToken("Xform"));
        s->DefinePrim(SdfPath("/World/A"), TfToken("Cube"));
        s->DefinePrim(SdfPath("/World/B"), TfToken("Cube"));
        UsdGeomImageable(s->DefinePrim(SdfPath("/World/Group"), TfToken("Xform"))).MakeInvisible();
        s->DefinePrim(SdfPath("/World/Group/C"), TfToken("Cube"));
        UsdVariantSet look = s->DefinePrim(SdfPath("/Looks"), TfToken("Xform")).GetVariantSets().AddVariantSet("look");
        look.AddVariant("red");
        look.AddVariant("blue");
        look.SetVariantSelection("red");
        s->DefinePrim(SdfPath("/Payloaded"), TfToken("Xform")).GetPayloads().AddPayload(asset->GetIdentifier(),
                                                                                       SdfPath("/Asset"));
        s->DefinePrim(SdfPath("/src"), TfToken("Xform"));
        s->DefinePrim(SdfPath("/src/geo"), TfToken("Cube"));
        const UsdPrim inst = s->DefinePrim(SdfPath("/inst"), TfToken("Xform"));
        inst.GetReferences().AddInternalReference(SdfPath("/src"));
        inst.SetInstanceable(true);
    }

    UsdPrim prim(const char* path) const { return stage->usdStage()->GetPrimAtPath(SdfPath(path)); }
    bool visible(const char* path) const {
        return UsdGeomImageable(prim(path)).ComputeVisibility() != UsdGeomTokens->invisible;
    }
    QUndoStack* stack() { return undo.stack(stage); }
};

QAction* findAction(QMenu* menu, const QString& text) {
    for (QAction* action : menu->actions()) {
        if (action->text() == text) return action;
        if (action->menu()) {
            if (QAction* found = findAction(action->menu(), text)) return found;
        }
    }
    return nullptr;
}

PrimMenuContext contextFor(TestStage& test, const char* clicked, std::vector<SdfPath> paths = {}) {
    PrimMenuContext context;
    context.undo = &test.undo;
    context.stage = test.stage;
    context.clicked = SdfPath(clicked);
    context.paths = paths.empty() ? std::vector{SdfPath(clicked)} : paths;
    return context;
}
}  // namespace

USDCC_TEST(prim_actions_active_and_rename) {
    TestStage test;
    const std::vector<SdfPath> ab{SdfPath("/World/A"), SdfPath("/World/B")};
    CHECK(prim_actions::setActive(test.undo, test.stage, ab, false), "deactivate two prims");
    CHECK(!test.prim("/World/A").IsActive() && !test.prim("/World/B").IsActive() && test.stack()->count() == 1 &&
              test.stack()->undoText() == "Deactivate 2 Prims",
          "both inactive, one step: [%s]", qPrintable(test.stack()->undoText()));
    test.stack()->undo();
    CHECK(test.prim("/World/A").IsActive() && test.prim("/World/B").IsActive(), "undo activates them again");
    CHECK(!prim_actions::setActive(test.undo, test.stage, ab, true), "nothing to do: no step");

    CHECK(prim_actions::rename(test.undo, test.stage, SdfPath("/World/A"), "Apple") && test.prim("/World/Apple") &&
              !test.prim("/World/A"),
          "rename");
    test.stack()->undo();
    CHECK(test.prim("/World/A") && !test.prim("/World/Apple"), "undo renames it back");
    CHECK(!prim_actions::rename(test.undo, test.stage, SdfPath("/World/A"), "not valid"), "an invalid name is refused");
}

USDCC_TEST(prim_actions_visibility) {
    TestStage test;
    CHECK(!test.visible("/World/Group/C"), "C starts hidden by its group");
    CHECK(prim_actions::setVisible(test.undo, test.stage, {SdfPath("/World/Group/C")}, true) &&
              test.visible("/World/Group/C"),
          "Visible shows it, under its invisible group");
    test.stack()->undo();
    CHECK(!test.visible("/World/Group/C"), "undo hides it again");

    CHECK(prim_actions::showOnly(test.undo, test.stage, {SdfPath("/World/A")}), "Show Only This");
    CHECK(test.visible("/World/A") && !test.visible("/World/B") && !test.visible("/Looks") && test.visible("/World"),
          "A and its ancestors show; B and /Looks don't");
    test.stack()->undo();
    CHECK(test.visible("/World/B") && test.visible("/Looks"), "undo shows them again");

    CHECK(prim_actions::resetVisibility(test.undo, test.stage, {SdfPath("/World/Group")}) && test.visible("/World/Group/C"),
          "Reset clears the group's invisible");
    CHECK(!prim_actions::resetVisibility(test.undo, test.stage, {SdfPath("/World/A")}), "nothing authored: no step");
}

USDCC_TEST(prim_actions_variants_and_payloads) {
    TestStage test;
    auto selection = [&]() { return test.prim("/Looks").GetVariantSet("look").GetVariantSelection(); };
    CHECK(prim_actions::setVariant(test.undo, test.stage, SdfPath("/Looks"), "look", "blue") && selection() == "blue",
          "pick blue");
    test.stack()->undo();
    CHECK(selection() == "red", "undo: red again");
    CHECK(prim_actions::clearVariant(test.undo, test.stage, SdfPath("/Looks"), "look") && selection().empty(),
          "clear the selection");

    const SdfPath payloaded("/Payloaded");
    CHECK(prim_actions::hasPayloads(test.stage, payloaded) && prim_actions::canUnload(test.stage, payloaded) &&
              test.prim("/Payloaded/body"),
          "the payload is loaded");
    CHECK(prim_actions::unload(test.undo, test.stage, {payloaded}) && !test.prim("/Payloaded/body") &&
              prim_actions::canLoad(test.stage, payloaded),
          "unload");
    test.stack()->undo();
    CHECK(test.prim("/Payloaded/body"), "undo loads it again");
    test.stack()->redo();
    CHECK(!test.prim("/Payloaded/body"), "redo unloads it");
    CHECK(prim_actions::load(test.undo, test.stage, {payloaded}) && test.prim("/Payloaded/body"), "load");
    CHECK(!prim_actions::hasPayloads(test.stage, SdfPath("/World")), "no payloads under /World");
}

USDCC_TEST(prim_actions_instance_proxies_and_info) {
    TestStage test;
    const SdfPath proxy("/inst/geo");
    CHECK(test.prim("/inst/geo").IsInstanceProxy() && !prim_actions::isEditable(test.prim("/inst/geo")), "a proxy");
    CHECK(!prim_actions::setActive(test.undo, test.stage, {proxy}, false) &&
              !prim_actions::setVisible(test.undo, test.stage, {proxy}, false) && test.stack()->count() == 0,
          "edits on it are refused");

    const QString looks = prim_actions::infoText(test.prim("/Looks"));
    CHECK(looks.contains("Path: /Looks") && looks.contains("Type: Xform") && looks.contains("Variants: look = red") &&
              looks.contains("Specifier: def"),
          "info: [%s]", qPrintable(looks));
    const QString inst = prim_actions::infoText(test.prim("/inst"));
    CHECK(inst.contains("1 references") && inst.contains("instance of"), "info: [%s]", qPrintable(inst));
}

USDCC_TEST(prim_context_menu_entries) {
    TestStage test;
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, contextFor(test, "/World/A"));
        CHECK(findAction(&menu, "A  ·  Cube") && findAction(&menu, "Active") && findAction(&menu, "Visibility") &&
                  findAction(&menu, "Copy Path") && findAction(&menu, "Prim Info..."),
              "a cube's menu");
        CHECK(!findAction(&menu, "Load Payload") && !findAction(&menu, "Variants") && !findAction(&menu, "Move Up"),
              "without payloads, variants or a move callback");
        QAction* active = findAction(&menu, "Active");
        CHECK(active->isCheckable() && active->isChecked(), "checked: it's active");
        active->trigger();
        CHECK(!test.prim("/World/A").IsActive(), "Active deactivates it");
        findAction(&menu, "Copy Path")->trigger();
        CHECK(QApplication::clipboard()->text() == "/World/A", "Copy Path: [%s]",
              qPrintable(QApplication::clipboard()->text()));
    }
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, contextFor(test, "/Looks"));
        QAction* red = findAction(&menu, "red");
        QAction* blue = findAction(&menu, "blue");
        CHECK(findAction(&menu, "Variants") && findAction(&menu, "look") && red && red->isChecked() && blue &&
                  !blue->isChecked(),
              "Variants > look, red checked");
        blue->trigger();
        CHECK(test.prim("/Looks").GetVariantSet("look").GetVariantSelection() == "blue", "picking blue selects it");
    }
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, contextFor(test, "/Payloaded"));
        QAction* load = findAction(&menu, "Load Payload");
        QAction* unload = findAction(&menu, "Unload Payload");
        CHECK(load && unload && !load->isEnabled() && unload->isEnabled(), "loaded: Unload is the one enabled");
        unload->trigger();
        CHECK(!test.prim("/Payloaded/body"), "Unload Payload unloads it");
    }
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, contextFor(test, "/inst/geo"));
        CHECK(!findAction(&menu, "Active")->isEnabled() && !findAction(&menu, "Visible")->isEnabled() &&
                  findAction(&menu, "Prim Info...")->isEnabled(),
              "an instance proxy: edits disabled, info works");
    }
    {
        QMenu menu;
        int moved = 0;
        PrimMenuContext context = contextFor(test, "/World/B", {SdfPath("/World/A"), SdfPath("/World/B")});
        context.move = [&](int direction) { moved = direction; };
        usdcc::usd::populatePrimMenu(&menu, context);
        CHECK(findAction(&menu, "2 Prims") && findAction(&menu, "Move Up") && !findAction(&menu, "Move Up")->isEnabled(),
              "two prims: the title says so, and Move needs one");
    }
}

USDCC_TEST(prim_context_menu_composition) {
    TestStage test;
    QString opened;
    usdcc::usd::setOpenStageHandler([&](const QString& path) { opened = path; });
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, contextFor(test, "/inst"));
        QAction* reference = findAction(&menu, "Reference   (this file)  </src>");
        CHECK(findAction(&menu, "Composition") && reference, "Composition lists the internal reference");
        CHECK(findAction(&menu, "Asset:  (this file)") && !findAction(&menu, "Asset:  (this file)")->isEnabled(),
              "its details, read-only");
        CHECK(findAction(&menu, "Open as Stage") && !findAction(&menu, "Open as Stage")->isEnabled(),
              "nothing to open for this file");
    }
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, contextFor(test, "/Payloaded"));
        QAction* open = findAction(&menu, "Open as Stage");
        CHECK(open && open->isEnabled(), "a payload's file can be opened as a stage");
        if (open) open->trigger();
        CHECK(opened == QString::fromStdString(test.asset->GetIdentifier()), "Open as Stage passes the file: [%s]",
              qPrintable(opened));
        CHECK(!findAction(&menu, "Show in Attribute Editor"), "no Show in Attribute Editor");
    }
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, contextFor(test, "/Looks"));
        CHECK(findAction(&menu, "Variant   look = red"), "the variant set, with its selection");
    }
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, contextFor(test, "/World/A"));
        CHECK(!findAction(&menu, "Composition"), "no Composition for a prim without arcs");
    }
    usdcc::usd::setOpenStageHandler({});
}

USDCC_TEST(prim_info_dialog) {
    TestStage test;
    QWidget* dialog = usdcc::usd::showPrimInfo(test.prim("/Looks"), nullptr);
    bool path = false;
    for (QLabel* label : dialog->findChildren<QLabel*>()) path = path || label->text() == "/Looks";
    CHECK(dialog->isVisible() && path && dialog->windowTitle().contains("Looks"), "the dialog shows the prim");
    dialog->close();
}
