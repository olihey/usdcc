// Prims as USDA text for Send to… (docs/PEERS.md §10.3): what the selection writes out, and
// pasting it into another stage as one undo step. Runs in usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/share/prim_text.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>

#include <QUndoStack>

using usdcc::usd::Stage;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::share::pasteUsda;
using usdcc::usd::share::primsAsUsda;
PXR_NAMESPACE_USING_DIRECTIVE

USDCC_TEST(prim_text_round_trip) {
    // The sender's stage: a prim that gets its child from a reference, as composition does.
    const SdfLayerRefPtr asset = SdfLayer::CreateAnonymous("asset.usda");
    CHECK(asset->ImportFromString("#usda 1.0\ndef Xform \"Asset\"\n{\n    def Sphere \"Ball\"\n    {\n"
                                  "        double radius = 3\n    }\n}\n"),
          "the asset's text didn't parse");
    const UsdStageRefPtr from = UsdStage::CreateInMemory();
    from->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    UsdPrim hero = from->DefinePrim(SdfPath("/World/Hero"), TfToken("Xform"));
    hero.GetReferences().AddReference(asset->GetIdentifier(), SdfPath("/Asset"));
    from->DefinePrim(SdfPath("/World/Lamp"), TfToken("SphereLight"));
    from->DefinePrim(SdfPath("/Other"), TfToken("Cube"));
    const StageRefPtr sender = std::make_shared<Stage>(from);

    QString error;
    // /World/Hero/Ball is under /World/Hero: it comes with it, not twice.
    const QString usda =
        primsAsUsda(sender, {SdfPath("/World/Hero"), SdfPath("/World/Hero/Ball"), SdfPath("/World/Lamp")}, &error);
    CHECK(!usda.isEmpty(), "nothing written: %s", qPrintable(error));
    CHECK(usda.contains("radius = 3") && usda.contains("over \"World\"") && !usda.contains("Other"),
          "the text isn't the flattened selection:\n%s", qPrintable(usda));
    CHECK(primsAsUsda(sender, {SdfPath("/Nope")}, &error).isEmpty() && !error.isEmpty(), "a missing prim wrote text");

    // The receiver's stage has a /World/Hero already: the pasted one is Hero_1.
    const UsdStageRefPtr to = UsdStage::CreateInMemory();
    to->DefinePrim(SdfPath("/World/Hero"), TfToken("Cube"));
    const StageRefPtr receiver = std::make_shared<Stage>(to);
    UndoManager undo;
    const std::string before = [&] {
        std::string text;
        to->GetRootLayer()->ExportToString(&text);
        return text;
    }();
    const std::vector<SdfPath> pasted = pasteUsda(receiver, undo, usda, &error);
    CHECK(pasted == std::vector<SdfPath>({SdfPath("/World/Hero_1"), SdfPath("/World/Lamp")}), "pasted to %zu paths: %s",
          pasted.size(), qPrintable(error));
    double radius = 0;
    const UsdPrim ball = to->GetPrimAtPath(SdfPath("/World/Hero_1/Ball"));
    CHECK(ball && ball.GetAttribute(TfToken("radius")).Get(&radius) && radius == 3,
          "the referenced child didn't come along");
    CHECK(to->GetPrimAtPath(SdfPath("/World/Hero")).GetTypeName() == "Cube", "the receiver's own prim changed");
    QUndoStack* stack = undo.stack(receiver);
    CHECK(stack && stack->count() == 1 && stack->text(0) == "Paste 2 prims", "not one undo step");
    if (stack) {
        stack->undo();
        std::string after;
        to->GetRootLayer()->ExportToString(&after);
        CHECK(after == before && !to->GetPrimAtPath(SdfPath("/World/Lamp")), "undo didn't take the paste back");
    }
    CHECK(pasteUsda(receiver, undo, "not usda at all", &error).empty() && !error.isEmpty(), "junk was pasted");
}
