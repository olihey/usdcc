// The selection in the undo history (UndoManager::trackSelection()): a change is a
// step of its own, one made with an edit is part of that edit's step, quick changes
// are one step, and nothing is recorded during an undo, a redo or a pause. Runs in
// usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/session_stages.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QUndoStack>

#include <initializer_list>
#include <vector>

using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
namespace prim_actions = usdcc::usd::prim_actions;
namespace session_stages = usdcc::usd::session_stages;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
using Paths = std::vector<SdfPath>;

Paths paths(std::initializer_list<const char*> list) {
    Paths result;
    for (const char* path : list) result.emplace_back(path);
    return result;
}

// /A, /B and /C on a stage whose selection is tracked, each change a step of its own.
struct Scene {
    StageManager manager;
    UndoManager undo;
    StageRefPtr stage;
    explicit Scene(bool track = true) {
        if (track) undo.trackSelection(manager);
        undo.setSelectionMergeInterval(0);
        stage = manager.createStage();
        for (const char* path : {"/A", "/B", "/C"}) UsdGeomCube::Define(stage->usdStage(), SdfPath(path)).GetSizeAttr().Set(1.0);
    }
    QUndoStack* stack() { return undo.stack(stage); }
    void select(std::initializer_list<const char*> list) { manager.setSelectedPaths(stage, paths(list)); }
    bool selected(std::initializer_list<const char*> list) const { return manager.selectedPaths(stage) == paths(list); }
    bool has(const char* path) const { return static_cast<bool>(stage->usdStage()->GetPrimAtPath(SdfPath(path))); }
    double size(const char* path) const {
        double value = 0.0;
        UsdGeomCube(stage->usdStage()->GetPrimAtPath(SdfPath(path))).GetSizeAttr().Get(&value);
        return value;
    }
    void setSize(const char* path, double value) {
        UsdGeomCube(stage->usdStage()->GetPrimAtPath(SdfPath(path))).GetSizeAttr().Set(value);
    }
};
}  // namespace

USDCC_TEST(selection_undo_is_a_step) {
    Scene s;
    CHECK(s.stack()->count() == 0, "defining prims outside undo records nothing");
    s.select({"/A"});
    CHECK(s.stack()->count() == 1 && s.stack()->undoText() == "Select A", "one step, named for the prim (%s)",
          qPrintable(s.stack()->undoText()));
    s.select({"/A", "/B"});
    CHECK(s.stack()->undoText() == "Select 2 Prims", "several prims (%s)", qPrintable(s.stack()->undoText()));
    s.select({});
    CHECK(s.stack()->count() == 3 && s.stack()->undoText() == "Deselect All", "none (%s)", qPrintable(s.stack()->undoText()));
    s.select({});
    CHECK(s.stack()->count() == 3, "the same selection again is no step");

    s.stack()->undo();
    CHECK(s.selected({"/A", "/B"}), "undo: both again");
    s.stack()->undo();
    CHECK(s.selected({"/A"}), "undo: the first");
    s.stack()->undo();
    CHECK(s.selected({}), "undo: none, as at the start");
    CHECK(s.stack()->count() == 3 && s.stack()->index() == 0, "undoing records nothing (count=%d)", s.stack()->count());
    s.stack()->redo();
    CHECK(s.selected({"/A"}), "redo");
    s.stack()->redo();
    CHECK(s.selected({"/A", "/B"}) && s.stack()->count() == 3, "redo records nothing either");

    // Like any step, a new one takes the place of what could be redone.
    s.stack()->undo();
    s.select({"/C"});
    CHECK(s.stack()->count() == 2 && !s.stack()->canRedo(), "selecting after an undo drops the redo");
}

USDCC_TEST(selection_undo_with_an_edit) {
    Scene s;
    s.select({"/A"});

    // Changed during the edit.
    CHECK(s.undo.apply(s.stage, "Grow", [&]() {
              s.setSize("/A", 2.0);
              s.select({"/B"});
              return true;
          }),
          "applies");
    CHECK(s.stack()->count() == 2 && s.stack()->undoText() == "Grow", "the edit and its selection are one step (count=%d)",
          s.stack()->count());
    s.stack()->undo();
    CHECK(s.size("/A") == 1.0 && s.selected({"/A"}), "undo: the size and the selection");
    s.stack()->redo();
    CHECK(s.size("/A") == 2.0 && s.selected({"/B"}), "redo: both");

    // Changed once the edit is done: a scope around the two.
    SdfPath made;
    {
        const UndoManager::SelectionScope scope(s.undo, s.stage);
        made = prim_actions::createPrim(s.undo, s.stage, SdfPath::AbsoluteRootPath(), "Sphere");
        s.manager.setSelectedPaths(s.stage, {made});
    }
    CHECK(!made.IsEmpty() && s.stack()->count() == 3, "create and select: one step (count=%d)", s.stack()->count());
    s.stack()->undo();
    CHECK(!s.stage->usdStage()->GetPrimAtPath(made) && s.selected({"/B"}), "undo: the prim is gone, the selection back");
    s.stack()->redo();
    CHECK(s.stage->usdStage()->GetPrimAtPath(made) && s.manager.selectedPaths(s.stage) == Paths{made}, "redo: made and selected");

    // A scope without an edit, and one without anything.
    {
        const UndoManager::SelectionScope scope(s.undo, s.stage);
        s.select({"/A"});
        s.select({"/C"});
    }
    CHECK(s.stack()->count() == 4 && s.stack()->undoText() == "Select C", "the changes of a scope are one step");
    { const UndoManager::SelectionScope scope(s.undo, s.stage); }
    CHECK(s.stack()->count() == 4, "an empty scope is no step");
    s.stack()->undo();
    CHECK(s.manager.selectedPaths(s.stage) == Paths{made}, "undo goes back to before the scope");

    // An edit that fails leaves the selection as it was, too.
    CHECK(!s.undo.apply(s.stage, "Fails", [&]() {
              s.setSize("/A", 9.0);
              s.select({"/A"});
              return false;
          }),
          "a failing edit");
    CHECK(s.size("/A") == 2.0 && s.manager.selectedPaths(s.stage) == Paths{made} && s.stack()->index() == 3,
          "is rolled back with its selection, and is no step");
    s.undo.beginEdit(s.stage, "Drag");
    s.select({"/B"});
    s.undo.cancelEdit(s.stage);
    CHECK(s.manager.selectedPaths(s.stage) == Paths{made} && s.stack()->index() == 3, "so is a cancelled session");
}

USDCC_TEST(selection_undo_follows_a_rename) {
    Scene s;
    s.select({"/A"});
    CHECK(prim_actions::rename(s.undo, s.stage, SdfPath("/A"), "Z"), "renames");
    CHECK(s.selected({"/Z"}), "the selection follows");
    CHECK(s.stack()->count() == 2 && s.stack()->undoText() != "Select Z", "as part of the rename's step (count=%d, %s)",
          s.stack()->count(), qPrintable(s.stack()->undoText()));
    s.stack()->undo();
    CHECK(s.has("/A") && s.selected({"/A"}), "undo: the old name, selected");
    CHECK(s.stack()->count() == 2 && s.stack()->canRedo(), "and no step recorded while undoing");
    s.stack()->redo();
    CHECK(s.has("/Z") && s.selected({"/Z"}) && s.stack()->count() == 2, "redo: the new name, selected");
}

USDCC_TEST(selection_undo_delete_and_missing_prims) {
    Scene s;
    s.select({"/A", "/B"});
    {
        // What deletePrimsAsking() does: delete, then drop what's gone.
        const UndoManager::SelectionScope scope(s.undo, s.stage);
        CHECK(prim_actions::deletePrims(s.undo, s.stage, {SdfPath("/A")}).isEmpty(), "deletes");
        s.select({"/B"});
    }
    CHECK(!s.has("/A") && s.stack()->count() == 2, "one step (count=%d)", s.stack()->count());
    s.stack()->undo();
    CHECK(s.has("/A") && s.selected({"/A", "/B"}), "undo brings the prim back, selected");
    s.stack()->redo();
    CHECK(!s.has("/A") && s.selected({"/B"}), "redo");

    // A prim that went some other way isn't selected again.
    s.select({"/C"});
    s.stage->usdStage()->RemovePrim(SdfPath("/B"));
    s.stack()->undo();
    CHECK(s.selected({}), "undo leaves out a prim that's gone");
}

USDCC_TEST(selection_undo_merges_quick_changes) {
    Scene s;
    s.undo.setSelectionMergeInterval(60 * 60 * 1000);
    s.select({"/A"});
    s.select({"/A", "/B"});
    s.select({"/B"});
    CHECK(s.stack()->count() == 1 && s.stack()->undoText() == "Select B", "one step, named for where it ended (count=%d, %s)",
          s.stack()->count(), qPrintable(s.stack()->undoText()));
    s.stack()->undo();
    CHECK(s.selected({}), "undo goes back to before the first");
    s.stack()->redo();
    CHECK(s.selected({"/B"}), "redo to after the last");
    s.select({});
    CHECK(s.stack()->count() == 0 && !s.stack()->canUndo(), "back where it began: no step is left (count=%d)", s.stack()->count());

    // An edit between two changes keeps them apart.
    s.select({"/A"});
    CHECK(s.undo.apply(s.stage, "Grow", [&]() { s.setSize("/A", 2.0); return true; }), "an edit");
    s.select({"/B"});
    CHECK(s.stack()->count() == 3, "select, edit, select (count=%d)", s.stack()->count());

    s.undo.setSelectionMergeInterval(0);
    s.select({"/C"});
    s.select({"/A"});
    CHECK(s.stack()->count() == 5, "with no interval every change is a step (count=%d)", s.stack()->count());
}

USDCC_TEST(selection_undo_pause_and_scripts) {
    Scene s;
    {
        const UndoManager::SelectionPause pause;
        s.select({"/A"});
    }
    CHECK(s.stack()->count() == 0, "a paused change is no step");
    s.select({"/B"});
    s.stack()->undo();
    CHECK(s.selected({"/A"}), "but the next one knows where it came from");
    s.stack()->redo();

    // A script run is a global edit: its selection is in its step, or a step of its own.
    s.undo.beginGlobalEdit(s.manager, "Script");
    s.select({"/C"});
    CHECK(s.undo.endGlobalEdit() == 0, "a script that only selects records no edit");
    CHECK(s.stack()->count() == 2 && s.stack()->undoText() == "Select C", "but its selection (count=%d, %s)", s.stack()->count(),
          qPrintable(s.stack()->undoText()));
    s.undo.beginGlobalEdit(s.manager, "Script");
    s.setSize("/A", 3.0);
    s.select({"/A"});
    s.select({"/A", "/B"});
    CHECK(s.undo.endGlobalEdit() == 1, "a script that edits and selects");
    CHECK(s.stack()->count() == 3 && s.stack()->undoText() == "Script", "is one step (count=%d)", s.stack()->count());
    s.stack()->undo();
    CHECK(s.size("/A") == 1.0 && s.selected({"/C"}), "undone as one");
    s.stack()->redo();
    CHECK(s.size("/A") == 3.0 && s.selected({"/A", "/B"}), "and redone");
}

USDCC_TEST(selection_undo_is_opt_in) {
    Scene s(false);
    s.select({"/A"});
    CHECK(s.stack()->count() == 0, "a manager that doesn't track records no selection");
    CHECK(s.undo.apply(s.stage, "Grow", [&]() {
              s.setSize("/A", 2.0);
              s.select({"/B"});
              return true;
          }),
          "applies");
    {
        const UndoManager::SelectionScope scope(s.undo, s.stage);
        s.select({"/C"});
    }
    CHECK(s.stack()->count() == 1, "only the edit (count=%d)", s.stack()->count());
    s.stack()->undo();
    CHECK(s.size("/A") == 1.0 && s.selected({"/C"}), "and its undo leaves the selection alone");
}

// A session's selection is how the stage was left, not something to undo.
USDCC_TEST(selection_undo_not_for_a_restored_session) {
    Scene s;
    QJsonObject state = session_stages::capture(s.manager, s.stage);
    state.insert("selection", QJsonArray{"/A", "/C"});
    session_stages::apply(s.manager, s.stage, state, nullptr);
    CHECK(s.selected({"/A", "/C"}), "restored");
    CHECK(s.stack()->count() == 0 && !s.stack()->canUndo(), "with nothing to undo (count=%d)", s.stack()->count());
    s.select({"/B"});
    s.stack()->undo();
    CHECK(s.selected({"/A", "/C"}), "and the first change goes back to it");
}
