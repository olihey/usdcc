# Reordering and Reparenting in the Outliner

Drag prims in the Outliner: between siblings to reorder them, onto another prim to reparent them, each as one undo step.

Moving a prim in USD looks like a small edit but isn't. A prim isn't one thing in one file: it's opinions in several layers, under
variants, and behind arcs, and other prims point at its path. `UsdNamespaceEditor` handles much of that, but not all of it, and
what it does goes beyond what the app's undo records. Everything marked below was run against this build (USD 26.11).

## 1. How it works

- **Dragging a row:** onto another row reparents, between rows reorders or moves to that position, onto the empty space moves to the root.
  **Alt** on the drop keeps the local transform instead of the world position. Several selected rows move together.
- **Move Up and Move Down** in the prim menu use the same code.
- **`prim_actions::movePrims()`** does the move, `whyNotMove()` and `whyNotDrop()` say why not, and `moveNotes()` what it would do
  beyond the edit target.
- **`UndoManager::applyAcrossLayers()`** records every layer of the stage and the other open stages as one step. Rename uses it too.
- **`Stage::primsMoved`** announces the move, on undo and redo as well.

## 2. What `UsdNamespaceEditor` takes care of

Checked, with `MovePrimAtPath`:

| Pointing at the moved prim | Result |
|---|---|
| A relationship's target | Follows |
| An attribute's connection | Follows |
| An inherit | Follows |
| An internal reference | Follows, and still composes |
| A collection's includes and excludes | Follow |
| The layer's `defaultPrim` | Follows, becoming a path such as `/Pa/Top`, which a reference still finds |
| Its subtree, and arcs on the prim itself | Move with it |
| A prim with its own variant sets, moved whole | Moves, with its variants |
| An instance | Moves, and stays an instance |

It also refuses, with a reason: a name already taken, a move into its own descendant, instance proxies, and a layer that can't be edited.

## 3. The issues

### 3.1 It edits layers other than the edit target, and undo doesn't record them

**This was the biggest issue, and it was already a bug in Rename, now fixed.** With the `def` in a sublayer and the edit target on the root layer, the
move rewrites **both** layers, and also the session layer. It does so even when the edit target holds nothing for the prim.

`UndoManager` used to record only the edit target's layer, so after Undo the prim stayed renamed in the sublayer, and the root layer's
`over` was gone.

**So:** these edits record every used layer of the stage and the other open stages, as one step.

It also means **a drag can mark other files as unsaved,** so such a drop asks first (§6).

### 3.2 Opinions in variants get left behind

The namespace editor only moves what's in the selected composition:

- **A prim overridden in an unselected variant splits.** Moving `/Chair/Seat` with `blue` selected, `red`'s `over "Seat"` stays. Selecting
  `red` then brings `/Chair/Seat` back, with the override, while `/Else/Seat` has lost it: `size` reads 2, not 3. Nothing reports this.
- **Overridden in the selected variant,** or existing only in one, it can't move the opinion, so it authors a `relocates` instead.

**So:** `whyNotMove()` looks for these itself, walking the variants of the prim's ancestors in the local layers, and refuses, naming
the variant. Moving the prim that owns the variant set is fine, since its variants go with it.

### 3.3 Prims that come from a reference

Moving `/Inst/Leg`, whose `def` is in the referenced file, can't change that file, so USD authors **layer metadata** instead:
`relocates = { </Inst/Leg>: </Dest/Leg> }`. It works, keeps local overrides, and moving it back removes the entry. But it's a different
kind of edit, applied to anything under a reference or payload, and it's a composition feature to be careful with.

One sequence of moves with relocates and variants on one stage ended in a USD verification failure in `CanApplyEdits`
("Unable to find node that introduced dependency"). It hasn't been reduced to a minimal case. The refusal in §3.2 now stops the variant
half of that sequence, but relocates are the part of this to treat with the most care.

**So:** it's allowed, and the drop says so first (§6). Failures are caught with a `TfErrorMark` and become a refusal, with the whole step rolled
back. Moving the prim holding the reference is ordinary and works.

### 3.4 Other open stages

If another open stage references this layer, and has an override on the moved prim, the override is stranded: it no longer applies, and
nothing says so. `AddDependentStage()` fixes that (checked: the override moved to the new path), which also edits **that stage's** layers.

**So:** every open stage from `StageManager` is added as a dependent, and undo records their layers too. **Files not open can't be
fixed:** a closed file with an override on the old path silently stops applying. That's true of Rename today as well.

### 3.5 Muted layers

A muted layer isn't touched. On unmuting, its opinion brings the old path back (checked: `/G/K` returns, holding its attribute).

**So:** it's refused when a muted layer has opinions at the path, naming it.

### 3.6 The world transform changes

Reparenting keeps the local transform, so the prim jumps: from (11, 0, 0) to (1, 5, 0) in my test. Most DCCs keep it where it is, by
changing its local transform. That's an authored edit, it can land in the edit target over a weaker layer's value, and it's only simple
when nothing's animated. `XformStack` and `xform_math` have what's needed for the static case.

**So:** a drop keeps the world position by default (§6), through `XformStack::setLocalTransform()`. **That replaces the prim's op stack**
with translate, rotate and scale, or one matrix if there's shear, so pivots are flattened. It only does so when the parent's transform
actually differs. It's skipped, and logged, when the prim or a parent is animated (more than one time sample). A prim with
`resetXformStack` has nothing to change. A prim with no transform of its own, such as a Scope, can't be compensated, so its children
will change place.

### 3.7 Child order is fragile

- **Stale names:** the namespace editor doesn't update `reorder nameChildren`. After moving or renaming a child the old name stays in the
  list, and the child loses its place.
- **Children not listed** keep their place among the listed ones rather than going last: reorder `[B, A]` with a later `D` gives `B, C, D, A`.
- **`OverridePrim` authors nothing where the prim exists,** which made Move Up do nothing for a weaker layer's children. It now uses
  `SdfCreatePrimInLayer`.
- **Reordering is an opinion like any other:** an `over` in the edit target can reorder a weaker layer's children or a reference's
  (both checked), and it wins, so it's usable everywhere, and it creates an `over` where there was none.

**So:** it writes the whole list, and removes or renames the name in every layer's list when a child leaves or changes name. Dropping a prim at a position
in another parent is two edits, the move and then the order, in one undo step.

### 3.8 Everything holding a path

After a move the old path means nothing. These have to follow it, on undo and redo as well:

- **The selection,** which the Attribute Editor and viewport follow. `onPrimRenamed` does this for rename, and only forwards.
- **The Outliner's expanded rows,** saved by path, so a moved subtree would collapse.
- **The transform tool's target op** (`Stage::m_targetPrim`), which is otherwise cleared.
- **An open Variant Editor,** which closes when its prim goes. That's acceptable.

**So:** `Stage::primsMoved` carries the old and new paths, fired from the undo step itself so that undo and redo announce the move too.
Rename uses it, which also fixed its selection after undo.

### 3.9 The drag itself

- **Three drops:** on a row reparents, between rows reorders or reparents to that position, and on empty space moves to the root.
- **Several prims:** one undo step, keeping their tree order. A selected prim whose ancestor is also selected goes along with it, and isn't
  moved separately. Name clashes refuse the whole drop. **Checked: an editor holds one edit.** With two declared, it applies the last and
  reports success, so each prim gets its own editor, and a failure rolls back the whole step.
- **Refused drops** show during the hover: the view refuses, and a line at the bottom of the panel says why. The hover uses the quick
  checks, with USD's own on the drop. *The hover's speed on a big stage hasn't been measured.*
- **The tree rebuilds** after each edit, so the drop doesn't touch the model itself: it requests the edit, as rename does.
- **A name in use under the new parent** refuses, rather than renaming as Create does.

### 3.10 Where a move isn't allowed

Refused, with the reason in the panel during the hover, and in the log:

| Case | Why |
|---|---|
| An instance proxy | USD refuses |
| Onto an instance proxy, or an inactive or unloaded prim | Checked: USD accepts under an inactive prim, and the prim then vanishes from the Outliner. `whyNotCreateUnder()` says this already |
| During variant edit mode | USD refuses reparenting under a mapped edit target. Reordering inside the variant still works |
| Opinions in a variant (§3.2) or a muted layer (§3.5) | They'd be left behind |
| A layer without edit permission | USD refuses |

An inactive prim can itself be moved.

## 4. Tested, and not

The tests go through what the view calls (`canDropMimeData`, `dropMimeData`), as Qt's drag runs a blocking loop of its own. **A real mouse
drag hasn't been exercised,** nor the feel of the drop indicator. Alt is read when the drop happens, and hasn't been pressed in a test.

## 5. Not checked yet

- **Speed on a large stage,** for the hover and the edit. An edit records every used layer of every open stage.
- **The USD verification failure** in §3.3, not reduced to a minimal case.
- **Closed files** with overrides on the old path: nothing can tell.

## 6. Decisions

- **The transform:** keep the world position when nothing relevant is animated. Alt keeps the local transform.
- **A drag that changes other files:** asks for that drop, naming the layers, with "Don't ask again" (`Outliner/askBeforeMovingAcrossLayers`).
  Rename doesn't ask, as it did not before.
- **Prims under a reference or payload:** allowed through `relocates`, saying so first. The same question, and the same "Don't ask again".
  Reordering them changes only an opinion, so it doesn't ask.
- **A name taken in the new parent:** refused, saying which name.
