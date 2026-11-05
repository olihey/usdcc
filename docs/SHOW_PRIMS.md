# Show Prims

Status: **done** (§6, §7), with the recommended choices in §5 and a name filter (question 2).

Everywhere the app offers **Show USDA**, a **Show Prims** below it opens a dialog with the file's
prims as a tree, looking like the Outliner: the same icons, name colours and tooltips, but only to
look at. Like Show USDA (docs/USDA_VIEW.md §10), it opens the file in a private stage, not as a
stage of the app.

## 1. What can be reused

- **The Outliner's tree:** `UsdPrimTreeModel` and `OutlinerItemDelegate` give the icons, the
  reference badge, name colours and tooltips, and load an instance's proxies only when it's
  expanded. The model has no read-only mode yet: its rows are renamable and have the active
  checkbox.
- **Not `PrimPickerDialog`:** it also shows a tree with these icons, but builds its own simple
  model, without the name colours, tooltips or instance proxies. Sharing the Outliner's model
  makes this look like the Outliner and stay that way.
- **Show USDA's private stage:** `UsdaPreviewDialog` opens a stage on a worker thread that
  `StageManager` never hears of, and copes with being closed during the load. That code moves out
  so both dialogs share it.

## 2. The pieces

- **`UsdPrimTreeModel::setReadOnly(true)`:** rows are no longer editable or checkable, and
  `CheckStateRole` returns nothing. The delegate already draws no checkbox then, and `setData()`
  does nothing.
- **`loadPreviewStage()`** (`preview_stage.h`, from `UsdaPreviewDialog::startLoading()`): loads a
  private stage on a worker thread and reports back to its owner with the stage or the error. Closing the
  dialog meanwhile just drops the result, and the app waits for a running thread when it quits.
- **`PrimTreeDialog`** (new): the model and delegate in a `QTreeView`, over a status line.
- **`showPrimTree(path, parent)`:** next to `showUsdaPreview()`, using the same `canPreviewUsda()`.

## 3. The dialog

```
 Show Prims - chair.usda                                                  ✕
 ───────────────────────────────────────────────────────────────────────────
  ▾ ⬡ Chair
      ▸ ⬡ Seat
        ▣ Leg
  ▸ ◌ Looks
 ───────────────────────────────────────────────────────────────────────────
  3 root prims · 12 prims · 2 payloads not loaded
                                       [Load Payloads]            [Close]
```

- **Only to look at:** expand and collapse, scroll, and tooltips. No menus, no rename, no
  checkboxes, and it doesn't touch the app's selection, so the Outliner and viewports don't react.
- **Loading:** a tree needs the composed stage, so unlike Show USDA there's nothing to show first.
  The dialog opens at once with "Loading stage...", and the tree appears when it's ready. That took
  25 s for the 82 MB test file, whose references are all missing, so a real file may differ. If
  it fails, the dialog says why.
- **Opening state:** the root prims expanded, the rest collapsed.
- **Keeping up:** layers are shared with the app's stages, so an edit to one recomposes this stage.
  The tree rebuilds on resyncs and keeps what's expanded, as the Outliner does.
- **As Show USDA:** not modal, over the main window, closed with Esc or Close, freed on close, its
  size remembered, and one dialog per file.

## 4. Where Show Prims goes

Below Show USDA, enabled exactly where it is:

- The Outliner's Composition submenu
- The Attribute Editor's arc menu
- The Dependencies panel
- The Layers panel: also for in-memory layers. It shows that layer as a stage's root, so its own
  sublayers and references compose, but not the rest of the stack.
- The USDA view's asset-link menu, in the panel and in the Show USDA dialog
- File > Preview: **Show Prims...**, with a file dialog

## 5. Decisions to make

1. **Payloads?** Recommended: **not loaded, with a Load Payloads button**, as in Show USDA. A payload
   prim shows without its children until then, and the status line says how many aren't loaded.
   Loading everything up front shows the whole structure at once, but a stage built on payloads can
   take as long as opening the real scene. The button is the one exception to "no actions", since it
   changes only this dialog's view.
2. **A name filter?** Taken: **yes** (§7). It searches what's loaded, so an instance's contents match
   once it's expanded: searching them all up front would load every instance's proxies.
3. **Can rows be selected?** Recommended: **yes, but nothing reacts**. A highlight helps keep your
   place in a long tree. The alternative, no selection at all, is stricter but harder to read.

## 6. Phases

| Phase | Work | Tests |
|---|---|---|
| **1. The dialog** *(done)* | `UsdPrimTreeModel::setReadOnly()`; `loadPreviewStage()`, with `UsdaPreviewDialog` moved onto it; `PrimTreeDialog` and `showPrimTree()`; the status line and Load Payloads | Read-only rows can't be edited or checked; the tree shows after the load with the Outliner's icons; `StageManager` and the selection untouched; a broken file; closing during a load; Load Payloads adds children; an edit to a shared layer updates the tree; one dialog per file; Show USDA's tests still pass |
| **2. Everywhere** *(done)* | Show Prims below Show USDA in each place in §4 | Each menu has it below Show USDA, enabled the same |

## 7. As built

- **The filter:**
  - Above the tree, "Filter by name", applied 200 ms after typing pauses (a `QSortFilterProxyModel`
    over the Outliner's model). It ignores case and keeps each match's ancestors.
  - Everything left is expanded so every match shows. The status line says "12 matches", counting
    the matches only, not their ancestors.
  - Clearing it restores what was open before. Esc clears it first, then closes the dialog.
  - While the stage has instances, the status line adds "instances: expand to search inside".
- **Status line:** the prim count (instance contents not included) and how many payloads aren't
  loaded.
- **Shared with Show USDA** (`preview_stage.h`): the private stage's loading, the preview window's
  setup, its owner, and finding the dialog already showing a file.
- **Measured** on the generated 82 MB file (190,000 prims, all its references missing):

| Step | Time | Where |
|---|---|---|
| The stage, without payloads (that file already open in Show USDA) | about 18 s | worker thread: the app stays usable |
| Building the tree | 0.16 s | UI thread |
| A filter matching 101,111 prims | 0.7 s | UI thread |

- **Known limits:** as Show USDA (docs/USDA_VIEW.md §10.8): the worker thread shares layers with the
  app's stages, and a previewed file's missing references warn in the Log panel. A filter on a very
  big tree pauses the app briefly, as in the table.
