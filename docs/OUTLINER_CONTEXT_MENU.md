# Outliner Context Menu — Plan

Status: **done** (all three phases, §7), with the recommended choices in §6.

A right-click menu on the Outliner's rows to activate or deactivate prims, show or hide them,
load or unload their payloads, pick variants, and see basic information about a prim. Every
edit is one undo step.

## 1. What there was before

- **No menu on the tree.** Right-clicking a row does nothing. The panel's *tab* menu
  (`OutlinerViewPanel::populateContextMenu()`, ADS's tab menu) has Move Up and Move Down.
- **The active checkbox isn't undoable.** `UsdPrimTreeModel::setData()` calls
  `UsdPrim::SetActive()` directly, outside `UndoManager`, so Ctrl+Z doesn't bring a prim back.
  Rename (also in `setData()`) has the same gap. Move Up/Down goes through
  `UndoManager::apply()`, as the attribute editor does.
- **Loading isn't a layer edit.** `UsdStage::Load()`/`Unload()` change the stage's load rules,
  not any layer, so `UndoManager`'s layer recording can't see them.

## 2. The menu

Right-clicking a row selects it first if it isn't selected (as in Explorer); the menu then acts
on the whole selection, except Variants and Info, which need one prim. Entries that don't apply
are left out, not shown disabled; entries that apply but can't be used (an instance proxy) are
disabled, with a tooltip saying why.

```
 Chair  ·  Xform                      (title: name and type, not clickable)
 ───────────────────────────
 ✓ Active                             checkable
   Visibility        ▸  ● Visible
                        ○ Invisible
                        ─────────
                        Show Only This
                        Reset (Inherited)
 ───────────────────────────
   Load Payload                       when it has payloads at or below it
   Unload Payload
 ───────────────────────────
   Variants          ▸  look   ▸  ● red
                                  ○ blue
                                  ─────────
                                  Clear Selection
                        lod    ▸  ...
 ───────────────────────────
   Move Up / Move Down                (moved from the tab menu, §6 question 4)
 ───────────────────────────
   Copy Path
   Prim Info…
```

## 3. The actions

| Entry | What it does (OpenUSD) | Undo |
|---|---|---|
| **Active** | `SetActive(bool)` on each selected prim. Checked when all of them are active; a mix shows unchecked and activates them all | `UndoManager::apply()` |
| **Visible / Invisible** | `UsdGeomImageable::MakeVisible()` / `MakeInvisible()`. `MakeVisible()` also authors on ancestors and their other children when an ancestor is invisible, so the prim really shows (usdview's behaviour) | `apply()` |
| **Show Only This** | Makes the selection visible and its siblings, and its ancestors' siblings, invisible | `apply()` |
| **Reset (Inherited)** | Clears the authored `visibility` opinion in the edit target (back to `inherited`) | `apply()` |
| **Load / Unload Payload** | `UsdStage::Load(path, UsdLoadWithDescendants)` / `Unload(path)` for each selected prim | A `QUndoCommand` pushed onto the stage's undo stack (`UndoManager::stack()`) that calls `Load()`/`Unload()` and the reverse, so it undoes in order with the layer edits |
| **Variants ▸ *set* ▸ *variant*** | `GetVariantSet(set).SetVariantSelection(name)`, authored in the edit target; the current selection is checked | `apply()` |
| **Clear Selection** | `ClearVariantSelection()`: back to the selection from weaker layers or the default | `apply()` |
| **Move Up / Down** | as now (`moveSelectedPrim()`) | `apply()`, as now |
| **Copy Path** | the selected paths, one per line, to the clipboard | — |
| **Prim Info…** | §4 | — |

Shown only when they apply:
- **Visibility**: the prim is imageable (`UsdGeomImageable`).
- **Load/Unload**: the prim or a descendant has payloads (`HasAuthoredPayloads()`, or
  `UsdStage::FindLoadable(path)` isn't empty). Load is enabled when something under it is
  unloaded, Unload when it is loaded.
- **Variants**: the prim has variant sets (`GetVariantSets().GetNames()`).

Instance proxies are read-only (docs/OUTLINER_NAMES.md): Active, Visibility and Variants are
disabled on them ("Part of an instance: edit the prototype's source"); Copy Path and Info work.

Every edit is one undo step for the whole selection, named for what it did ("Deactivate 3
Prims", "Set look to red on Chair"). Variant and load changes resync the prim, so the tree
rebuilds as it already does on resyncs, keeping expansion and selection by path. Unloading
removes descendants: a selected prim under it is dropped from the selection.

## 4. Prim Info

A small read-only dialog, one per prim, that can stay open while you work (non-modal):

| Row | From |
|---|---|
| Path, Name | `GetPath()` |
| Type | `GetTypeName()`, and its applied API schemas (`GetAppliedSchemas()`) |
| Specifier | def / over / class, and whether it's abstract or defined |
| Kind | `UsdModelAPI::GetKind()` (component, assembly, ...) when authored |
| State | active, loaded, visible (computed), instance / instance proxy / in prototype; for an instance, its prototype's path |
| Composition | counts of references, payloads, inherits, specializes; variant sets with their selections |
| Defined in | the layers with specs for it, strongest first (`GetPrimStack()`), with their paths |
| Children | how many |

A Copy button copies it all as text. The same dialog can later open from the Attributes panel.

## 5. Also fixed

The active **checkbox** goes through the same undoable action as the menu: the model emits a
request (`activeChangeRequested(path, bool)`) and the panel applies it with
`UndoManager::apply()`; `setData()` no longer edits the stage itself. **Rename** gets the same
treatment, since it has the same gap.

## 6. Decisions (as taken: the recommended ones)

1. **Prim Info: a dialog, or lines in the menu?** Recommended: **the dialog** (§4). A submenu of
   disabled lines is quicker to reach but can't hold the prim stack, and its text can't be
   selected or copied.
2. **Load: with descendants, or only this prim?** Recommended: **with descendants**
   (`UsdLoadWithDescendants`, usdview's default), so a payload's nested payloads come with it.
3. **Visibility: `MakeVisible()` or just the attribute?** Recommended: **`MakeVisible()`**: it
   shows the prim even under an invisible ancestor, which is what "Visible" means to the user.
   It does author on ancestors' other children, which the undo step covers. The alternative
   only sets the prim's own attribute, which does nothing under an invisible parent.
4. **Move Up/Down: move to this menu?** Recommended: **yes**, out of the tab menu, where they
   act on a selection the tab menu doesn't show.
5. **Keyboard shortcuts** for the most used: Ctrl+H hide/show, Ctrl+Shift+A active? Recommended:
   **none for now**; they would need to be panel-wide like the Script Editor's, and the viewport
   may want the same keys.

## 7. Phases

| Phase | Work |
|---|---|
| **1. Menu and edits** *(done)* | `populatePrimMenu()` (prim_context_menu.h), shown by `OutlinerViewPanel::showPrimMenu()`; the edits in `prim_actions` (prim_actions.h), all through `UndoManager::apply()`; the model's `activeChangeRequested`/`renameRequested` make the checkbox and rename undoable (§5) |
| **2. Load / unload** *(done)* | `prim_actions::load()`/`unload()`, with a `QUndoCommand` that puts the stage's load rules back (`UsdStage::GetLoadRules()`/`SetLoadRules()`); afterwards the panel drops selected prims that are gone |
| **3. Prim Info** *(done)* | `showPrimInfo()`, from `prim_actions::info()` |

Tests (usdcc_ui_tests, `tests/outliner_context_menu_test.cpp`): each action on a small stage (with a payload, a variant set, an
invisible parent, an instance), its undo and redo, the entries shown or left out per prim, and
instance proxies disabled.
