// Reordering and reparenting (docs/OUTLINER_REPARENT.md): undo across layers, what
// follows a moved prim, what's refused and why, order, and the world position. Runs in
// usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/variant_actions.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformOp.h>

#include <QUndoStack>

#include <memory>

using usdcc::usd::PrimMoves;
using usdcc::usd::Stage;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
namespace prim_actions = usdcc::usd::prim_actions;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
struct Scene {
    SdfLayerRefPtr weak = SdfLayer::CreateAnonymous("weak.usda");
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    StageRefPtr stage = std::make_shared<Stage>(usd);
    UndoManager undo;
    SdfLayerHandle root = usd->GetRootLayer();
    Scene() { root->GetSubLayerPaths().push_back(weak->GetIdentifier()); }
    void define(const char* path, const char* type = "Xform") { usd->DefinePrim(SdfPath(path), TfToken(type)); }
    bool has(const char* path) const { return static_cast<bool>(usd->GetPrimAtPath(SdfPath(path))); }
    QString move(const char* path, const char* parent, prim_actions::MoveOptions options = {}) {
        return prim_actions::movePrims(undo, stage, {SdfPath(path)}, SdfPath(parent), options);
    }
    std::vector<std::string> children(const char* path) const {
        std::vector<std::string> names;
        for (const UsdPrim& child : usd->GetPrimAtPath(SdfPath(path)).GetFilteredChildren(UsdPrimAllPrimsPredicate)) names.push_back(child.GetName());
        return names;
    }
    QUndoStack* stack() { return undo.stack(stage); }
    // A def in the weaker layer, and an override in the root layer.
    void splitAcrossLayers(const char* path) {
        usd->SetEditTarget(weak);
        define(path, "Cube");
        usd->SetEditTarget(root);
        usd->OverridePrim(SdfPath(path)).CreateAttribute(TfToken("note"), SdfValueTypeNames->Int).Set(1);
    }
    std::string file(const SdfLayerHandle& layer) const {
        std::string text;
        layer->ExportToString(&text);
        return text;
    }
};
using Names = std::vector<std::string>;
}  // namespace

// The bug this all began with: undo recorded only the edit target's layer.
USDCC_TEST(prim_move_rename_undoes_in_every_layer) {
    Scene s;
    s.splitAcrossLayers("/K");
    const std::string weak = s.file(s.weak);
    const std::string root = s.file(s.root);
    CHECK(prim_actions::rename(s.undo, s.stage, SdfPath("/K"), "L"), "renames");
    CHECK(s.has("/L") && !s.has("/K"), "composed");
    CHECK(s.weak->GetPrimAtPath(SdfPath("/L")) && s.root->GetPrimAtPath(SdfPath("/L")), "in both layers");
    CHECK(s.stack()->count() == 1, "one step");
    s.stack()->undo();
    CHECK(s.has("/K") && !s.has("/L"), "undo: composed");
    CHECK(s.file(s.weak) == weak && s.file(s.root) == root, "undo: both layers as they were");
    s.stack()->redo();
    CHECK(s.has("/L") && !s.has("/K") && s.usd->GetPrimAtPath(SdfPath("/L")).GetAttribute(TfToken("note")), "redo");
    CHECK(!prim_actions::rename(s.undo, s.stage, SdfPath("/L"), "L"), "not to its own name");
}

USDCC_TEST(prim_move_reparents_and_undoes) {
    Scene s;
    s.splitAcrossLayers("/G/K");
    s.define("/N");
    s.define("/G/K/Child", "Cube");
    const std::string weak = s.file(s.weak);
    const std::string root = s.file(s.root);
    int announced = 0;
    PrimMoves last;
    auto connection = s.stage->primsMoved.connect([&](const PrimMoves& moves) { ++announced; last = moves; });

    CHECK(s.move("/G/K", "/N").isEmpty(), "moves");
    CHECK(s.has("/N/K/Child") && !s.has("/G/K"), "with its subtree");
    CHECK(s.weak->GetPrimAtPath(SdfPath("/N/K")) && s.root->GetPrimAtPath(SdfPath("/N/K")), "in both layers");
    CHECK(s.stack()->text(0) == QStringLiteral("Move K to /N"), "named for what it did");
    CHECK(announced == 1 && last.front().first == SdfPath("/G/K") && last.front().second == SdfPath("/N/K"), "announced");

    s.stack()->undo();
    CHECK(s.has("/G/K/Child") && !s.has("/N/K"), "undo");
    CHECK(s.file(s.weak) == weak && s.file(s.root) == root, "both layers as they were");
    CHECK(announced == 2 && last.front().first == SdfPath("/N/K") && last.front().second == SdfPath("/G/K"), "announced the way back");
    s.stack()->redo();
    CHECK(s.has("/N/K/Child") && announced == 3, "redo");
}

USDCC_TEST(prim_move_what_points_at_it_follows) {
    Scene s;
    s.define("/World");
    s.define("/World/Chair");
    s.define("/Other");
    const UsdPrim mesh = s.usd->DefinePrim(SdfPath("/Other/Mesh"), TfToken("Mesh"));
    mesh.CreateRelationship(TfToken("material:binding")).AddTarget(SdfPath("/World/Chair"));
    const UsdAttribute attr = mesh.CreateAttribute(TfToken("a"), SdfValueTypeNames->Float);
    attr.AddConnection(SdfPath("/World/Chair.size"));
    UsdCollectionAPI collection = UsdCollectionAPI::Apply(mesh, TfToken("c"));
    collection.GetIncludesRel().AddTarget(SdfPath("/World/Chair"));
    s.usd->SetDefaultPrim(s.usd->GetPrimAtPath(SdfPath("/World/Chair")).GetParent());

    CHECK(s.move("/World/Chair", "/Other").isEmpty(), "moves");
    SdfPathVector targets;
    mesh.GetRelationship(TfToken("material:binding")).GetTargets(&targets);
    CHECK(targets == SdfPathVector{SdfPath("/Other/Chair")}, "a relationship");
    targets.clear();
    attr.GetConnections(&targets);
    CHECK(targets == SdfPathVector{SdfPath("/Other/Chair.size")}, "a connection");
    targets.clear();
    collection.GetIncludesRel().GetTargets(&targets);
    CHECK(targets == SdfPathVector{SdfPath("/Other/Chair")}, "a collection");
    s.stack()->undo();
    targets.clear();
    mesh.GetRelationship(TfToken("material:binding")).GetTargets(&targets);
    CHECK(targets == SdfPathVector{SdfPath("/World/Chair")}, "and back on undo");
}

USDCC_TEST(prim_move_several_as_one_step) {
    Scene s;
    for (const char* path : {"/A", "/B", "/C", "/A/Under", "/D"}) s.define(path);
    // An editor holds one edit: declaring two and applying moves only the last.
    CHECK(prim_actions::movePrims(s.undo, s.stage, {SdfPath("/C"), SdfPath("/A"), SdfPath("/A/Under")}, SdfPath("/D")).isEmpty(), "moves");
    CHECK(s.has("/D/A") && s.has("/D/C"), "all of them");
    CHECK(s.has("/D/A/Under") && !s.has("/D/Under"), "one under another goes along with it");
    CHECK(s.stack()->count() == 1, "one step");
    s.stack()->undo();
    CHECK(s.has("/A/Under") && s.has("/C") && !s.has("/D/A"), "one undo");

    s.define("/E/Same");
    s.define("/F/Same");
    const QString why = prim_actions::movePrims(s.undo, s.stage, {SdfPath("/E/Same"), SdfPath("/F/Same")}, SdfPath("/D"));
    CHECK(why.contains(QStringLiteral("two prims")), "two of one name refuse");
    CHECK(s.has("/E/Same") && s.has("/F/Same"), "and nothing moved");
}

USDCC_TEST(prim_move_refusals) {
    Scene s;
    for (const char* path : {"/A", "/A/B", "/C", "/C/B", "/Off"}) s.define(path);
    CHECK(s.move("/A/B", "/C").contains(QStringLiteral("already has")), "the name's taken");
    CHECK(s.move("/A", "/A/B").contains(QStringLiteral("into itself")), "into its own descendant");
    CHECK(s.move("/Gone", "/C").contains(QStringLiteral("doesn't exist")), "not there");
    s.usd->GetPrimAtPath(SdfPath("/Off")).SetActive(false);
    CHECK(s.move("/C/B", "/Off").contains(QStringLiteral("inactive")), "under an inactive prim, where it would vanish");
    s.usd->GetPrimAtPath(SdfPath("/Off")).SetActive(true);
    s.usd->GetPrimAtPath(SdfPath("/A")).SetActive(false);
    CHECK(s.move("/A", "/Off").isEmpty(), "an inactive prim itself can move");
    CHECK(s.stack()->count() == 1, "only that made a step");

    // An instance's contents.
    s.define("/Proto");
    s.define("/Proto/G", "Cube");
    s.define("/Dest");
    const UsdPrim instance = s.usd->DefinePrim(SdfPath("/I"), TfToken("Xform"));
    instance.GetReferences().AddInternalReference(SdfPath("/Proto"));
    instance.SetInstanceable(true);
    CHECK(s.move("/I/G", "/Dest").contains(QStringLiteral("instance")), "an instance proxy");
    CHECK(s.move("/C", "/I/G").contains(QStringLiteral("instance")), "onto one");
    CHECK(s.move("/I", "/Dest").isEmpty(), "an instance itself");
    CHECK(s.usd->GetPrimAtPath(SdfPath("/Dest/I")).IsInstance(), "still one");
}

USDCC_TEST(prim_move_refuses_a_muted_layers_opinions) {
    Scene s;
    s.define("/G/K");
    s.define("/N");
    SdfCreatePrimInLayer(s.weak, SdfPath("/G/K"));  // OverridePrim would author nothing: the prim exists
    s.usd->MuteLayer(s.weak->GetIdentifier());
    CHECK(s.move("/G/K", "/N").contains(QStringLiteral("muted")), "says why");
    s.usd->UnmuteLayer(s.weak->GetIdentifier());
    CHECK(s.move("/G/K", "/N").isEmpty(), "and works unmuted");
}

USDCC_TEST(prim_move_refuses_a_layer_it_cant_edit) {
    Scene s;
    s.usd->SetEditTarget(s.weak);
    s.define("/G/K", "Cube");
    s.define("/N");
    s.usd->SetEditTarget(s.root);
    s.weak->SetPermissionToEdit(false);
    CHECK(s.move("/G/K", "/N").contains(QStringLiteral("can't be edited")), "says why");
    CHECK(s.has("/G/K"), "nothing moved");
    s.weak->SetPermissionToEdit(true);
}

USDCC_TEST(prim_move_opinions_in_variants) {
    Scene s;
    s.define("/Chair");
    s.define("/Chair/Seat", "Cube");
    s.define("/Else");
    usdcc::usd::variant_actions::addSet(s.undo, s.stage, SdfPath("/Chair"), "look", "red");
    usdcc::usd::variant_actions::addVariant(s.undo, s.stage, SdfPath("/Chair"), "look", "blue");
    s.stage->enterVariantEdit(SdfPath("/Chair"), "look", "red");
    s.usd->GetPrimAtPath(SdfPath("/Chair/Seat")).GetAttribute(TfToken("size")).Set(3.0);
    s.define("/Chair/OnlyInRed", "Sphere");

    CHECK(s.move("/Chair/Seat", "/Else").contains(QStringLiteral("exit first")), "not while editing a variant");
    s.stage->exitVariantEdit();

    UsdVariantSet look = s.usd->GetPrimAtPath(SdfPath("/Chair")).GetVariantSet("look");
    look.SetVariantSelection("red");
    CHECK(s.move("/Chair/Seat", "/Else").contains(QStringLiteral("{look=red}")), "a selected variant's opinion");
    CHECK(s.move("/Chair/OnlyInRed", "/Else").contains(QStringLiteral("{look=red}")), "a prim only in one");
    look.SetVariantSelection("blue");
    CHECK(s.move("/Chair/Seat", "/Else").contains(QStringLiteral("{look=red}")), "an unselected variant's, which would split it");

    // The prim holding the set takes its variants along.
    CHECK(s.move("/Chair", "/Else").isEmpty(), "the set's own prim moves");
    const UsdPrim moved = s.usd->GetPrimAtPath(SdfPath("/Else/Chair"));
    CHECK(moved && moved.GetVariantSet("look").HasAuthoredVariant("red"), "with its variants");
    moved.GetVariantSet("look").SetVariantSelection("red");
    double size = 0;
    s.usd->GetPrimAtPath(SdfPath("/Else/Chair/Seat")).GetAttribute(TfToken("size")).Get(&size);
    CHECK(size == 3.0 && s.has("/Else/Chair/OnlyInRed"), "and what's in them");
}

USDCC_TEST(prim_move_order) {
    Scene s;
    for (const char* path : {"/P/A", "/P/B", "/P/C", "/P/D", "/Q/X", "/Q/Y"}) s.define(path);
    prim_actions::MoveOptions at;

    at.row = 0;
    CHECK(s.move("/P/C", "/P", at).isEmpty(), "reorders");
    CHECK(s.children("/P") == Names({"C", "A", "B", "D"}), "to the front");
    CHECK(s.stack()->text(s.stack()->count() - 1) == QStringLiteral("Reorder C"), "named so");
    int steps = s.stack()->count();
    at.row = 1;
    CHECK(s.move("/P/C", "/P", at).isEmpty() && s.stack()->count() == steps, "dropped where it is: no step");
    at.row = 0;
    CHECK(s.move("/P/C", "/P", at).isEmpty() && s.stack()->count() == steps, "either side of itself");
    at.row = -1;
    CHECK(s.move("/P/C", "/P", at).isEmpty() && s.stack()->count() == steps, "or on its parent");

    at.row = 4;
    CHECK(s.move("/P/C", "/P", at).isEmpty(), "reorders");
    CHECK(s.children("/P") == Names({"A", "B", "D", "C"}), "to the end");
    s.stack()->undo();
    CHECK(s.children("/P") == Names({"C", "A", "B", "D"}), "undone");

    // Several, keeping their tree order, whichever order they came in.
    at.row = 4;
    CHECK(prim_actions::movePrims(s.undo, s.stage, {SdfPath("/P/B"), SdfPath("/P/A")}, SdfPath("/P"), at).isEmpty(), "two");
    CHECK(s.children("/P") == Names({"C", "D", "A", "B"}), "in tree order");

    // Into a position in another parent, as one step.
    steps = s.stack()->count();
    at.row = 1;
    CHECK(s.move("/P/D", "/Q", at).isEmpty(), "moves");
    CHECK(s.children("/Q") == Names({"X", "D", "Y"}), "at the row");
    CHECK(s.stack()->count() == steps + 1, "one step");
    s.stack()->undo();
    CHECK(s.children("/Q") == Names({"X", "Y"}) && s.children("/P") == Names({"C", "D", "A", "B"}), "undone together");
    s.stack()->redo();

    // The list has no stale names.
    const SdfPrimSpecHandle spec = s.root->GetPrimAtPath(SdfPath("/P"));
    const auto list = spec->GetNameChildrenOrder();
    CHECK(std::find(list.begin(), list.end(), TfToken("D")) == list.end(), "the one moved away left the list");
    CHECK(prim_actions::rename(s.undo, s.stage, SdfPath("/P/C"), "Z"), "renames");
    CHECK(s.children("/P") == Names({"Z", "A", "B"}), "and it keeps its place");

    // The root.
    s.define("/R1");
    s.define("/R2");
    at.row = 0;
    CHECK(s.move("/R2", "/", at).isEmpty(), "at the root");
    CHECK(s.children("/").front() == "R2", "first");
}

USDCC_TEST(prim_move_order_over_another_layer) {
    Scene s;
    s.usd->SetEditTarget(s.weak);
    for (const char* path : {"/P/A", "/P/B"}) s.define(path);
    s.usd->SetEditTarget(s.root);
    prim_actions::MoveOptions at;
    at.row = 0;
    CHECK(s.move("/P/B", "/P", at).isEmpty(), "reorders");
    CHECK(s.children("/P") == Names({"B", "A"}), "the weaker layer's children");
    CHECK(s.root->GetPrimAtPath(SdfPath("/P")) && !s.root->GetPrimAtPath(SdfPath("/P/B")), "with an over here, moving nothing");
}

USDCC_TEST(prim_move_other_open_stages_follow) {
    Scene s;
    s.define("/Asset");
    s.define("/Asset/Part", "Cube");
    s.define("/Asset/Dst");
    s.usd->SetDefaultPrim(s.usd->GetPrimAtPath(SdfPath("/Asset")));
    const UsdStageRefPtr other = UsdStage::CreateInMemory();
    other->DefinePrim(SdfPath("/Use"), TfToken("Xform")).GetReferences().AddReference(s.root->GetIdentifier());
    other->OverridePrim(SdfPath("/Use/Part")).CreateAttribute(TfToken("o"), SdfValueTypeNames->Int).Set(5);
    const StageRefPtr otherStage = std::make_shared<Stage>(other);
    const std::string before = [&] { std::string t; other->GetRootLayer()->ExportToString(&t); return t; }();

    prim_actions::MoveOptions options;
    options.openStages = {s.stage, otherStage};
    CHECK(s.move("/Asset/Part", "/Asset/Dst", options).isEmpty(), "moves");
    int value = 0;
    const UsdPrim moved = other->GetPrimAtPath(SdfPath("/Use/Dst/Part"));
    CHECK(moved && moved.GetAttribute(TfToken("o")).Get(&value) && value == 5, "the other stage's override follows");
    s.stack()->undo();
    std::string after;
    other->GetRootLayer()->ExportToString(&after);
    CHECK(after == before, "and undo restores that stage's layer too");
    CHECK(other->GetPrimAtPath(SdfPath("/Use/Part")).GetAttribute(TfToken("o")).Get(&value) && value == 5, "its override's back");
}

USDCC_TEST(prim_move_keeps_the_world_position) {
    Scene s;
    auto translated = [&](const char* path, GfVec3d offset) {
        UsdGeomXformable xformable(s.usd->DefinePrim(SdfPath(path), TfToken("Xform")));
        xformable.AddTranslateOp().Set(offset);
        return xformable;
    };
    auto world = [&](const char* path) {
        return UsdGeomXformCache().GetLocalToWorldTransform(s.usd->GetPrimAtPath(SdfPath(path))).ExtractTranslation();
    };
    translated("/Pa", GfVec3d(10, 0, 0));
    translated("/Pa/Ch", GfVec3d(1, 0, 0));
    translated("/Pb", GfVec3d(0, 5, 0));
    translated("/Pa/Local", GfVec3d(2, 0, 0));
    prim_actions::MoveOptions keep;
    keep.keepWorldPosition = true;

    CHECK(s.move("/Pa/Ch", "/Pb", keep).isEmpty(), "moves");
    CHECK(GfIsClose(world("/Pb/Ch"), GfVec3d(11, 0, 0), 1e-9), "where it was");
    bool resets = false;
    CHECK(GfIsClose(UsdGeomXformable(s.usd->GetPrimAtPath(SdfPath("/Pb/Ch"))).GetOrderedXformOps(&resets).front().GetOpType() == UsdGeomXformOp::TypeTranslate ? 1.0 : 0.0, 1.0, 1e-9), "as translate, rotate and scale");
    s.stack()->undo();
    CHECK(GfIsClose(world("/Pa/Ch"), GfVec3d(11, 0, 0), 1e-9) && s.has("/Pa/Ch"), "undo: both the move and the transform");
    GfVec3d local;
    UsdGeomXformable(s.usd->GetPrimAtPath(SdfPath("/Pa/Ch"))).GetOrderedXformOps(&resets).front().Get(&local);
    CHECK(GfIsClose(local, GfVec3d(1, 0, 0), 1e-9), "and its own values");

    // Without: the local transform stays.
    CHECK(s.move("/Pa/Local", "/Pb").isEmpty(), "moves");
    CHECK(GfIsClose(world("/Pb/Local"), GfVec3d(2, 5, 0), 1e-9), "so it changes place");

    // Animated: it can't hold still, so it keeps its own values.
    translated("/Anim", GfVec3d(0, 0, 0));
    UsdGeomXformOp animated = UsdGeomXformable(s.usd->GetPrimAtPath(SdfPath("/Anim"))).GetOrderedXformOps(&resets).front();
    animated.Set(GfVec3d(1, 1, 1), 1.0);
    animated.Set(GfVec3d(2, 2, 2), 2.0);  // one sample alone is no animation
    translated("/Pa/Kid", GfVec3d(3, 0, 0));
    CHECK(s.move("/Pa/Kid", "/Anim", keep).isEmpty(), "moves under an animated parent");
    UsdGeomXformable(s.usd->GetPrimAtPath(SdfPath("/Anim/Kid"))).GetOrderedXformOps(&resets).front().Get(&local);
    CHECK(GfIsClose(local, GfVec3d(3, 0, 0), 1e-9), "its local transform untouched");

    // A prim that ignores its parents' transform has nothing to change.
    UsdGeomXformable free = translated("/Pa/Free", GfVec3d(4, 0, 0));
    free.SetResetXformStack(true);
    CHECK(s.move("/Pa/Free", "/Pb", keep).isEmpty(), "moves");
    CHECK(GfIsClose(world("/Pb/Free"), GfVec3d(4, 0, 0), 1e-9), "resetXformStack: unchanged");
}

// Phase 4.
USDCC_TEST(prim_move_from_a_reference) {
    Scene s;
    const SdfLayerRefPtr asset = SdfLayer::CreateAnonymous("asset.usda");
    const UsdStageRefPtr assetStage = UsdStage::Open(asset);
    assetStage->SetDefaultPrim(assetStage->DefinePrim(SdfPath("/Chair"), TfToken("Xform")));
    assetStage->DefinePrim(SdfPath("/Chair/Leg"), TfToken("Cube"));
    const std::string assetBefore = s.file(asset);

    s.usd->DefinePrim(SdfPath("/Inst"), TfToken("Xform")).GetReferences().AddReference(asset->GetIdentifier());
    s.define("/Dest");
    s.usd->OverridePrim(SdfPath("/Inst/Leg")).CreateAttribute(TfToken("o"), SdfValueTypeNames->Int).Set(1);
    const std::string before = s.file(s.root);

    const prim_actions::MoveNotes notes = prim_actions::moveNotes(s.usd, {SdfPath("/Inst/Leg")}, SdfPath("/Dest"));
    CHECK(notes.relocated == std::vector<SdfPath>{SdfPath("/Inst/Leg")}, "it says so first");
    CHECK(prim_actions::moveNotes(s.usd, {SdfPath("/Inst")}, SdfPath("/Dest")).isEmpty(), "not for the prim holding the reference");

    CHECK(s.move("/Inst/Leg", "/Dest").isEmpty(), "moves");
    CHECK(s.has("/Dest/Leg") && !s.has("/Inst/Leg"), "composed");
    int value = 0;
    CHECK(s.usd->GetPrimAtPath(SdfPath("/Dest/Leg")).GetAttribute(TfToken("o")).Get(&value) && value == 1, "with its override");
    CHECK(s.file(s.root).find("relocates") != std::string::npos, "through a relocates entry");
    CHECK(s.file(asset) == assetBefore, "the asset untouched");
    s.stack()->undo();
    CHECK(s.has("/Inst/Leg") && !s.has("/Dest/Leg"), "undo");
    CHECK(s.file(s.root) == before, "the layer as it was");
    s.stack()->redo();
    CHECK(s.has("/Dest/Leg"), "redo");
}

USDCC_TEST(prim_move_notes_name_other_layers) {
    Scene s;
    s.splitAcrossLayers("/G/K");
    s.define("/N");
    const prim_actions::MoveNotes notes = prim_actions::moveNotes(s.usd, {SdfPath("/G/K")}, SdfPath("/N"));
    CHECK(notes.otherLayers.size() == 1 && notes.otherLayers.front() == s.weak->GetDisplayName(), "the weaker layer");
    CHECK(prim_actions::moveNotes(s.usd, {SdfPath("/G/K")}, SdfPath("/G")).isEmpty(), "a reorder changes nothing else");
    s.define("/Only");
    CHECK(prim_actions::moveNotes(s.usd, {SdfPath("/Only")}, SdfPath("/N")).isEmpty(), "nor does a prim only here");
}

USDCC_TEST(prim_move_tool_target_follows) {
    Scene s;
    s.define("/A/B");
    s.define("/C");
    UsdGeomXformable(s.usd->GetPrimAtPath(SdfPath("/A/B"))).AddTranslateOp().Set(GfVec3d(1, 0, 0));
    const TfToken op("xformOp:translate");
    s.stage->setTargetXformOp(SdfPath("/A/B"), op);
    CHECK(s.move("/A/B", "/C").isEmpty(), "moves");
    CHECK(s.stage->targetPrim() == SdfPath("/C/B") && s.stage->targetXformOp() == op, "the tool's target goes with it");
    s.stack()->undo();
    CHECK(s.stage->targetPrim() == SdfPath("/A/B") && s.stage->targetXformOp() == op, "and back");
}
