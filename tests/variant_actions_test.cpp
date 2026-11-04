// Variant authoring (docs/VARIANT_EDITOR.md): sets and variants with their undo,
// layers, and variant edit mode's traps. Runs in usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/composition_arcs.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/variant_actions.h"
#include "usdcc/usd/xform_stack.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>

#include <QUndoStack>

#include <memory>

using usdcc::usd::Stage;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
namespace actions = usdcc::usd::variant_actions;
namespace prim_actions = usdcc::usd::prim_actions;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const SdfPath kChair("/Chair");
const SdfPath kSeat("/Chair/Seat");

struct Scene {
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    StageRefPtr stage = std::make_shared<Stage>(usd);
    UndoManager undo;
    SdfLayerHandle root = usd->GetRootLayer();
    Scene() {
        usd->DefinePrim(kChair, TfToken("Xform"));
        usd->DefinePrim(kSeat, TfToken("Cube"));
        usd->DefinePrim(SdfPath("/Table"), TfToken("Cube"));
    }
    UsdPrim chair() const { return usd->GetPrimAtPath(kChair); }
    UsdVariantSet look() const { return chair().GetVariantSet("look"); }
    std::string file() const {
        std::string text;
        root->ExportToString(&text);
        return text;
    }
    bool contains(const std::string& what) const { return file().find(what) != std::string::npos; }
    void addLook() {
        actions::addSet(undo, stage, kChair, "look", "red");
        actions::addVariant(undo, stage, kChair, "look", "blue");
    }
    // Something in red: a value on the child.
    void paintRed() {
        look().SetVariantSelection("red");
        usd->SetEditTarget(look().GetVariantEditTarget());
        usd->GetPrimAtPath(kSeat).GetAttribute(TfToken("size")).Set(3.0);
        usd->SetEditTarget(root);
    }
};
}  // namespace

USDCC_TEST(variant_set_add_undo_redo) {
    Scene s;
    const std::string before = s.file();
    CHECK(actions::addSet(s.undo, s.stage, kChair, "look", "red"), "adds a set");
    CHECK(s.look().GetVariantNames() == std::vector<std::string>{"red"}, "with its first variant");
    CHECK(s.look().GetVariantSelection() == "red", "selected");
    CHECK(!actions::addSet(s.undo, s.stage, kChair, "look", "green"), "not twice");
    CHECK(!actions::addSet(s.undo, s.stage, kChair, "1 bad", "red"), "not a name USD would refuse");
    CHECK(!actions::addSet(s.undo, s.stage, kChair, "other", "bad name"), "nor a bad variant name");

    QUndoStack* stack = s.undo.stack(s.stage);
    CHECK((stack->count()) == (1), "count");
    stack->undo();
    CHECK(s.file() == before, "undo leaves the file as it was");
    stack->redo();
    CHECK(s.look().GetVariantSelection() == "red", "redo brings it back");
}

USDCC_TEST(variant_remove_keeps_no_dangling_selection) {
    Scene s;
    s.addLook();
    s.look().SetVariantSelection("blue");
    QUndoStack* stack = s.undo.stack(s.stage);
    const int steps = stack->count();
    CHECK(actions::removeVariant(s.undo, s.stage, kChair, "look", "blue"), "removes");
    CHECK(s.look().GetVariantNames() == std::vector<std::string>{"red"}, "it's gone");
    CHECK(s.look().GetVariantSelection().empty(), "and so is the selection pointing at it");
    stack->undo();
    CHECK((stack->count()) == (steps + 1), "count");
    CHECK(s.look().GetVariantNames().size() == 2 && s.look().GetVariantSelection() == "blue", "undo restores both");
}

USDCC_TEST(variant_rename_keeps_opinions_and_selection) {
    Scene s;
    s.addLook();
    s.paintRed();
    CHECK(actions::renameVariant(s.undo, s.stage, kChair, "look", "red", "crimson"), "renames");
    CHECK(s.look().GetVariantSelection() == "crimson", "the selection follows");
    double size = 0;
    s.usd->GetPrimAtPath(kSeat).GetAttribute(TfToken("size")).Get(&size);
    CHECK(size == 3.0, "its opinions came along");
    CHECK(!s.contains("\"red\""), "nothing's left of the old one");
    CHECK(!actions::renameVariant(s.undo, s.stage, kChair, "look", "crimson", "blue"), "not onto another");
    s.undo.stack(s.stage)->undo();
    CHECK(s.look().GetVariantSelection() == "red" && s.contains("\"red\"") && !s.contains("crimson"), "undo");
    s.usd->GetPrimAtPath(kSeat).GetAttribute(TfToken("size")).Get(&size);
    CHECK(size == 3.0, "undo keeps the opinions too");
}

USDCC_TEST(variant_duplicate_copies_opinions) {
    Scene s;
    s.addLook();
    s.paintRed();
    CHECK(actions::duplicateVariant(s.undo, s.stage, kChair, "look", "red", "pink"), "duplicates");
    CHECK(s.look().GetVariantSelection() == "red", "the selection stays");
    s.look().SetVariantSelection("pink");
    double size = 0;
    s.usd->GetPrimAtPath(kSeat).GetAttribute(TfToken("size")).Get(&size);
    CHECK(size == 3.0, "it holds the same opinions");
    s.look().SetVariantSelection("red");
    s.undo.stack(s.stage)->undo();
    CHECK(!s.contains("pink"), "undo removes it");
}

USDCC_TEST(variant_set_remove_all_three_parts) {
    Scene s;
    s.addLook();
    actions::addSet(s.undo, s.stage, kChair, "size", "big");
    CHECK(actions::removeSet(s.undo, s.stage, kChair, "size"), "removes");
    CHECK(!s.chair().GetVariantSets().HasVariantSet("size"), "the name");
    CHECK(s.chair().GetVariantSets().HasVariantSet("look"), "the other one stays");
    CHECK(!s.contains("size =") && !s.contains("variantSet \"size\"") && !s.contains("big"), "the spec and the selection");
    s.undo.stack(s.stage)->undo();
    CHECK(s.chair().GetVariantSets().HasVariantSet("size") && s.chair().GetVariantSet("size").GetVariantSelection() == "big", "undo");
}

USDCC_TEST(variant_set_rename_keeps_everything) {
    Scene s;
    s.addLook();
    actions::addSet(s.undo, s.stage, kChair, "size", "big");
    s.paintRed();
    const std::vector<std::string> before = s.chair().GetVariantSets().GetNames();
    CHECK(actions::renameSet(s.undo, s.stage, kChair, "look", "mood"), "renames");
    const UsdVariantSet mood = s.chair().GetVariantSet("mood");
    CHECK(mood.GetVariantSelection() == "red", "its selection");
    CHECK(mood.GetVariantNames().size() == 2, "its variants");
    double size = 0;
    s.usd->GetPrimAtPath(kSeat).GetAttribute(TfToken("size")).Get(&size);
    CHECK(size == 3.0, "its opinions");
    CHECK(!s.chair().GetVariantSets().HasVariantSet("look") && !s.contains("look"), "nothing of the old name");
    CHECK(s.chair().GetVariantSets().GetNames().size() == before.size(), "no extra sets");
    CHECK(!actions::renameSet(s.undo, s.stage, kChair, "mood", "size"), "not onto another");
    s.undo.stack(s.stage)->undo();
    CHECK(s.chair().GetVariantSets().GetNames() == before && s.look().GetVariantSelection() == "red", "undo");
}

USDCC_TEST(variant_from_a_weaker_layer) {
    Scene s;
    const SdfLayerRefPtr weak = SdfLayer::CreateAnonymous("weak.usda");
    s.root->GetSubLayerPaths().push_back(weak->GetIdentifier());
    s.usd->SetEditTarget(weak);
    const SdfPath q("/Q");
    s.usd->DefinePrim(q, TfToken("Xform"));
    actions::addSet(s.undo, s.stage, q, "mode", "a");
    s.usd->SetEditTarget(s.root);

    const UsdPrim prim = s.usd->GetPrimAtPath(q);
    CHECK(!actions::setPlace(s.stage, prim, "mode").here, "not here");
    CHECK(!actions::canRemoveSet(s.stage, prim, "mode") && !actions::canRenameSet(s.stage, prim, "mode"), "so it stays");
    CHECK(!actions::removeSet(s.undo, s.stage, q, "mode"), "and refuses");
    CHECK(actions::addVariant(s.undo, s.stage, q, "mode", "b"), "it can still gain one here");
    const std::vector<actions::VariantEntry> variants = actions::variantsOf(s.stage, prim, "mode");
    CHECK((variants.size()) == (std::size_t(2)), "count");
    CHECK(variants[0].name == "a" && !variants[0].isHere() && variants[0].isElsewhere(), "a is the other layer's");
    CHECK(variants[1].name == "b" && variants[1].isHere(), "b is here");
    CHECK(!actions::removeVariant(s.undo, s.stage, q, "mode", "a"), "a can't go from here");
    CHECK(!actions::renameVariant(s.undo, s.stage, q, "mode", "a", "c"), "nor be renamed");
    CHECK(actions::removeVariant(s.undo, s.stage, q, "mode", "b"), "b can");
    CHECK(actions::duplicateVariant(s.undo, s.stage, q, "mode", "a", "a2"), "a can be copied here");
}

USDCC_TEST(variant_edit_mode_session_selection) {
    Scene s;
    s.addLook();
    s.look().SetVariantSelection("blue");
    const std::string file = s.file();
    std::string session;
    s.usd->GetSessionLayer()->ExportToString(&session);
    int changes = 0;
    auto connection = s.stage->variantEditChanged.connect([&]() { ++changes; });

    CHECK(!s.stage->enterVariantEdit(kChair, "look", "green"), "not a variant it has");
    CHECK(s.stage->enterVariantEdit(kChair, "look", "red"), "enters");
    CHECK((changes) == (1), "count");
    CHECK(s.stage->variantEdit() && s.stage->variantEdit()->variant == "red", "says so");
    CHECK(s.look().GetVariantSelection() == "red", "selected, so it composes");
    CHECK(s.file() == file, "the file hasn't changed");
    CHECK(s.usd->GetEditTarget().MapToSpecPath(kSeat).GetString() == "/Chair{look=red}Seat", "the target maps into it");
    CHECK(s.stage->editTargetLayer() == s.root, "in the same layer");

    CHECK(s.stage->enterVariantEdit(kChair, "look", "blue"), "another variant of the set switches");
    CHECK(s.usd->GetEditTarget().MapToSpecPath(kChair).GetString() == "/Chair{look=blue}", "to it");
    CHECK(!s.stage->enterVariantEdit(kSeat, "look", "x"), "another set is refused");

    s.stage->exitVariantEdit();
    CHECK(!s.stage->variantEdit(), "leaves");
    CHECK(s.usd->GetEditTarget().GetMapFunction().IsIdentity() && s.stage->editTargetLayer() == s.root, "the plain layer");
    CHECK(s.look().GetVariantSelection() == "blue", "the authored selection is back");
    std::string after;
    s.usd->GetSessionLayer()->ExportToString(&after);
    CHECK(after == session, "the session layer is as it was");
}

USDCC_TEST(variant_edit_mode_keeps_a_session_selection) {
    Scene s;
    s.addLook();
    s.usd->SetEditTarget(s.usd->GetSessionLayer());
    s.look().SetVariantSelection("blue");
    s.usd->SetEditTarget(s.root);
    s.stage->enterVariantEdit(kChair, "look", "red");
    CHECK(s.look().GetVariantSelection() == "red", "over the session's");
    s.stage->exitVariantEdit();
    CHECK(s.look().GetVariantSelection() == "blue", "and it's put back");
}

USDCC_TEST(variant_edit_mode_left_by_any_target_change) {
    Scene s;
    s.addLook();
    s.stage->enterVariantEdit(kChair, "look", "red");
    CHECK(s.stage->setEditTargetLayer(s.root), "the same layer");
    CHECK(!s.stage->variantEdit() && s.usd->GetEditTarget().GetMapFunction().IsIdentity(), "leaves the variant");

    s.stage->enterVariantEdit(kChair, "look", "red");
    s.usd->SetEditTarget(s.usd->GetSessionLayer());  // as a script would
    CHECK(!s.stage->variantEdit(), "so does a script");
    CHECK(s.stage->editTargetLayer() == s.usd->GetSessionLayer(), "and its target stays");
    CHECK(s.look().GetVariantSelection() == "red", "the authored selection, from addSet");
}

USDCC_TEST(variant_edit_mode_edits_land_inside) {
    Scene s;
    s.addLook();
    s.stage->enterVariantEdit(kChair, "look", "blue");
    QUndoStack* stack = s.undo.stack(s.stage);
    const int steps = stack->count();

    const UsdPrim seat = s.usd->GetPrimAtPath(kSeat);
    CHECK(prim_actions::isEditable(seat) && prim_actions::isEditable(s.chair()), "the prim and under it");
    CHECK(s.undo.apply(s.stage, QStringLiteral("Edit"), [&]() { return seat.GetAttribute(TfToken("size")).Set(5.0); }), "edits");
    CHECK(s.root->GetAttributeAtPath(SdfPath("/Chair{look=blue}Seat.size")), "into the variant");
    CHECK(!s.root->GetAttributeAtPath(SdfPath("/Chair/Seat.size")), "not beside it");
    CHECK((stack->count()) == (steps + 1), "count");

    stack->undo();
    CHECK(!s.root->GetAttributeAtPath(SdfPath("/Chair{look=blue}Seat.size")), "undo in the mode");
    stack->redo();
    CHECK(s.root->GetAttributeAtPath(SdfPath("/Chair{look=blue}Seat.size")), "redo in the mode");
    s.stage->exitVariantEdit();
    stack->undo();
    CHECK(!s.root->GetAttributeAtPath(SdfPath("/Chair{look=blue}Seat.size")), "undo after leaving");
    stack->redo();
    CHECK(s.root->GetAttributeAtPath(SdfPath("/Chair{look=blue}Seat.size")), "redo after leaving");
}

USDCC_TEST(variant_edit_mode_guards) {
    Scene s;
    s.addLook();
    actions::addSet(s.undo, s.stage, kChair, "size", "big");
    actions::addVariant(s.undo, s.stage, kChair, "size", "small");
    s.stage->enterVariantEdit(kChair, "look", "red");

    // Trap 1: USD would write this to the plain layer, and report success.
    CHECK(!prim_actions::isEditable(s.usd->GetPrimAtPath(SdfPath("/Table"))), "outside it can't be edited");
    CHECK(!prim_actions::setActive(s.undo, s.stage, {SdfPath("/Table")}, false), "and the action refuses");
    CHECK(s.usd->GetPrimAtPath(SdfPath("/Table")).IsActive(), "nothing happened");

    // Trap 2: a selection under the target.
    CHECK(prim_actions::setVariant(s.undo, s.stage, kChair, "look", "blue"), "the edited set's combo");
    CHECK(s.stage->variantEdit()->variant == "blue", "moves the mode");
    CHECK(s.look().GetVariantSelection() == "blue", "and what composes");
    CHECK(!s.contains("{look=red}look"), "writes nothing inside");
    CHECK(prim_actions::setVariant(s.undo, s.stage, kChair, "size", "small"), "another set's");
    CHECK(s.chair().GetVariantSet("size").GetVariantSelection() == "small", "takes effect");
    CHECK(!prim_actions::clearVariant(s.undo, s.stage, kChair, "look"), "clearing the edited one is refused");

    // Trap 3.
    CHECK(!prim_actions::canRename(s.usd->GetPrimAtPath(kSeat)), "UsdNamespaceEditor refuses this target");
    CHECK(!prim_actions::rename(s.undo, s.stage, kSeat, "Other"), "so rename does");
    s.stage->exitVariantEdit();
    CHECK(prim_actions::canRename(s.usd->GetPrimAtPath(kSeat)), "back to normal");
    CHECK(prim_actions::isEditable(s.usd->GetPrimAtPath(SdfPath("/Table"))), "here too");
}

USDCC_TEST(variant_managing_during_edit_mode) {
    Scene s;
    s.addLook();
    s.paintRed();
    s.stage->enterVariantEdit(kChair, "look", "red");
    const std::vector<actions::VariantEntry> entries = actions::variantsOf(s.stage, s.chair(), "look");
    CHECK(entries.size() == 2 && entries[0].isHere() && entries[1].isHere(), "the set is still on the plain prim");

    CHECK(actions::addVariant(s.undo, s.stage, kChair, "look", "green"), "adding works");
    CHECK(s.root->GetObjectAtPath(SdfPath("/Chair{look=green}")), "in the set, not nested in red");
    CHECK(!s.root->GetObjectAtPath(SdfPath("/Chair{look=red}{look=green}")), "not there");
    CHECK(!actions::removeVariant(s.undo, s.stage, kChair, "look", "red"), "the edited one can't be removed");
    CHECK(!actions::renameVariant(s.undo, s.stage, kChair, "look", "red", "x"), "nor renamed");
    CHECK(actions::removeVariant(s.undo, s.stage, kChair, "look", "blue"), "another one can");
    CHECK(!actions::removeSet(s.undo, s.stage, kChair, "look"), "nor the set");

    // A set made now nests: USD's standard shape, for another set.
    CHECK(actions::addSet(s.undo, s.stage, kChair, "finish", "matte"), "a new set");
    CHECK(s.root->GetObjectAtPath(SdfPath("/Chair{look=red}{finish=matte}")), "nests in the variant being edited");
}

USDCC_TEST(variant_edit_mode_arcs_and_xform) {
    Scene s;
    s.addLook();
    s.stage->enterVariantEdit(kChair, "look", "red");
    CHECK(prim_actions::addArc(s.undo, s.stage, kChair, usdcc::usd::ArcInfo::Type::Inherit, std::string(), SdfPath("/Table")), "adds an arc");
    bool found = false;
    for (const usdcc::usd::ArcInfo& arc : usdcc::usd::compositionArcs(s.chair())) {
        if (arc.type == usdcc::usd::ArcInfo::Type::Inherit) found = arc.editable;
    }
    CHECK(found, "it's editable while it's where edits go");
    s.stage->exitVariantEdit();
    found = true;
    for (const usdcc::usd::ArcInfo& arc : usdcc::usd::compositionArcs(s.chair())) {
        if (arc.type == usdcc::usd::ArcInfo::Type::Inherit) found = arc.editable;
    }
    (void)found;

    // Removing an op whose attribute is in the plain layer keeps it: the variant
    // holds no opinion of it to delete.
    const UsdPrim seat = s.usd->GetPrimAtPath(kSeat);
    usdcc::usd::XformStack stack(seat);
    CHECK(stack.add(UsdGeomXformOp::TypeTranslate, 0).has_value(), "a plain op");
    s.stage->enterVariantEdit(kChair, "look", "blue");
    CHECK(usdcc::usd::XformStack(seat).remove(0), "removed in the variant");
    CHECK(s.root->GetAttributeAtPath(SdfPath("/Chair/Seat.xformOp:translate")), "the plain attribute stays");
}

USDCC_TEST(variant_opinions_read_and_remove) {
    Scene s;
    s.addLook();
    s.stage->enterVariantEdit(kChair, "look", "red");
    const UsdPrim seat = s.usd->GetPrimAtPath(kSeat);
    seat.GetAttribute(TfToken("size")).Set(3.0);
    s.chair().CreateAttribute(TfToken("note"), SdfValueTypeNames->String).Set(std::string("in red"));
    s.usd->DefinePrim(SdfPath("/Chair/Extra"), TfToken("Sphere"));
    s.chair().SetMetadata(TfToken("hidden"), true);
    s.stage->exitVariantEdit();

    const SdfPath red("/Chair{look=red}");
    const std::vector<actions::Opinion> tree = actions::variantOpinions(s.root, red);
    auto find = [&](const std::string& label) -> const actions::Opinion* {
        for (const actions::Opinion& o : tree) {
            if (o.label == label) return &o;
        }
        return nullptr;
    };
    CHECK(find("note") && find("note")->value.find("in red") != std::string::npos, "an attribute, with its value");
    CHECK(find("hidden") && find("hidden")->kind == actions::Opinion::Kind::Metadata, "metadata");
    CHECK(find("Extra") && find("Extra")->value == "def Sphere", "a prim it defines");
    const actions::Opinion* seatRow = find("Seat");
    CHECK(seatRow && seatRow->value == "over" && seatRow->children.size() == 1 && seatRow->children[0].label == "size", "an override, with what's in it");
    CHECK((actions::opinionCount(tree)) == (5), "count");
    CHECK(actions::variantOpinions(s.root, SdfPath("/Chair{look=blue}")).empty(), "blue holds nothing, unselected or not");

    // The last opinion under an `over` takes the `over` too, but never the variant.
    CHECK(actions::removeOpinion(s.undo, s.stage, s.root, seatRow->children[0]), "removes the value");
    CHECK(!s.root->GetPrimAtPath(SdfPath("/Chair{look=red}Seat")), "and the empty over");
    CHECK(s.root->GetObjectAtPath(red), "the variant stays");
    s.undo.stack(s.stage)->undo();
    CHECK(s.root->GetAttributeAtPath(SdfPath("/Chair{look=red}Seat.size")), "undo brings both back");

    CHECK(actions::removeOpinion(s.undo, s.stage, s.root, *find("hidden")), "metadata");
    CHECK(actions::removeOpinion(s.undo, s.stage, s.root, *find("note")), "an attribute");
    CHECK(actions::removeOpinion(s.undo, s.stage, s.root, *find("Extra")), "a prim");
    CHECK((actions::opinionCount(actions::variantOpinions(s.root, red))) == (2), "count");
    CHECK(s.root->GetObjectAtPath(red), "emptying it keeps the variant");
}
