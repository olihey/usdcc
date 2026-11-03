# Layer Editor — Plan

Status: **all three phases done** (§8), with the recommended choices in §7.

A view panel that shows a stage's layer stack and manages it: add and remove in-memory
(anonymous) and file layers, reorder and mute them, pick the layer edits go to (the edit
target), and save them.

## 1. What there is today

- **Edit target:** every edit goes to the stage's edit target, which the app never changes, so
  it's always the root layer. `prim_actions`, the Attribute Editor and the transform tools all
  author through it, and the Attribute Editor's Composition section calls an arc "editable"
  when it's in the edit target (docs/COMPOSITION_ARCS.md §4).
- **Undo:** each undo step records the edits to one layer, the edit target when the step
  began, and keeps that layer (`LayerEditCommand`, undo_manager.cpp). So changing the edit
  target later doesn't break undoing older steps. Edits to any *other* layer, such as a parent
  layer's sublayer list, aren't recorded.
- **Saving:** there is none. Edits live in memory until the app closes, and closing a stage
  never asks about unsaved changes.
- **Related:** the Dependencies panel (docs/COMPOSITION_ARCS.md §7) lists sublayers as files,
  but only reads them.

## 2. The panel

A "Layers" view panel type in the registry, on the Stages menu like the Outliner, showing its
panel's stage:

```
 ☰  Stage: shot.usda ▾                    Editing: anim.usda    [+ ▾] [💾]
 ───────────────────────────────────────────────────────────────────────────
                                                  Target  Mute
 ▸ Session Layer                     (in memory)    ○       
 ▾ shot.usda                        *               ○       
     ✎ anim.usda                    *               ●       ☐
       lighting.usda                                ○       ☐
     ▸ set.usda                                     ○       ☑     (muted, dimmed)
       Untitled 1                   (in memory)     ○       ☐
       missing.usda                 (not found)                   (red)
```

- **A tree, strongest first:** the session layer, then the root layer, with each layer's
  sublayers under it in their strength order (the order of `subLayerPaths`). This is how USD
  composes the layer stack, so the top row wins.
- **Each row shows:**
  - The layer's display name.
  - `*` when it has unsaved changes (`IsDirty()`).
  - "(in memory)" for an anonymous layer.
  - Red with "(not found)" for a sublayer that can't be opened.
  - Dimmed when it's muted.
  - A tooltip with the identifier, the real path, the format and its time offset/scale.
- **Target column:** one radio-style mark, on the current edit target.
- **Mute column:** a checkbox; the root layer and the session layer can't be muted.
- **Toolbar:** "Editing: <layer>" (the edit target, always visible), an Add menu and Save.
- Follows the stage's change notices (a sublayer added by a script shows up) and dirty
  changes, coalesced like the Dependencies panel.

## 3. The edit target

- **Set it:** double-click a layer, click its Target mark, or right-click > Set as Edit Target.
  Any layer in the stage's layer stack can be the target, the session layer included. That's
  the usual place for non-destructive tweaks, as in usdview.
- **Can't be the target:** a muted layer and a missing one.
- **Muting the target:** muting the edit target first moves the target to the root layer.
- **One per stage:** the target belongs to the stage (`UsdStage::SetEditTarget()`), not to the
  panel. Every panel showing the stage follows it.
- **New notice:** `Stage` gets an `editTargetChanged` notice. The Attribute Editor rebuilds on it,
  since which arcs and values are editable changes. The Layer Editor moves its mark, and the
  status bar shows "Editing: anim.usda" for the current stage.
- **Not an undo step** (§7, question 1): it's a mode, like the selected tool, and undoing an
  edit shouldn't silently switch layers. Each undo step already undoes on the layer it was made
  in (§1).
- **Scripts:** `usdcc.usd` gets an edit-target getter and setter, so scripts can set it too.

## 4. Adding, removing and reordering layers

**Add** (toolbar + and right-click), into the selected layer's sublayers, as the strongest one
(§7, question 3):

- **New In-Memory Layer:** `SdfLayer::CreateAnonymous()`, named "Untitled n". It can be saved
  to a file later (§5).
- **New Layer File...:** a save dialog; `SdfLayer::CreateNew()` writes an empty layer to that
  path.
- **Add Existing Layer...:** an open dialog; the file is added, and opened if it isn't already.
- **The authored path** is relative to the parent layer when both are files, by the asset
  field's rule; otherwise it's absolute, or the identifier for an in-memory layer.
- **Option:** "and make it the edit target", on by default for a new layer, since a new empty
  layer is usually made to edit in.

**Remove** (Delete key, right-click):

- Takes the layer out of its parent's sublayer list. It never deletes a file.
- The root layer and the session layer can't be removed.
- Removing the edit target moves the target to the parent layer.
- An in-memory layer with content asks first, since nothing else holds that content.

**Reorder:**

- Move Up and Move Down (Ctrl+Up/Down), and dragging, within one parent's sublayer list.
- Dragging to another parent is a later step (§7, question 5).

**Undo:**

- Every add, remove, reorder and mute is one undo step.
- **Sublayer list changes** are edits to the parent layer, which often isn't the edit target.
  So `UndoManager::apply()` gets an overload that takes the layer to record on.
- **Mute** is stage state, not a layer edit. It gets a small `QUndoCommand`, as Load/Unload has
  (`LoadRulesCommand`).
- **In-memory layers** are freed by USD once nothing references them. The undo command for
  adding or removing one keeps a reference, so undoing a removal brings back its content.

## 5. Saving

The app has no saving yet, and a layer editor that can create layers needs it:

- **Save** (Ctrl+S on the panel, toolbar 💾, right-click): `SdfLayer::Save()` on the selected
  layer, or on the edit target.
- **Save All:** every dirty file layer of the stage. Also File > Save Stage (Ctrl+Shift+S) in
  the main menu.
- **Save As...:**
  - For a file layer it writes a copy (`Export()`); the stage keeps using the original.
  - For an in-memory layer it writes the file and points the parent's sublayer path at it,
    making it a file layer.
- **Reload** *(done, ahead of this phase)*: right-click > Reload on a file layer loads it fresh
  from its file, discarding its unsaved changes (`layer_actions::reloadLayer()`). It warns first
  when the layer has unsaved changes. USD replaces a text layer's content spec by spec, through
  the undo recorder, so the reload is an undo step that brings the changes back. A layer that
  streams its data (usdc) is swapped whole, past the recorder: its reload can't be undone and
  clears the stage's undo history, which the warning says (it's shown whenever there's history
  to lose). In-memory layers have no file to reload from.
- **Unsaved changes on close:** closing a stage, or the app, with dirty file layers asks Save
  All, Discard or Cancel, like the Script Editor's tabs.
- **Not undo steps:** saving and reverting are file operations.

## 6. Layer properties

Under the tree, for the selected layer, editable as Attribute Editor rows (each one undo step):

- Its time offset and scale in its parent (the `subLayerOffsets` entry).
- Default prim, start and end time codes, time codes per second and frames per second.
- Comment and documentation.
- Read-only: identifier, real path, file format, and whether it has unsaved changes.

Right-click also has Copy Path, Reveal in Explorer and Open as Stage (both shared with the
Attribute Editor and the Dependencies panel), and Show in Dependencies.

## 7. Decisions to make

1. **Is changing the edit target an undo step?** Recommended: **no** (§3). Maya and usdview
   don't make it one either.
2. **Show the session layer?** Recommended: **yes**, as the first row, collapsed. It's where
   viewer-only overrides go, and hiding it hides what's overriding the stage.
3. **Where does Add put a new layer?** Recommended: **as the strongest sublayer of the selected
   layer** (the root layer if none is selected). The other choice, a sibling just above the
   selected layer, is quicker for "a layer on top of this one" but harder to predict.
4. **Layers brought in by references and payloads?** Recommended: **not here**. They're not in
   the stage's layer stack, can't be the edit target without an edit-target mapping, and the
   Dependencies panel already lists them. Editing inside a referenced layer is a later feature.
5. **Drag a layer to another parent?** Recommended: **later**. Move Up and Move Down within a
   parent covers the common case.
6. **Lock layers** (`SetPermissionToEdit(false)`), so a layer can't be the target by mistake?
   Recommended: **later**, as a Lock column, once there's a use for it.

## 8. Phases

Phase 1 as built:

- `layerTree()` (`layer_stack.h`, usdcc_usd) reads the stack from the root and session layers'
  `subLayerPaths`, so missing and muted sublayers are listed too. A muted layer has no children,
  since its sublayers aren't composed.
- `Stage::editTargetLayer()` / `setEditTargetLayer()` and `Stage::editTargetChanged`, from
  `UsdNotice::StageEditTargetChanged`, so a script's `Usd.Stage.SetEditTarget()` is followed too.
  In Python: `usdcc.usd.edit_target()` and `set_edit_target(layer)`.
- `layer_actions::setMuted()` is a `QUndoCommand` that keeps a reference to the layer, so a muted
  in-memory or edited layer keeps its edits. Muting the target, or a layer it's under, moves the
  target to the root layer, and undo moves it back.
- `LayerEditorViewPanel` ("Layers" on the Stages menu): columns Layer, Target (the pencil on the
  edit target; click to move it) and Mute. Double-click also sets the target. The right-click
  menu has Set as Edit Target, Mute/Unmute, Copy Identifier, Copy Path, Reveal in Explorer and
  Open as Stage.
- The target's name is bold and accent blue. The toolbar and the status bar say "Editing:
  <layer>", the status bar for the current stage. The Attribute Editor rebuilds when the target
  changes.

Phase 2 as built:

- `layer_actions::addInMemoryLayer()`, `addNewLayerFile()` (written at once), `addExistingLayer()`,
  `removeLayer()` and `moveLayer()`, each one undo step recorded on the parent layer through the
  new `UndoManager::apply(stage, layer, ...)`. That step keeps a reference to the layer it adds or
  removes, so undo and redo find it, with its unsaved edits.
- A layer already in the stack can't be added twice (that includes adding a layer to itself).
- The panel's + (and right-click > Add Sublayer) adds under the selected layer, else the root.
  "Edit the New Layer" in that menu (on by default, saved) makes a new in-memory or file layer
  the edit target; an added existing layer isn't.
- Remove (Delete), Move Up / Move Down (Ctrl+Up/Down) and dragging a sublayer above or below a
  sibling. A drop anywhere else is refused.
- Removing an in-memory layer with content doesn't ask first, as §4 planned: undo brings it back
  with its content, which a question would only slow down.

Phase 3 as built:

- `layer_actions::saveLayer()`, `saveAll()` and `saveLayerAs()`, with `unsavedLayers()`: the file
  layers with unsaved changes among every layer the stage uses, so a referenced file a script
  edited counts too. Saving sends `Stage::layersSaved`, which the Layers panels follow, since a
  save sends no change notice.
- **Layers panel:** the toolbar's 💾 and Ctrl+S save the selected layer, else the edit target.
  Right-click has Save and Save As...; the options menu has Save All. Save on an in-memory layer
  opens Save As.
- **Save As:**
  - On a file layer, or a layer with no parent here, it writes a copy, as §5 planned.
  - On an in-memory sublayer, the file takes its place in the parent's list (one undo step)
    and becomes the edit target if the in-memory layer was.
- **File > Save Stage** (Ctrl+Shift+S, since Ctrl+S is the Script Editor's and the Layers
  panel's own Save) saves every unsaved file layer of the current stage.
- **Unsaved changes on close:**
  - `confirmUnsavedLayers()` lists the unsaved file layers and asks Save All, Discard or Cancel.
  - It runs before File > Close Stage (and the Stages menu's Close Stage) and when the window
    closes, through the new `MainWindow::addCloseCheck()`.
  - A layer another stage that stays open still uses isn't asked about, since nothing is lost.
- **Layer properties:** under the tree, in a splitter, for the selected layer:
  - Read-only: Identifier, File and Format.
  - For a sublayer: Time Offset and Time Scale in its parent.
  - Default Prim, Start and End Time Code, Time Codes per Second, Frames per Second, Comment
    and Documentation.
  - Each edit is one undo step on that layer (`layer_actions::editLayer()`,
    `setSublayerOffset()`). A field not authored on the layer has the unauthored look and shows
    USD's fallback.
  - The rows are rebuilt only when another layer is selected; otherwise just refilled, so a
    field being typed in keeps its text.


| Phase | Work | Tests |
|---|---|---|
| **1. See and target** *(done)* | `LayerEditorViewPanel` (the tree, dirty and missing marks, the Target and Mute columns, the "Editing:" label); setting the edit target; `Stage::editTargetChanged`, followed by the Attribute Editor and a status-bar label; mute and unmute as undo steps; the `usdcc.usd` edit-target calls | The tree of a stage with nested and missing sublayers; setting the target sends the notice and edits land in that layer; mute/unmute and their undo; muting the target moves it |
| **2. Add, remove, reorder** *(done)* | `layer_actions` (Qt-free where it can be): add in-memory, new-file and existing layers; remove; move up/down and drag within a parent; relative paths; `UndoManager::apply()` on a given layer; references kept to in-memory layers | Each action and its undo; the authored path; an in-memory layer's content surviving remove and undo |
| **3. Save and properties** *(done)* | Save, Save As (file and in-memory), Save All; the unsaved-changes prompt on Close Stage and app exit; File > Save Stage; the layer properties rows | Save writes the file and clears the dirty mark; Save As on an in-memory layer swaps the sublayer path; the prompt's three answers |
