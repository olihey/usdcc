// Creating prims (docs/CREATE_MENU.md): unique names, where they can't go, undo, no type,
// variant edit mode, and the catalog of what can be made. Runs in usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_creation_catalog.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/variant_actions.h"

#include <pxr/base/tf/type.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/typed.h>

#include <QUndoStack>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

using usdcc::usd::PrimCreationCatalog;
using usdcc::usd::Stage;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
namespace prim_actions = usdcc::usd::prim_actions;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
struct Scene {
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    StageRefPtr stage = std::make_shared<Stage>(usd);
    UndoManager undo;
    SdfPath root = SdfPath::AbsoluteRootPath();
    SdfPath create(const std::string& type, const SdfPath& parent) { return prim_actions::createPrim(undo, stage, parent, type); }
    SdfPath create(const std::string& type) { return create(type, root); }
};

const PrimCreationCatalog::Item* findItem(const PrimCreationCatalog& catalog, const std::string& category, const std::string& label) {
    static std::vector<PrimCreationCatalog::Category> kept;
    kept = catalog.categories();
    for (const PrimCreationCatalog::Category& c : kept) {
        if (c.name != category) continue;
        for (const auto& group : c.groups) {
            for (const PrimCreationCatalog::Item& item : group) {
                if (item.label == label) return &item;
            }
        }
    }
    return nullptr;
}
}  // namespace

USDCC_TEST(create_prim_names_and_undo) {
    Scene s;
    const std::string before = [&] { std::string t; s.usd->GetRootLayer()->ExportToString(&t); return t; }();
    const SdfPath cube = s.create("Cube");
    CHECK(cube == SdfPath("/Cube"), "named after its type");
    CHECK(s.usd->GetPrimAtPath(cube).GetTypeName() == TfToken("Cube"), "of that type");
    CHECK(s.usd->GetPrimAtPath(cube).GetAttribute(TfToken("size")).HasAuthoredValue() == false, "with nothing authored");

    const SdfPath second = s.create("Cube");
    CHECK(second == SdfPath("/Cube1"), "the name's taken: Cube1");
    CHECK(s.create("Sphere", cube) == SdfPath("/Cube/Sphere"), "under a parent");
    CHECK(s.usd->GetPrimAtPath(cube).GetTypeName() == TfToken("Cube"), "the first one kept its type");

    // Inactive: it's still there, and its name is taken.
    s.usd->GetPrimAtPath(second).SetActive(false);
    CHECK(s.create("Cube") == SdfPath("/Cube2"), "an inactive prim's name counts");

    CHECK(s.create("NotAType").IsEmpty(), "a type USD doesn't know is refused");

    QUndoStack* stack = s.undo.stack(s.stage);
    CHECK(stack->text(stack->index() - 1) == QStringLiteral("Create Cube"), "named for the type");
    while (stack->canUndo()) stack->undo();
    std::string after;
    s.usd->GetRootLayer()->ExportToString(&after);
    CHECK(after == before, "undo leaves the file as it was");
    stack->redo();
    CHECK(s.usd->GetPrimAtPath(cube).IsValid(), "redo");
}

USDCC_TEST(create_prim_without_a_type) {
    Scene s;
    const SdfLayerRefPtr asset = SdfLayer::CreateAnonymous("asset.usda");
    const UsdStageRefPtr assetStage = UsdStage::Open(asset);
    assetStage->SetDefaultPrim(assetStage->DefinePrim(SdfPath("/Chair"), TfToken("Cube")));

    const SdfPath path = s.create(std::string());
    CHECK(path == SdfPath("/Prim"), "called Prim");
    const UsdPrim prim = s.usd->GetPrimAtPath(path);
    CHECK(prim.IsDefined() && prim.GetTypeName().IsEmpty(), "defined, with no type");
    CHECK(s.create(std::string()) == SdfPath("/Prim1"), "made unique like any other");

    CHECK(prim_actions::addArc(s.undo, s.stage, path, usdcc::usd::ArcInfo::Type::Reference, asset->GetIdentifier(), SdfPath()), "a reference");
    const UsdPrim referenced = s.usd->GetPrimAtPath(path);
    CHECK(referenced.GetTypeName() == TfToken("Cube"), "it takes the referenced type");
    double size = 0;
    CHECK(referenced.GetAttribute(TfToken("size")).Get(&size) && size == 2.0, "and its properties");
    CHECK(s.usd->GetRootLayer()->GetPrimAtPath(path)->GetTypeName().IsEmpty(), "while staying typeless in this file");
}

USDCC_TEST(create_prim_versioned_families) {
    Scene s;
    const SdfPath capsule = s.create("Capsule_1");
    CHECK(capsule == SdfPath("/Capsule"), "named after the family");
    CHECK(s.usd->GetPrimAtPath(capsule).GetTypeName() == TfToken("Capsule_1"), "of the version asked for");
}

USDCC_TEST(create_target_for_a_selection) {
    Scene s;
    const SdfPath a = s.create("Xform");
    const SdfPath b = s.create("Scope");
    using prim_actions::createTarget;
    CHECK(createTarget(s.stage, {}).parent == s.root && createTarget(s.stage, {}).problem.isEmpty(), "nothing: the root");
    CHECK(createTarget(s.stage, {a}).parent == a, "one: under it");
    CHECK(createTarget(s.stage, {a, b}).parent == s.root, "several: the root");
    CHECK(createTarget(s.stage, {SdfPath("/Gone")}).parent == s.root, "one that's gone: the root");

    s.usd->GetPrimAtPath(a).SetActive(false);
    const prim_actions::CreateTarget inactive = createTarget(s.stage, {a});
    CHECK(inactive.problem.contains(QStringLiteral("inactive")), "inactive: says why");
    CHECK(s.create("Cube", a).IsEmpty(), "and refuses, rather than a USD error");
    s.usd->GetPrimAtPath(a).SetActive(true);

    // An instance's contents.
    const SdfPath proto = s.create("Xform");
    s.create("Cube", proto);
    const SdfPath inst = s.create("Xform");
    const UsdPrim instance = s.usd->GetPrimAtPath(inst);
    instance.GetReferences().AddInternalReference(proto);
    instance.SetInstanceable(true);
    const SdfPath proxy = inst.AppendChild(TfToken("Cube"));
    CHECK(s.usd->GetPrimAtPath(proxy).IsInstanceProxy(), "a proxy");
    CHECK(createTarget(s.stage, {proxy}).problem.contains(QStringLiteral("instance")), "says why");
    CHECK(s.create("Cube", proxy).IsEmpty(), "and refuses");

    // Not loaded.
    const SdfLayerRefPtr heavy = SdfLayer::CreateAnonymous("heavy.usda");
    const UsdStageRefPtr heavyStage = UsdStage::Open(heavy);
    heavyStage->SetDefaultPrim(heavyStage->DefinePrim(SdfPath("/Heavy"), TfToken("Xform")));
    const SdfPath load = s.create("Xform");
    s.usd->GetPrimAtPath(load).GetPayloads().AddPayload(heavy->GetIdentifier());
    s.usd->Unload(load);
    CHECK(createTarget(s.stage, {load}).problem.contains(QStringLiteral("loaded")), "unloaded: says why");
    CHECK(s.create("Cube", load).IsEmpty(), "and refuses");
}

USDCC_TEST(create_prim_in_variant_edit_mode) {
    Scene s;
    const SdfPath chair = s.create("Xform");
    const SdfPath table = s.create("Scope");
    const SdfPath seat = s.create("Cube", chair);
    usdcc::usd::variant_actions::addSet(s.undo, s.stage, chair, "look", "red");
    s.stage->enterVariantEdit(chair, "look", "red");

    using prim_actions::createTarget;
    CHECK(createTarget(s.stage, {}).parent == chair, "nothing selected: the variant's prim");
    CHECK(createTarget(s.stage, {table}).parent == chair && createTarget(s.stage, {table}).problem.isEmpty(), "outside it: likewise");
    CHECK(createTarget(s.stage, {seat}).parent == seat, "inside it: under the selected prim");

    const SdfPath made = s.create("Sphere", chair);
    CHECK(made == chair.AppendChild(TfToken("Sphere")), "it's made");
    const SdfLayerHandle layer = s.usd->GetRootLayer();
    CHECK(layer->GetPrimAtPath(SdfPath("/Xform{look=red}Sphere")), "inside the variant");
    CHECK(!layer->GetPrimAtPath(made), "not beside it");
    CHECK(s.create("Cube", s.root).IsEmpty(), "the root can't be reached from there");
    s.stage->exitVariantEdit();
}

USDCC_TEST(create_catalog_reaches_every_type) {
    const PrimCreationCatalog catalog;
    const std::vector<PrimCreationCatalog::Category> categories = catalog.categories();
    std::set<std::string> listed;
    for (const auto& category : categories) {
        CHECK(!category.groups.empty(), "no empty category");
        for (const auto& group : category.groups) {
            CHECK(!group.empty(), "no empty group");
            for (const auto& item : group) {
                CHECK(listed.insert(item.label).second, "listed once");
                CHECK(UsdSchemaRegistry::GetInstance().FindConcretePrimDefinition(TfToken(item.type)) != nullptr, "something USD can make");
            }
        }
    }
    const std::set<std::string> hidden{"IrFkController", "IrJointScope", "IrSwitchController"};
    std::set<TfType> types;
    TfType::Find<UsdTyped>().GetAllDerivedTypes(&types);
    int concrete = 0;
    for (const TfType& type : types) {
        const TfToken name = UsdSchemaRegistry::GetConcreteSchemaTypeName(type);
        if (name.IsEmpty()) continue;
        ++concrete;
        const std::string family = UsdSchemaRegistry::ParseSchemaFamilyAndVersionFromIdentifier(name).first.GetString();
        CHECK(listed.count(family) == 1 || hidden.count(family) == 1, "every concrete type is reachable, or deliberately hidden");
    }
    CHECK(concrete > 0, "USD reports some");
    for (const std::string& name : hidden) CHECK(listed.count(name) == 0, "the hidden ones stay out");
    for (const auto& category : categories) CHECK(category.name != PrimCreationCatalog::kPlugins, "nothing unsorted among USD's own");
}

USDCC_TEST(create_catalog_order_and_versions) {
    const PrimCreationCatalog catalog;
    const std::vector<PrimCreationCatalog::Category> categories = catalog.categories();
    CHECK(categories.front().name == PrimCreationCatalog::kTopLevel, "the top level first");
    CHECK(categories.front().groups.front().front().label == "Xform", "as written");
    CHECK(categories[1].name == "Geometry" && categories[1].groups.size() == 4, "then Geometry, in its groups");
    CHECK(categories[1].groups[0][0].label == "Cube", "Cube first");

    // A family with two versions has both, the original by the family's name: not every
    // renderer knows the newer ones (RTX draws neither DomeLight_1 nor Capsule_1).
    for (const auto& [category, family] : {std::pair{"Geometry", "Capsule"}, std::pair{"Geometry", "Cylinder"},
                                           std::pair{"Lights", "DomeLight"}}) {
        const std::string newer = std::string(family) + "_1";
        // Copies: findItem()'s pointer is only good until its next call.
        const auto copy = [](const PrimCreationCatalog::Item* item) {
            return item ? std::optional<PrimCreationCatalog::Item>(*item) : std::nullopt;
        };
        const auto original = copy(findItem(catalog, category, family));
        CHECK(original && original->type == family, "%s makes %s", family, family);
        const auto latest = copy(findItem(catalog, category, newer));
        CHECK(latest && latest->type == newer, "and %s its own line", newer.c_str());
        CHECK(original && latest && original->documentation.starts_with("The original version; " + newer + " is newer") &&
                  latest->documentation.starts_with(std::string("The newest version of ") + family),
              "their tooltips say which is which");
    }
    // Next to each other, the original first.
    for (const auto& category : catalog.categories()) {
        if (category.name != "Lights") continue;
        const auto& line = category.groups.front();
        const auto at = std::find_if(line.begin(), line.end(), [](const auto& item) { return item.type == "DomeLight"; });
        CHECK(at != line.end() && at + 1 != line.end() && (at + 1)->type == "DomeLight_1", "side by side");
    }
    const auto* cube = findItem(catalog, "Geometry", "Cube");
    CHECK(cube && cube->type == "Cube" && !cube->documentation.empty() && !cube->documentation.starts_with("The "),
          "with its documentation, and no version note for a type with one version");
}

USDCC_TEST(create_catalog_changes_at_runtime) {
    PrimCreationCatalog catalog;
    PrimCreationCatalog::Entry rigging;
    rigging.category = "Rigging";
    catalog.set("Cube", rigging);
    CHECK(findItem(catalog, "Rigging", "Cube") && !findItem(catalog, "Geometry", "Cube"), "moved to a new category");
    std::vector<PrimCreationCatalog::Category> categories = catalog.categories();
    CHECK(categories.back().name == "Rigging", "after the built-in ones");
    catalog.set("Cube", rigging);
    CHECK(findItem(catalog, "Rigging", "Cube"), "setting it again is fine");

    CHECK(catalog.remove("Cube"), "removed");
    CHECK(findItem(catalog, PrimCreationCatalog::kPlugins, "Cube"), "with no entry it's in Plugins");
    CHECK(catalog.categories().back().name == PrimCreationCatalog::kPlugins, "which is last");
    CHECK(!catalog.remove("Cube"), "there's none now");

    catalog.hide("Sphere");
    CHECK(!findItem(catalog, "Geometry", "Sphere"), "hidden");

    catalog.hide("Capsule_1");
    CHECK(!findItem(catalog, "Geometry", "Capsule") && !findItem(catalog, "Geometry", "Capsule_1"),
          "either spelling names the family, both versions");

    PrimCreationCatalog::Entry future;
    future.category = "Future";
    catalog.set("TypeFromALaterPlugin", future);
    for (const auto& category : catalog.categories()) CHECK(category.name != "Future", "a type USD doesn't know yet is left out");

    catalog.reset();
    CHECK(findItem(catalog, "Geometry", "Cube") && findItem(catalog, "Geometry", "Sphere"), "reset restores the built-in ones");
    CHECK(!findItem(catalog, PrimCreationCatalog::kPlugins, "Cube"), "and only those");
}
