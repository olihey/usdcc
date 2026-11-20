# Deleting Prims

Delete prims from the Outliner or the viewport, with **Delete** on the selection or from the prim menu, as one undo step.

Deleting looks like the simplest edit, but USD has no "delete" for a prim. A prim is opinions in several layers and behind arcs. The
edit target can only remove its own opinions. Whatever any other layer or arc says stays, and the prim stays with it. The only
opinion that makes a prim go away from wherever it comes is `active = false`. This page is about choosing between the two, and about
what still points at a prim once it's gone.

## 1. How it works

- **Delete** (the key, in the Outliner or the viewport) and **Delete** in the prim menu act on the stage's selection. A prim whose
  ancestor is selected too goes with it and isn't counted on its own.
- **In the viewport** the native window gets the key, not its panel: `HydraViewportWindow` emits `deleteRequested()`, except during a
  gizmo drag, whose edit is still open. A click in the viewport now takes the keyboard (`requestActivate()`), which a native child
  window doesn't do by itself. Undo and the other window shortcuts still work there, since Qt still counts the main window active.
- **`prim_actions::planDelete()`** says, for each prim, what the edit target can do and why (§2), without changing anything.
  **`prim_actions::deletePrims()`** does it, with a `DeleteMode` for the prims the edit target can't remove.
- **`deletePrimsAsking()`** (prim_context_menu.h) is what the key and the menu call. It asks only when some prim can't simply be
  removed (§4), then drops the deleted prims from the selection.
- **One undo step,** named for what it did: "Delete Chair", "Deactivate Chair", "Delete 3 Prims". It records every layer it changes
  (`UndoManager::applyAcrossLayers()`), as Move does.

## 2. Each prim falls into one case

From the specs that keep the prim there, read from its prim index: the local layers' specs at its path, and those from arcs on its
**ancestors** (a reference on its parent, its parent's variant). **Not** `GetPrimStack()` as such, which also lists the specs from the
prim's **own** arcs: a prim holding a reference then looked as if it came from one. Its own arcs go with the opinions that author them,
so they don't count.

A child spec needs a parent spec in the same layer, so a descendant's opinions in another layer show up at its ancestor too, as an
`over`. Looking at the prim covers its whole subtree.

| Case | What's there | What happens |
|---|---|---|
| **Only in the edit target** | One spec, the edit target's, at the prim's path | `RemovePrim`. It's gone, with its subtree |
| **In other local layers too** | Also specs in other layers of the root layer stack (sublayers, the session layer) | Ask: **Deactivate**, or **Delete from All Layers** |
| **From an ancestor's arc** | A spec in a file its parent references, or inside its parent's variant | Only deactivation can remove it. The dialog says where it comes from |
| **Instance proxy**, or out of a variant edit target's reach | — | Skipped (`isEditable()`) |
| **A stronger layer authors `active = true`** | — | Can't be deactivated from here. Skipped, naming the layer |

"Delete from All Layers" is offered only when every spec is a plain one, at the prim's path, in an editable local layer. A prim from
an arc can't have that file changed from here.

## 3. The issues

### 3.1 `RemovePrim` alone is a trap

With the `def` in a sublayer and an `over` in the edit target, `RemovePrim` removes the `over`. The prim stays, and loses the overrides
the user made. To the user, Delete did nothing, or it undid their work.

**So:** a prim the edit target can't remove completely isn't touched by `RemovePrim` at all. It's deactivated, and its overrides stay,
so activating it again brings it back as it was.

The Material Editor's Delete (`shade_actions::deleteNodes()`) used to do exactly this: remove, then deactivate whatever was left. It now
uses the same helper, `prim_actions::deleteInEditTarget()`, and keeps the overrides too. It still doesn't ask: deleting a shader node
from a weaker layer deactivates it, as before.

### 3.2 Deactivating isn't about the `def`'s layer

`active = false` doesn't compete with the layer that defines the prim. It competes only with other opinions on `active`. Since almost
no one authors `active = true`, a weaker layer can deactivate a prim that a stronger layer defines.

**So:** the plan looks for the strongest `active` opinion in the prim stack. When it's `true` and in a layer stronger than the edit
target, the prim is skipped, naming that layer. After the edit, each prim is checked to be inactive anyway. If one isn't, the whole step
is rolled back, saying which.

### 3.3 What points at it

After a removal, other prims can still point at the old path: a relationship target (a material binding, a collection's includes or
excludes, a skeleton binding) or an attribute connection. USD reports nothing, and a binding to nothing renders without a material.

**So:** in the same undo step, each of those targets and connections is removed **in the edit target,** with
`UsdRelationship::RemoveTarget()` and `UsdAttribute::RemoveConnection()`. An explicit list (`rel material:binding = </Mat>`) loses the
entry. Any other list also gets a `delete` entry, so a target a weaker layer authored goes too, without touching that layer. A relationship left with no targets, whose only
opinion is the edit target's, is removed, as `removeRelationshipTarget()` does. Instance proxies and prims outside a variant edit
target's reach can't be changed. Those, and targets a stronger layer authors, are logged as still pointing at it.

**Not for deactivation:** the prim still exists, and activating it should bring its bindings back too.

**Composition arcs aren't changed:** an internal reference, inherit or specialize pointing at the deleted prim is left as it is.
Removing an arc changes the other prim's composition far more than a dangling target does. *Whether USD's composition warning about
it reaches the Log panel hasn't been checked.*

### 3.4 The default prim

Removing the root layer's `defaultPrim` leaves the metadata pointing at nothing, and anything referencing this file without a prim path
breaks.

**So:** when the prim the root layer names as its default prim is removed, `defaultPrim` is cleared in the same step. Deactivating
leaves it, since the prim is still there.

### 3.5 Child order

A removed prim's name stays in its parent's `reorder nameChildren` list. USD ignores it, but a new prim of the same name would take
its old place.

**So:** the name is removed from the order lists of the layers the step records.

### 3.6 What's left after a delete

- **Inactive prims stay in the Outliner,** greyed, as they do when deactivated by hand. That shows the prim is still there, in a lower
  layer, and can be activated again.
- **The selection:** deleted prims are dropped from it. Deactivated ones stay selected.
- **Undo** brings the prims back, selected as they were: the delete and what it does to the selection are one step
  (`UndoManager::SelectionScope`).

### 3.7 Muted layers

A muted layer isn't part of the prim stack, so it's neither checked nor changed. A `def` there brings the prim back when it's unmuted,
unless the prim was deactivated. That's the same as any other edit while a layer is muted.

## 4. The dialog

Shown only when some prim can't simply be removed, or is skipped. For one prim:

```
 Delete Chair
 ───────────────────────────────────────────────
 Chair is also in props.usda, which the edit target (shot.usda) can't change.

 Deactivating it hides it and keeps its overrides, so it can be activated again.

          [ Deactivate ]  [ Delete from All Layers ]  [ Cancel ]
```

For several, one line per prim that needs it, and one for those skipped. The prims the edit target can remove are removed whichever
button is chosen. "Delete from All Layers" names the layers in its tooltip, and appears only when it's possible for all of them. It
marks those files as unsaved.

## 5. Decisions

- **Ask only when it matters.** A prim the edit target holds alone is just removed: undo is there. Asking every time trains people to
  click through.
- **Deactivate is the default button.** It changes only the edit target, and it's undone by activating as well as by undo.
- **No "Don't ask again".** The choice between deactivating and changing other files shouldn't be made once for all prims.
- **Pointers are cleaned up in the edit target only.** Changing other layers for them would spread one delete across files the user
  didn't pick.
- **Scripting and plugins** call `deletePrims()` with a `DeleteMode` and get the plan's reasons back, since there's no one to ask.

## 6. Tested

`prim_delete_test.cpp` (usdcc_tests) checks each case of §2 and each issue of §3 against this build, with undo and redo where they
apply:

- Only in the edit target: removed with its subtree. Undo restores the layer exactly.
- Defined in a weaker layer: deactivated, the override kept, the weaker layer untouched.
- A weaker edit target deactivates a `def` in a stronger layer (§3.2).
- A stronger `active = true`: refused, nothing changed. Delete from All Layers still removes it.
- Delete from All Layers: gone from both layers, one step, undo restores both files exactly.
- From a reference and from a parent's variant: deactivated, even when Delete from All Layers is asked for. The prim holding the
  reference, or the variant set, is removable.
- Several prims: the topmost only, in tree order, one step, mixed removal and deactivation.
- Instance proxies skipped. The instance itself is removable.
- Pointers (§3.3): a binding with only that target goes, another target stays, a connection goes, a weaker layer's target goes without
  touching that layer. Deactivating leaves a binding.
- The default prim (§3.4) and child order (§3.5).

`outliner_context_menu_test.cpp` (usdcc_ui_tests) checks the menu entry, the question's text, each of its three answers, the selection
afterwards, and that the entry is disabled on an instance proxy. `viewport_delete_test.cpp` sends Delete to a viewport's window: the
selection goes, as one step, and another key or an empty selection does nothing.

**Not tried by hand:** the Delete key in the running app (the tests send it to the window directly, so a click giving the viewport the
keyboard is unchecked), the dialog's look, and a delete during variant edit mode.

## 7. Not done

- **Cut** (copy and delete) needs copying prims in the Outliner first.
- **Speed on a large stage:** cleaning up pointers walks the whole stage once per delete. Not measured.
- **Prototypes:** pointers inside an instance's prototype aren't visited, as the walk doesn't enter instances. They can't be edited
  from here anyway.
