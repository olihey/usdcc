// Deleting prims (docs/PRIM_DELETE.md): what the edit target can remove, deactivating
// what it can't, removing from every layer, what points at a deleted prim, the default
// prim and child order, each as one undo step. Runs in usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>

#include <QUndoStack>

#include <memory>

using usdcc::usd::Stage;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
namespace prim_actions = usdcc::usd::prim_actions;
using prim_actions::DeleteMode;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// The root layer, the edit target, over a weaker sublayer.
struct Scene {
    SdfLayerRefPtr weak = SdfLayer::CreateAnonymous("weak.usda");
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    StageRefPtr stage = std::make_shared<Stage>(usd);
    UndoManager undo;
    SdfLayerHandle root = usd->GetRootLayer();
    Scene() { root->GetSubLayerPaths().push_back(weak->GetIdentifier()); }
    UsdPrim define(const char* path, const char* type = "Xform") { return usd->DefinePrim(SdfPath(path), TfToken(type)); }
    UsdPrim prim(const char* path) const { return usd->GetPrimAtPath(SdfPath(path)); }
    bool has(const char* path) const { return static_cast<bool>(prim(path)); }
    // A def in the weaker layer, and an override in the root layer.
    void splitAcrossLayers(const char* path) {
        usd->SetEditTarget(weak);
        define(path, "Cube");
        usd->SetEditTarget(root);
        usd->OverridePrim(SdfPath(path)).CreateAttribute(TfToken("note"), SdfValueTypeNames->Int).Set(1);
    }
    QString remove(std::vector<const char*> paths, DeleteMode mode = DeleteMode::Deactivate) {
        std::vector<SdfPath> sdf;
        for (const char* path : paths) sdf.emplace_back(path);
        return prim_actions::deletePrims(undo, stage, sdf, mode);
    }
    prim_actions::DeletePlan plan(const char* path) const { return prim_actions::planDelete(usd, {SdfPath(path)}); }
    QUndoStack* stack() { return undo.stack(stage); }
    std::string file(const SdfLayerHandle& layer) const {
        std::string text;
        layer->ExportToString(&text);
        return text;
    }
};
}  // namespace

USDCC_TEST(prim_delete_only_in_the_edit_target_removes) {
    Scene s;
    s.define("/A");
    s.define("/A/B", "Cube");
    const std::string before = s.file(s.root);
    const prim_actions::DeletePlan plan = s.plan("/A");
    CHECK(plan.prims.size() == 1 && plan.prims.front().removable && plan.allRemovable(), "removable");
    CHECK(s.remove({"/A"}).isEmpty(), "deletes");
    CHECK(!s.has("/A") && !s.has("/A/B"), "with its subtree");
    CHECK(s.stack()->count() == 1 && s.stack()->text(0) == QStringLiteral("Delete A"), "one step, named");
    s.stack()->undo();
    CHECK(s.has("/A/B") && s.file(s.root) == before, "undo");
    s.stack()->redo();
    CHECK(!s.has("/A"), "redo");
}

// §3.1: RemovePrim would take the override and leave the prim.
USDCC_TEST(prim_delete_defined_in_a_weaker_layer_deactivates_keeping_overrides) {
    Scene s;
    s.splitAcrossLayers("/K");
    const std::string weak = s.file(s.weak);
    const prim_actions::DeletePlan plan = s.plan("/K");
    CHECK(plan.prims.size() == 1, "planned");
    const prim_actions::DeletePlan::Prim& k = plan.prims.front();
    CHECK(!k.removable && k.removableEverywhere && !k.fromArc && k.whyNotDeactivate.isEmpty(), "can be deactivated or removed everywhere");
    CHECK(k.otherLayers == std::vector<std::string>{s.weak->GetDisplayName()}, "names the other layer");
    CHECK(s.remove({"/K"}).isEmpty(), "done");
    CHECK(s.has("/K") && !s.prim("/K").IsActive(), "deactivated");
    CHECK(s.root->GetAttributeAtPath(SdfPath("/K.note")), "the override stays");
    CHECK(s.file(s.weak) == weak, "the weaker layer untouched");
    CHECK(s.stack()->text(0) == QStringLiteral("Deactivate K"), "named for what it did");
    s.stack()->undo();
    CHECK(s.prim("/K").IsActive(), "undo");
}

// §3.2: active = false isn't about the def's layer.
USDCC_TEST(prim_delete_a_weaker_edit_target_deactivates_a_stronger_def) {
    Scene s;
    s.define("/K", "Cube");  // in the root layer
    s.usd->SetEditTarget(s.weak);
    const prim_actions::DeletePlan plan = s.plan("/K");
    CHECK(!plan.prims.front().removable && plan.prims.front().whyNotDeactivate.isEmpty(), "deactivatable from the weaker layer");
    CHECK(s.remove({"/K"}).isEmpty(), "done");
    CHECK(s.has("/K") && !s.prim("/K").IsActive(), "inactive");
    CHECK(s.weak->GetPrimAtPath(SdfPath("/K")) && !s.root->GetPrimAtPath(SdfPath("/K"))->HasActive(), "authored in the weaker layer only");
}

USDCC_TEST(prim_delete_a_stronger_active_true_refuses) {
    Scene s;
    s.usd->SetEditTarget(s.weak);
    s.define("/K", "Cube");
    s.usd->SetEditTarget(s.root);
    s.usd->OverridePrim(SdfPath("/K")).SetActive(true);
    s.usd->SetEditTarget(s.weak);
    const prim_actions::DeletePlan plan = s.plan("/K");
    CHECK(!plan.prims.front().whyNotDeactivate.isEmpty(), "says why");
    const std::string weak = s.file(s.weak);
    CHECK(!s.remove({"/K"}).isEmpty(), "refused");
    CHECK(s.prim("/K").IsActive() && s.file(s.weak) == weak && s.stack()->count() == 0, "nothing changed");
    CHECK(s.remove({"/K"}, DeleteMode::RemoveEverywhere).isEmpty(), "but it can be removed everywhere");
    CHECK(!s.has("/K"), "gone");
}

USDCC_TEST(prim_delete_from_all_layers) {
    Scene s;
    s.splitAcrossLayers("/K");
    s.usd->SetEditTarget(s.weak);
    s.define("/K/Child");
    s.usd->SetEditTarget(s.root);
    const std::string weak = s.file(s.weak);
    const std::string root = s.file(s.root);
    CHECK(s.remove({"/K"}, DeleteMode::RemoveEverywhere).isEmpty(), "done");
    CHECK(!s.has("/K") && !s.weak->GetPrimAtPath(SdfPath("/K")) && !s.root->GetPrimAtPath(SdfPath("/K")), "gone from both layers");
    CHECK(s.stack()->count() == 1 && s.stack()->text(0) == QStringLiteral("Delete K"), "one step");
    s.stack()->undo();
    CHECK(s.has("/K/Child") && s.file(s.weak) == weak && s.file(s.root) == root, "undo: both layers as they were");
    s.stack()->redo();
    CHECK(!s.has("/K"), "redo");
}

USDCC_TEST(prim_delete_from_a_reference_only_deactivates) {
    Scene s;
    SdfLayerRefPtr asset = SdfLayer::CreateAnonymous("asset.usda");
    asset->ImportFromString("#usda 1.0\ndef Xform \"Asset\"\n{\n    def Cube \"body\"\n    {\n    }\n}\n");
    s.define("/Inst").GetReferences().AddReference(asset->GetIdentifier(), SdfPath("/Asset"));
    const prim_actions::DeletePlan plan = s.plan("/Inst/body");
    CHECK(plan.prims.front().fromArc && !plan.prims.front().removableEverywhere, "from an arc");
    CHECK(!plan.canRemoveEverywhere(), "can't be removed everywhere");
    CHECK(s.remove({"/Inst/body"}, DeleteMode::RemoveEverywhere).isEmpty(), "done");
    CHECK(s.has("/Inst/body") && !s.prim("/Inst/body").IsActive(), "deactivated instead");
    // The prim holding the reference is an ordinary delete.
    CHECK(s.plan("/Inst").prims.front().removable, "its holder is removable");
}

// An ancestor's variant holds it: the edit target can't remove that, as with a reference.
USDCC_TEST(prim_delete_inside_a_variant_only_deactivates) {
    Scene s;
    UsdVariantSet look = s.define("/A").GetVariantSets().AddVariantSet("look");
    look.AddVariant("red");
    look.SetVariantSelection("red");
    {
        UsdEditContext context(look.GetVariantEditContext());
        s.define("/A/B");
    }
    const prim_actions::DeletePlan plan = s.plan("/A/B");
    CHECK(!plan.prims.front().removable && plan.prims.front().fromArc && !plan.prims.front().removableEverywhere, "from a variant");
    CHECK(s.remove({"/A/B"}).isEmpty() && !s.prim("/A/B").IsActive(), "deactivated");
    CHECK(s.plan("/A").prims.front().removable, "the prim with the variant set is removable, variants and all");
}

USDCC_TEST(prim_delete_several_as_one_step) {
    Scene s;
    s.define("/A");
    s.define("/A/Under");
    s.usd->SetEditTarget(s.weak);
    s.define("/B");
    s.usd->SetEditTarget(s.root);
    const prim_actions::DeletePlan plan = prim_actions::planDelete(s.usd, {SdfPath("/A/Under"), SdfPath("/B"), SdfPath("/A")});
    // Composed, the weaker layer's root prims come first.
    std::vector<SdfPath> order;
    for (const UsdPrim& child : s.usd->GetPseudoRoot().GetChildren()) order.push_back(child.GetPath());
    CHECK(plan.prims.size() == 2 && order.size() == 2 && plan.prims[0].path == order[0] && plan.prims[1].path == order[1],
          "topmost, in tree order");
    CHECK(!plan.allRemovable() && plan.canRemoveEverywhere(), "mixed");
    CHECK(s.remove({"/A/Under", "/B", "/A"}).isEmpty(), "done");
    CHECK(!s.has("/A") && !s.prim("/B").IsActive(), "one removed, one deactivated");
    CHECK(s.stack()->count() == 1 && s.stack()->text(0) == QStringLiteral("Delete 2 Prims"), "one step");
    s.stack()->undo();
    CHECK(s.has("/A/Under") && s.prim("/B").IsActive(), "one undo");
}

USDCC_TEST(prim_delete_skips_instance_proxies) {
    Scene s;
    s.define("/src");
    s.define("/src/geo", "Cube");
    const UsdPrim inst = s.define("/inst");
    inst.GetReferences().AddInternalReference(SdfPath("/src"));
    inst.SetInstanceable(true);
    const prim_actions::DeletePlan plan = s.plan("/inst/geo");
    CHECK(plan.prims.empty() && plan.skipped.size() == 1, "skipped");
    CHECK(plan.skipped.front().second.contains(QStringLiteral("instance")), "says why");
    CHECK(!s.remove({"/inst/geo"}).isEmpty() && s.stack()->count() == 0, "refused");
    CHECK(s.remove({"/inst"}).isEmpty() && !s.has("/inst"), "the instance itself goes");
}

// §3.3: targets and connections into what's removed go, in the edit target.
USDCC_TEST(prim_delete_drops_what_points_at_it) {
    Scene s;
    s.define("/Mat");
    s.define("/Mat/Shader").CreateAttribute(TfToken("outputs:out"), SdfValueTypeNames->Float);
    s.define("/Keep");
    const UsdPrim geo = s.define("/Geo", "Cube");
    geo.CreateRelationship(TfToken("material:binding")).SetTargets({SdfPath("/Mat")});
    geo.CreateRelationship(TfToken("both")).SetTargets({SdfPath("/Mat"), SdfPath("/Keep")});
    geo.CreateAttribute(TfToken("inputs:x"), SdfValueTypeNames->Float).SetConnections({SdfPath("/Mat/Shader.outputs:out")});
    // A target the weaker layer authors.
    s.usd->SetEditTarget(s.weak);
    s.usd->OverridePrim(SdfPath("/Geo")).CreateRelationship(TfToken("fromWeak")).AddTarget(SdfPath("/Mat"));
    s.usd->SetEditTarget(s.root);
    const std::string weak = s.file(s.weak);
    const std::string root = s.file(s.root);

    CHECK(s.remove({"/Mat"}).isEmpty(), "deleted");
    const UsdPrim after = s.prim("/Geo");
    CHECK(!after.GetRelationship(TfToken("material:binding")), "a binding with only that target goes");
    SdfPathVector targets;
    after.GetRelationship(TfToken("both")).GetTargets(&targets);
    CHECK(targets == SdfPathVector{SdfPath("/Keep")}, "the other target stays");
    SdfPathVector sources;
    after.GetAttribute(TfToken("inputs:x")).GetConnections(&sources);
    CHECK(sources.empty(), "the connection goes");
    targets.clear();
    after.GetRelationship(TfToken("fromWeak")).GetTargets(&targets);
    CHECK(targets.empty(), "a weaker layer's target goes");
    CHECK(s.file(s.weak) == weak, "without touching that layer");
    CHECK(s.stack()->count() == 1, "one step");
    s.stack()->undo();
    CHECK(s.file(s.root) == root, "undo: all of it back");
}

// Deactivating leaves them: the prim is still there.
USDCC_TEST(prim_delete_deactivating_keeps_what_points_at_it) {
    Scene s;
    s.usd->SetEditTarget(s.weak);
    s.define("/Mat");
    s.usd->SetEditTarget(s.root);
    s.define("/Geo", "Cube").CreateRelationship(TfToken("material:binding")).SetTargets({SdfPath("/Mat")});
    CHECK(s.remove({"/Mat"}).isEmpty(), "deactivated");
    SdfPathVector targets;
    s.prim("/Geo").GetRelationship(TfToken("material:binding")).GetTargets(&targets);
    CHECK(targets == SdfPathVector{SdfPath("/Mat")}, "the binding stays");
}

// §3.4.
USDCC_TEST(prim_delete_clears_the_default_prim) {
    Scene s;
    s.define("/A");
    s.define("/B");
    s.root->SetDefaultPrim(TfToken("A"));
    CHECK(s.remove({"/B"}).isEmpty() && s.root->GetDefaultPrim() == TfToken("A"), "another prim leaves it");
    CHECK(s.remove({"/A"}).isEmpty() && !s.root->HasDefaultPrim(), "cleared");
    s.stack()->undo();
    CHECK(s.root->GetDefaultPrim() == TfToken("A"), "undo");
}

// §3.5.
USDCC_TEST(prim_delete_tidies_child_order) {
    Scene s;
    s.define("/P");
    for (const char* child : {"/P/a", "/P/b", "/P/c"}) s.define(child);
    s.root->GetPrimAtPath(SdfPath("/P"))->SetNameChildrenOrder({TfToken("c"), TfToken("b"), TfToken("a")});
    CHECK(s.remove({"/P/b"}).isEmpty(), "deleted");
    const auto order = s.root->GetPrimAtPath(SdfPath("/P"))->GetNameChildrenOrder();
    CHECK(std::vector<TfToken>(order.begin(), order.end()) == (std::vector<TfToken>{TfToken("c"), TfToken("a")}), "its name left the order");
}

// The Material Editor's helper: removes what it can, else deactivates, keeping overrides.
USDCC_TEST(prim_delete_in_edit_target) {
    Scene s;
    s.define("/Own");
    s.splitAcrossLayers("/Split");
    CHECK(prim_actions::deleteInEditTarget(s.usd, SdfPath("/Own")) && !s.has("/Own"), "removed");
    CHECK(prim_actions::deleteInEditTarget(s.usd, SdfPath("/Split")) && !s.prim("/Split").IsActive(), "deactivated");
    CHECK(s.root->GetAttributeAtPath(SdfPath("/Split.note")), "keeping the override");
}
