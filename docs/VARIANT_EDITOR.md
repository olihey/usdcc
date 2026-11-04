# Variant Editor

Author variants without leaving the app's normal tools. The Attribute Editor manages a prim's variant sets, and
each set has an edit button that opens a dialog for its variants. To change what a variant does, the dialog enters
a **variant edit mode**: the stage's edit target becomes that variant, so the Attribute Editor, gizmos and Outliner
author into it, with no second attribute editor to build.

## 1. Where it lives

- `variant_actions.h`: sets and variants, each one undo step, and what a variant holds.
- `variant_editor_dialog.h`: the dialog, the banner and the set actions that ask for a name.
- `Stage`: the mode (`enterVariantEdit()`, `exitVariantEdit()`, `variantEdit()`, `variantEditChanged`).
- `editTargetReaches()` in `stage.h` and `prim_actions::isEditable()`, `canRename()`: what can be edited during it.

## 2. How USD behaves

Everything here was run against this build (USD 26.11), except where it says otherwise.

**Creating**
- `AddVariantSet` and `AddVariant` work. Names come back sorted, and a new set has no selection.
- A variant added from a stronger layer writes an `over` with its own `variantSet` block there, and the names are
  the union of both layers. So a set from a weaker layer can gain variants, but can't lose the ones it defines.

**Removing and renaming: USD has no API on the stage**
- `UsdVariantSet` and `UsdVariantSets` can only clear a selection, so this goes through `Sdf`.
- **Remove a variant:** `SdfVariantSetSpec::RemoveVariant`. The selection is left pointing at the removed name.
- **Remove a set takes three steps:** remove the spec (`SdfPrimSpec::RemoveVariantSet`), take the name out of the
  variant set list op, and remove the selection. Removing only the spec leaves the name listed; removing only the
  name leaves the spec in the file.
- **Rename a variant:** `AddVariant(new)`, `Sdf.CopySpec("/P{set=old}", "/P{set=new}")`, remove the old one, update
  the selection. Its opinions survive.
- **Rename a set:** `CopySpec("/P{old=}", "/P{new=}")` works, then the same three removal steps, keeping the
  name's place in the list.

**Editing inside a variant**
- `GetVariantEditTarget()` maps `/Chair/Seat` to `/Chair{look=red}Seat`. `attr.Set`, `CreateAttribute`, `DefinePrim`,
  `SetActive`, `RemoveProperty` and arc authoring all land in the variant. `GetLayer()` is still the plain layer.
- **It uses the current selection,** so a variant has to be selected to become the target.
- **A session-layer selection works:** with `a` authored in a weak layer and `b` in the session, the target maps to
  `{m=b}` in the weak layer. Removing the session opinion brings back `a`. The file never changes.
- **Nesting works:** with a variant as the edit target, a new set lands inside it, and its target maps to
  `/R{outer=o}Inner{inner=i}`.

**Three traps**
1. **Prims outside the variant's prim escape silently.** With `/Chair{look=red}` as the target, `/Table` maps to
   `/Table`: its `Set` returns true and writes into the plain layer. There's no error to catch.
2. **A selection set under the target goes into the variant.** `SetVariantSelection("blue")` writes `look = "blue"`
   inside `{look=red}`, and the composed selection stays `red`. So the combo would appear to do nothing.
3. **`UsdNamespaceEditor` refuses it:** "Edit targets that map paths across composition arcs are not currently
   supported". `prim_actions::rename` uses it, and it's the app's only use of that editor.

**Reading and limits**
- A variant's contents can be read from its spec whether it's selected or not: `layer.GetObjectAtPath("/Chair{look=red}")`
  and its `primSpec`, walking properties and children.
- A prim that exists only in a variant disappears when the selection moves away, and old `UsdPrim` handles go invalid.
- A variant can't reference its own descendants (USD reports a cycle).

## 3. The Attribute Editor

Each set's row in the Composition section:

```
Variant: look   [ red         v ]  [pencil]  [...]
```

- **The combo** works as before. When the selection names a variant that doesn't exist, a warning line under it says so.
- **The pencil** opens the dialog (§4) on that set.
- **The ⋮ menu:** Clear Selection, Rename Set, Remove Set. Rename and Remove are disabled, with the reason in the
  tooltip, when the set's definition isn't in the current layer.
- **The section's + menu** gains Variant Set, asking for the set's name and first variant, and selecting that variant.
- **The prim context menu's** Variants submenu gains Edit Variants for each set.
- **Instance proxies** stay read-only, as everywhere else.

## 4. The dialog

Non-modal, so the Attribute Editor, Outliner and viewport stay usable, which also rules out a modal dialog for the mode in
§5. A dialog rather than a ViewPanel because it's about one set of one prim, and it's opened to do a job and closed.
It uses the existing preview-window pattern, with one window per stage, prim and set.

```
Variants of look on /Chair
+-----------------------+------------------------------------+
| red  (selected)       | What it changes    Value           |
| blue                  |   note             "in red"     [x]|
| green  from base.usda |   v Seat           over         [x]|
|                       |       size         3            [x]|
|                       |   OnlyInRed        def Sphere   [x]|
|                       | 4 opinions                         |
| Add Rename Dup Remove |                                    |
+-----------------------+------------------------------------+
| [ Edit Variant ]                                   [ Close ]|
+------------------------------------------------------------+
```

**Left, the variants**
- **Add, Rename, Duplicate, Remove,** each one undo step in the current layer. Names are validated before USD sees them.
- **Which layer defines it:** a variant from another layer says so. It can be edited, which writes an override in the
  current layer, but Rename and Remove are disabled.
- **Rename and Remove** update the selection in the current layer. A selection in another layer can't be changed from
  here, so it would dangle: the warning line under the combo shows that.
- **Remove asks first** only when the variant holds opinions, saying how many. Undo also covers it.
- Clicking a variant only shows its contents. It doesn't change the selection or the scene.

**Right, what the variant changes**
- A tree of its opinions: prims, attributes with values, relationships, arcs. It's read from the spec, so it works for
  unselected variants.
- **× removes an opinion.** Removing the last one under a prim also cleans up its empty `over`.
- A click on a prim selects it in the stage when it exists under the current selection.
- Opinions of the variant in other layers are listed under "In base.usda", without an ×.

**Edit Variant** enters the mode for the variant on the left.

## 5. Variant edit mode

Attribute values are changed through the tools that already change them. This covers "change attributes on prims
within a variant", and also prims, transforms, arcs, schemas and relationships.

**Entering**
1. Remember any session-layer selection for this set.
2. Select the variant in the session layer, so that it composes and the file stays clean.
3. Set the stage's edit target to the variant, in the layer that was current.

**While in it**
- **It's obvious:** an amber banner in the dialog and the Attribute Editor, "Editing look=red on /Chair", with an Exit button.
  The status bar's editing label (`main.cpp:539`) and the Layer Editor's current row say the same.
- **Other variants:** clicking one on the left and Edit Variant switches straight to it. Edits so far stay in the old one.
- **Another set** can't be entered while in the mode: exit first. So there's one level of nesting for now (§6).
- **Prims outside `/Chair` and its children are read-only** (trap 1): the Attribute Editor, Outliner and prim actions
  all ask `isEditable()`, the gizmos draw nothing for them, and the Attribute Editor says why. Selecting them to look at
  is fine. Python can still author there, since USD can't stop it.
- **Rename is disabled,** with the reason (trap 3).
- **Selections never go into the variant by accident** (trap 2): `setVariant` and `clearVariant` author outside the
  variant, in the plain layer. The combo for the set being edited switches the variant being edited instead.
- **Undo** records by layer, so it works unchanged, and steps stay undoable after leaving. Entering and leaving
  aren't undo steps, the same as changing the edit target.
- **The session layer** shows as modified in the Layer Editor once the mode has been used, though its content is back
  as it was: `SdfLayer` has no public way to clear a dirty mark. Save skips it, as an anonymous layer.

**Leaving**
- The Exit button, the dialog closing, or anything else changing the edit target, such as picking a layer in the Layer Editor.
- Also when the variant stops existing, for example undoing its creation. A stage change checks for this.
- It sets the edit target back to the plain layer, and restores the session-layer selection remembered on entry.
- A selected prim that existed only in the variant disappears, and the selection has to cope with that.

**What knows about it**
- `Stage::setEditTargetLayer()` compares the whole target, so setting the same layer leaves the variant.
- `compositionArcs()`: an arc in the variant being edited is editable; the plain prim's aren't, meanwhile.
- `XformStack` removes an op's attribute only when the variant holds its one opinion, not just the same layer.
- A variant that stops existing (an undo of its creation) ends the mode: the dialog checks, as it's always open during it.

## 6. Decisions

- **A dialog, not a ViewPanel:** it's about one set of one prim, opened for a job and closed.
- **Edit through the app's tools,** not an attribute table in the dialog: everything already works there, and there's
  one way to edit.
- **The selection is in the session layer during the mode,** so that opening a variant to look at it never changes the file.
- **Prims outside the variant are read-only,** staying in the mode, rather than warning, or leaving when one is selected.

**Later:** entering a set nested in a variant (they can be created, and the dialog lists them), Python bindings for the
mode, and capturing changes made outside the mode into a variant.
