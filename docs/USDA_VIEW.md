# USDA View — Plan

Status: **phases 1 and 2 done** (§9), with the recommended choices in §8, except the big-file limit (§9.2). A saved 82 MB layer shows in about 0.1 s (§9.2); one that must be exported takes as long as USD's export.

A view panel that shows a stage as USDA text: any layer of it as authored, or the stage
flattened. It has syntax highlighting, and asset paths work as links that open the file in this
panel or a new one. Read-only for now, built so editing can come later (§7). USDA files can run
to hundreds of megabytes, so performance shapes the design (§5).

## 1. What there is today

- **The Script Editor's `CodeEditor`** (usdcc_scripting_ui): a `QPlainTextEdit` with a
  line-number gutter, the current line highlighted, and a `QSyntaxHighlighter` for Python. The
  gutter and current-line code are worth sharing; the highlighter approach isn't, for big files
  (§5).
- **The Layers panel** (docs/LAYER_EDITOR.md) lists the layer stack; `layerTree()` gives its
  layers.
- **Opening files:** `StageOpener` opens a stage off the UI thread behind a progress dialog.
  `openStageFile()` and `revealFile()` are shared helpers.

## 2. The panel

A "USDA" view panel type in the registry, on the Stages menu like the others, for its panel's
stage:

```
 ☰  Stage: shot.usda ▾   Show: [shot.usda (root)          ▾]   ◀ ▶   ⟳   🔍
 ───────────────────────────────────────────────────────────────────────────
   1  #usda 1.0
   2  (
   3      defaultPrim = "World"
   4      subLayers = [
   5          @./anim.usda@,                       ← a link
   6          @./set/set.usda@
   7      ]
   8  )
   9
  10  def Xform "World" (
  11      prepend references = @./props/chair.usda@</Chair>
 ...
 ───────────────────────────────────────────────────────────────────────────
  shot.usda · 1,204 lines · 48 KB                         Out of date — Refresh
```

- **Show** picks what's shown:
  - **A layer of the stack**, with the session layer, the root layer and every sublayer, as
    in the Layers panel. It starts on the root layer.
  - **Flattened Layer Stack:** the sublayers merged into one layer, with references and
    payloads kept as arcs (`UsdUtilsFlattenLayerStack()`).
  - **Flattened Stage:** everything composed into one layer (`UsdStage::Flatten()`).
  - **A file opened from a link** (§6), which needn't belong to the stage.
- **The text** is `SdfLayer::ExportToString()`, so a usdc or usdz layer is shown as USDA too.
- **Toolbar:** Back and Forward (link history, §6), Refresh, and Find. The bar under the text
  shows what's shown, its size, and whether it's out of date (§4).
- **Read-only:** select, copy, find (Ctrl+F, with a match count and next/previous), and go to
  line (Ctrl+G). A monospace font, line numbers, and the current line highlighted, as in the
  Script Editor.

## 3. Syntax highlighting

Colours from the Script Editor's palette, so both read alike:

| Token | Example | Colour |
|---|---|---|
| Specifiers, keywords | `def`, `over`, `class`, `prepend`, `uniform`, `custom`, `variantSet` | accent blue |
| Prim types, value types | `Xform`, `float3[]`, `token`, `rel` | light blue |
| Strings, tokens | `"World"`, `"""docs"""` | green |
| Asset paths | `@./chair.usda@` | green, underlined as a link (§6) |
| Prim and target paths | `</World/Chair>` | yellow |
| Numbers | `1.5`, `-2e3` | orange |
| Metadata keys | `defaultPrim`, `kind`, `references` | light grey |
| Comments | `# ...` | grey italic |

- **Context:** only the triple-quoted strings (`"""`) span lines, so a line's colouring needs
  just one bit of context: whether it starts inside such a string.

## 4. Keeping up with edits

- **The view shows a snapshot:** the text is made when it's shown or refreshed. Re-exporting a
  big layer on every edit (a gizmo drag sends dozens a second) would be far too slow.
- **Auto-refresh:** after the shown layer changes, the view refreshes itself 500 ms after the
  last change. This only applies when the panel is visible and the text is under a size limit
  (§8, question 3).
- **Manual refresh:** past the limit, or for Flattened Stage (which composes the whole stage),
  the bar says "Out of date" and Refresh (F5) brings it up to date.
- **Position kept:** a refresh keeps the scroll position by line, and by prim when the prim
  index (§6) has it.

## 5. Performance

What makes a big USDA slow in Qt, and what this does instead. The budget is a 100 MB / 2-million
line file: under a second to first paint, smooth scrolling, and no stall while typing in Find.

| Step | The slow way | This plan |
|---|---|---|
| Making the text | Flatten and export on the UI thread, which freezes it | Export a layer on the UI thread (USD's writer is fast, and the layer can't change under it). Flatten (seconds for a big stage) runs behind a busy indicator, with a Cancel that drops the result |
| Building the document | `QTextEdit`, which lays out the whole document | `QPlainTextEdit`, whose layout lays out only the blocks on screen. The text goes in with one `setPlainText()`, with no undo stack (`setUndoRedoEnabled(false)`) |
| Highlighting | `QSyntaxHighlighter`, which highlights every block when the document is set, and that alone takes seconds for millions of lines | Highlight only the blocks on screen as they come into view (`QTextLayout::setFormats()` on each, cached until the text changes). Which lines start inside a `"""` string is found by one fast byte scan in a worker thread; until it's done, lines are coloured as if outside one |
| Links | Find all `@...@` up front | Found per block as it's highlighted; hit-testing is a lookup in that block's ranges |
| Find | Search the whole document on every keystroke | Search from the cursor in a worker thread, after typing pauses (200 ms); the count fills in as it goes |
| Line numbers | A gutter repainting every line | Painting only the visible block range, as `CodeEditor` already does |
| Memory | Keeping the exported `std::string`, the `QString` and the document | Only the document. A 100 MB file is about 200 MB as UTF-16, plus about 50 bytes a line of block overhead |

- **Very big files:** past a hard limit (§8, question 2) the panel shows the first part with a
  bar saying so, and "Show All" to load the rest anyway.
- **Measured, not assumed:** a test generates a big layer (many prims, with time samples and
  arrays) and times export, document build and first paint, so a regression shows. Phase 1 ends
  with those numbers in this file.
- **Fallback:** if `QPlainTextEdit` can't meet the budget, the view moves to a custom
  scroll-area view that paints lines straight from a UTF-8 buffer and a line-offset index. It
  sits behind the same panel API, so nothing else changes, but it costs selection, find and
  later editing written by hand. That's why it isn't the first choice.

## 6. Links

- **What's a link:**
  - Every asset path in the text, `@./props/chair.usda@` (and `@@@...@@@`): in sublayers,
    references, payloads and asset-valued attributes.
  - It's resolved like the composition arcs (`compositionArcs()`): relative to the layer it's
    written in, through the asset resolver.
  - In a flattened view, relative to the layer it came from where USD kept that, else as
    written.
- **On hover:** underlined, with a pointing-hand cursor, and a tooltip giving the resolved path
  or "Not found" (red).
- **Opening:**
  - **Click:** opens the file in this panel, and Back returns to where you were (and to that
    scroll position).
  - **Ctrl+click or middle-click:** opens it in a new USDA panel next to this one.
  - **Right-click on a link:** Open Here, Open in New Panel, Open as Stage, Reveal in
    Explorer, Copy Path, Copy Resolved Path.
  - **A file that isn't a layer** (a texture, ...): click opens it with the system's app
    (`QDesktopServices`).
- **A linked file** is shown with `SdfLayer::FindOrOpen()`: the same layer the stage uses if
  it's open, else opened for reading. Its own links work the same way.
- **Prim paths** (`</World/Chair>`): a click selects that prim when it's in the panel's
  stage, so the Outliner and the Attribute Editor follow.
- **Follow selection** (an option): selecting a prim elsewhere scrolls to its `def`/`over` in
  the shown layer. This needs a prim index, a map from prim path to line, built by the same
  background scan that tracks `"""` strings: it follows the nesting of `def`, `over`, `class`
  and `variantSet` blocks by their braces.

## 7. Built for editing later

Read-only now, but nothing should block making it editable:

- **The source of truth stays the layer.** An edit would be "Apply": `SdfLayer::ImportFromString()`
  into the layer. USD applies that spec by spec, through the undo recorder, as it does for Reload
  (docs/LAYER_EDITOR.md §5), so Apply would be one undo step. A parse error would be shown at its
  line and leave the layer untouched.
- **`QPlainTextEdit`** is already an editor; read-only is one flag. The viewport highlighter
  re-highlights an edited block, and the background scan reruns from the edited line on.
- **Editing would be for layers only.** A flattened view stays read-only, since there's no layer
  to apply it to.
- **Links:** with editing, a click must place the cursor, so links would move to Ctrl+click
  (§8, question 4).

## 8. Decisions to make

1. **The text widget?** Recommended: **`QPlainTextEdit` with viewport highlighting** (§5),
   measured against the budget in phase 1, with the custom view as the fallback. It gives
   selection, find, copy and later editing for free.
2. **Very big files?** Recommended: **show the first 50 MB** (about 1 million lines) with "Show
   All", past which the panel warns that it may be slow. The alternative, always everything,
   risks a long freeze on a multi-gigabyte flatten.
3. **Auto-refresh limit?** Recommended: **auto-refresh layers up to 5 MB**, debounced 500 ms;
   bigger ones and Flattened Stage refresh by hand. Refreshing everything automatically is
   simple but stalls during drags on big layers.
4. **Opening a link: click or Ctrl+click?** Recommended: **click** while the view is
   read-only, since it's what a link does. It moves to Ctrl+click if editing comes (§7). The
   other choice, Ctrl+click from the start, needs no change later but is less discoverable.
5. **Panel name?** Recommended: **"USDA"** in the Stages menu.

## 9. Phases

Phase 1 as built:

- **Pieces:**
  - `UsdaViewPanel` ("USDA" on the Stages menu).
  - `usdcc::ui::TextView`: the gutter and current line, moved out of `CodeEditor`, which now
    builds on it.
  - `usda::tokenize()` and `usda::lineStates()`, which share one scanner so they agree about
    strings.
  - First `UsdaHighlighter` on a `QPlainTextEdit`; since §9.2, `usdcc::ui::LineView`.
- **Differences from the plan:**
  - The `"""` scan runs on the UI thread, not in a worker: it takes 42 ms for 82 MB, too little
    to be worth a thread.
  - The find count runs on the UI thread too, 20,000 lines per event-loop turn, so typing stays
    smooth.
  - Flattened Stage runs on the UI thread with a wait cursor and no Cancel. USD can't flatten a
    stage that may be edited meanwhile, and a flatten can't be stopped once started, so a worker
    thread and Cancel would buy nothing safe.

### 9.1 Measured, first build

The first build was on `QPlainTextEdit`, and these rows are its numbers. The 82 MB row was in
fact the first 50 MB, the old limit (1.28 M of its 2.09 M lines). §9.2 has the numbers since.

From `usda_view_benchmark` (`USDCC_BENCH=1 usdcc_ui_tests usda_view_benchmark`), RelWithDebInfo,
offscreen, a generated layer of meshes with arrays and references:

| Size | Export (USD) | `"""` scan | Document (Qt) | To first paint | Scrolling |
|---|---|---|---|---|---|
| 8.2 MB, 128 K lines | 0.35 s | — | 0.88 s | **1.2 s** | — |
| 82 MB, 1.28 M lines | 3.3 s | 0.04 s | 5.0 s | **8.3 s** | 50 jumps through the file in 0.33 s |

- **Highlighting** isn't the problem any more. Colouring on screen only works: scrolling stays
  fast, and fewer than 20,000 lines get coloured.
- **The document:** `QPlainTextEdit::setPlainText()` costs about 4 µs a line, which makes
  `QPlainTextEdit` the biggest cost, as §5 feared.
- **The export:** USD's `ExportToString()` is the second biggest. Copying the layer to export it
  in a worker thread costs more than the export itself (5.4 s for 82 MB), so that's out.
- **Next step (to decide):** two changes would bring an 82 MB file well under a second:
  1. **The custom view of §5's fallback.** Lines are painted straight from the UTF-8 text and a
     line-offset index, built by the same scan as the line states. That cuts the 5.0 s to tens
     of milliseconds. It needs its own selection, copy, find and cursor, and for editing later it
     would hand over to `QPlainTextEdit` for files small enough.
  2. **Read a saved `.usda` file as it is on disk** instead of exporting it, when the layer has
     no unsaved changes: a file read takes about 0.1 s against the export's 3.3 s. Shown as the
     file is, comments and formatting included. A layer with unsaved changes, a usdc layer and
     the flattened views still export.

### 9.2 Both done

- **`usdcc::ui::LineView`** (usdcc_ui) replaced `QPlainTextEdit` in the USDA view. It's a
  `QAbstractScrollArea` that keeps the text as one UTF-8 string with a `memchr`-built index of
  line starts:
  - Only the lines on screen are decoded and laid out, each its own `QTextLayout` with the USDA
    formats, cached up to 4,000 lines.
  - It does its own gutter, current line, caret, mouse and keyboard selection (Shift, double-click
    a word), Copy, Select All, `find()` (ASCII case-insensitive, wrapping) and `countMatches()`.
  - Positions are a line and a UTF-16 column, so non-ASCII text selects correctly.
  - `TextView` (`QPlainTextEdit`) stays for the Script Editor, and for editing here later.
- **A saved text layer is read from its file:** a layer with a real path, no unsaved changes,
  and a file that starts with `#usda` (so a text `.usd` counts too). The status bar says "as
  saved", and comments and formatting show as written. Everything else is exported as before.
- **The big-file limit** is now 1 GB rather than 50 MB (§8, question 2). Showing a big file costs
  only memory now (the text, plus 8 bytes a line for the index), not seconds.
- **The find count** is one pass over the UTF-8 text, no longer spread over event-loop turns.

| Size | Making the text | `"""` scan | View | To first paint |
|---|---|---|---|---|
| 82 MB, 2.09 M lines, **saved** (read from the file) | 0.02 s | 0.08 s | 0.01 s | **0.12 s** |
| 82 MB, 2.09 M lines, unsaved (exported) | 3.1 s | 0.08 s | 0.01 s | **3.2 s** |
| 8.2 MB, unsaved (exported) | 0.31 s | — | 0.03 s | **0.35 s** |

- **Other timings:** counting 101,111 matches in the 82 MB file takes 57 ms; 50 jumps through it
  take 0.17 s.
- **What's left is USD's export** of a layer with unsaved changes (or of a flattened view). It
  can't run on another thread while the stage may change, and copying the layer first costs more
  than the export itself.

### 9.3 Phase 2 as built

- **`LineView` links**, kept generic:
  - The panel's link finder says where a line's links are (asset paths and prim paths, from
    `usda::tokenize()`). It's asked when a line is laid out, so only for lines on screen, and
    the links are cached with the layout.
  - Over a link: a pointing hand and a tooltip. A click opens it; a drag from it still
    selects. Middle-click and right-click work on links too (`linkActivated`,
    `linkMenuRequested`).
- **Asset paths:**
  - Resolved against the layer shown, the linked file's layer, or (for the flattened views
    and in-memory layers) the root layer. A layer that's open already counts as found, as in
    `compositionArcs()`.
  - A click opens the file here as a new Show entry, "<file> (linked)". A sublayer of the stack
    opens as that layer instead, so Show names it.
  - Ctrl+click or middle-click opens it in a new USDA panel, from the registry, docked next to
    this one (`UsdaViewPanel::setNewPanelHandler()`, set in `main.cpp`).
  - A file USD has no format for opens in the system's app. A missing one says "Not found" in
    the status bar.
  - A linked file's layer is held while it's shown (`SdfLayer::FindOrOpen()`), and read from
    disk when it's saved text.
- **Back / Forward** (◀ ▶, Alt+Left/Right) return to each place with its scroll position. A stage
  switch clears the history.
- **Prim paths:**
  - A click selects the prim when it's in the panel's stage. A variant selection or a
    property in the path is dropped first.
  - Not in a linked file, whose paths aren't the stage's.
- **The link menu:**
  - Asset paths: Open Here, Open in New Panel, Open as Stage, Reveal in Explorer, Copy Path,
    Copy Resolved Path.
  - Prim paths: Select Prim and Copy Path.
- **Follow Selection** (options menu, off by default, saved):
  - Selecting a prim scrolls to its `def`/`over`/`class` and puts the cursor there.
  - The prim index (`usda::primLines()`) is built when it's first needed after a refresh:
    0.3 s for the 82 MB file's 190,000 prims.
  - Prims inside variants are found by their path without the variant selection.

| Phase | Work | Tests |
|---|---|---|
| **1. Show it fast** *(done, §9.2)* | `UsdaViewPanel` (Show menu: the stack's layers, Flattened Layer Stack, Flattened Stage); a read-only text view sharing `CodeEditor`'s gutter and current line (moved to usdcc_ui); the viewport highlighter with the background `"""` scan; Find and Go to Line; refresh (auto under the limit, else the Out of date bar, keeping the position); Flattened Stage off the UI thread with Cancel; the big-file limit | Highlighting of each token kind, including a `"""` string across lines; only visible blocks highlighted; the three Show modes' text; refresh after an edit; a generated 100 MB layer within the budget (times written to this file) |
| **2. Links** *(done, §9.3)* | Link hit-testing and hover; click, Ctrl/middle-click, the link menu; Back/Forward with scroll positions; non-layer files opened by the system; prim-path clicks selecting; Follow Selection with the prim index | Resolving relative links per layer; a missing file's link; history; the prim index (nested prims, variants); a prim-path click selecting |
| **3. Editing** (later) | Editable layers with Apply and Revert, parse errors at their line, Apply as one undo step | Apply and undo; a parse error leaves the layer untouched |

## 10. Show USDA: a preview dialog

Status: **done** (§10.7), with the recommended choices in §10.6.

Everywhere the app offers **Open as Stage**, a **Show USDA** below it previews the file as USDA
text in a dialog. It's for a quick look, not for keeping around: no dock panel, nothing added to
the app's stages, and everything gone when the dialog closes. The dialog loads the file into a
stage of its own, and has everything the USDA panel has, because both are built from one shared
widget.

### 10.1 The two pieces

- **`UsdaView`** (new, a `QWidget`): everything in `UsdaViewPanel` today except being a dock
  panel. That's the Show menu, Back/Forward, Refresh, the `LineView` with highlighting and links,
  Find, Go to Line, the Out of date and size bars, and the status line. It shows a `StageRefPtr`
  it's given and knows nothing about docking.
- **Two thin hosts:**
  - `UsdaViewPanel`: a `ViewPanel` around a `UsdaView`, on an app stage, as today.
  - `UsdaPreviewDialog`: a `QDialog` around a `UsdaView`, on its own stage, with **Open as
    Stage** and **Close** buttons.

What differs by host is small, so `UsdaView` asks its host through a few callbacks:

| | Panel | Dialog |
|---|---|---|
| Ctrl+click or middle-click a link | a new USDA panel | a new preview dialog |
| Click a prim path | selects the prim in the app | jumps to its `def` in the text (nothing else shows this stage) |
| Follow Selection | in the options menu | not offered (no selection to follow) |

Moving the code makes this mostly a refactor: the panel's existing tests must keep passing
unchanged, which is the check that nothing was lost.

### 10.2 The dialog's stage

- **Private:** `StageManager` never hears of it. The dialog makes a `Stage` directly around a
  `UsdStage` of its own, so it isn't in the Stages menu, panels' dropdowns, the current stage, the
  undo history or the close-with-unsaved-layers question.
- **`Stage` needs one change:** its constructor assumes a stage in `UsdUtilsStageCache`. Without
  one, `cacheId()` is simply invalid. `UndoManager` and the Python bindings read it, but a preview
  never reaches them: it makes no edits, and they only list the stages `StageManager` has.
- **Separate from an open stage of the same file:** `UsdStage::Open()` outside a cache context
  makes a new stage every time, with its own session layer and mute state. So `StageManager`'s
  "same file, same stage" doesn't apply, and nothing the preview does affects the app's stage.
- **Layers are shared:** USD has one `SdfLayer` per file. So a file with unsaved edits in the app
  previews with those edits, and if the app edits it while a preview is open, the preview's
  normal change notices mark it out of date or refresh it. The preview is read-only and
  never changes a layer.
- **On close:** the dialog deletes itself and lets go of the stage. USD frees any layer nothing
  else uses.

### 10.3 Keeping it instant

A stage is slower than reading a file: it parses the layer and everything it references. On the
82 MB test file (§10.8), the text shows in about 0.3 s and the stage takes about 25 s. A preview
shouldn't wait for that, so it comes in two steps:

1. **At once, the file's text.** The dialog opens showing the file itself.
   - A text file that no stage has open has no unsaved edits, so it's read from disk without
     parsing it.
   - An open layer with unsaved edits, or a usdc or usdz file, is exported as today, which
     for a binary file means opening that layer first.
2. **Behind it, the stage**, on a worker thread with `StageManager::loadUsdStage()`, as
   `StageOpener` does.
   - Meanwhile Show lists just the file and says "Loading stage...", and text, Find and links
     work.
   - When it's ready, Show fills in the layer stack and the flattened views.
   - If it fails (a broken file), the text stays if there is any, and the status line gives
     the error.
   - Closing the dialog first just drops the result when the thread finishes; USD can't stop an
     open midway. The thread owns what it needs, so the dialog can go at any time.

**Payloads:** the stage opens with none loaded, so a preview stays cheap (`loadUsdStage()` gets a
load-set argument). Flattened Stage then leaves payloads out, and the status line says so, with
**Load Payloads** in the options menu to load them and flatten again (§10.6, question 2).

### 10.4 The dialog

```
 Show USDA - chair.usda                                                   ✕
 ───────────────────────────────────────────────────────────────────────────
  ☰  Show: [chair.usda (root)        ▾]   ◀ ▶   ⟳
 ───────────────────────────────────────────────────────────────────────────
    1  #usda 1.0
    2  def Xform "Chair" (
    3      references = @./leg.usda@</Leg>      ← a link
   ...
 ───────────────────────────────────────────────────────────────────────────
  chair.usda · 1,204 lines · 48 KB · as saved
                                              [Open as Stage]  [Close]
```

- **Not modal** (§10.6, question 1): the app stays usable, so a preview can sit beside the
  viewport while you read it. It has its own window, stays over the main window, and closes with
  it.
- **Esc** closes the Find bar if it's open, else the dialog.
- **Size:** 800 x 700 the first time; the last size is remembered.
- **One per file:** Show USDA on a file already previewed brings that dialog forward.
- **Open as Stage** opens the file through the usual `openStageFile()` and closes the dialog.
- **Links:**
  - A click opens the file in this dialog, with Back and Forward, as a "(linked)" entry in
    Show, as in the panel. It doesn't compose that file.
  - Ctrl+click, a middle-click or the link menu's **Show USDA** opens a new dialog with that
    file's own stage.

### 10.5 Where Show USDA goes

One helper next to `openStageFile()`, `showUsdaPreview(path, parent)`, below each Open as Stage:

| Where | Today | Notes |
|---|---|---|
| Outliner > Composition > an arc | Open as Stage, Copy Asset Path | |
| Attribute Editor > an arc's ⋮ menu | Remove, Open as Stage, Reveal in Explorer | |
| Dependencies panel, right-click | Select Prims Using It, Open as Stage, Reveal, Copy | Not for an asset attribute's file, such as a texture |
| Layers panel, right-click | Set as Edit Target, ..., Open as Stage | Also for in-memory layers, which Open as Stage can't open |
| USDA view, an asset link's menu | Open Here, Open in New Panel, Open as Stage | Both hosts; the dialog leaves out Open in New Panel, since its Show USDA is that |
| File menu | Open Stage... | **Show USDA...**, with a file dialog |

Disabled where Open as Stage is for a missing file, and for files USD has no format for.

### 10.6 Decisions to make

1. **Modal or not?** Recommended: **not modal**. It lets you compare the text with the viewport
   and open several previews. A modal dialog would be simpler (nothing can edit a shared layer
   meanwhile) but blocks the whole app for what's meant to be a quick look.
2. **Payloads?** Recommended: **not loaded**, with Load Payloads in the options menu. Loading them
   all could take as long as opening the real scene, for a preview that usually only needs the
   file's own text.
3. **A prim path click in the dialog?** Recommended: **jump to its `def`** in the text, using the
   prim index. The alternative, doing nothing, wastes a link, and selecting in the app would mean
   the wrong stage.
4. **Show USDA on a file already previewed?** Recommended: **bring that dialog forward**. The
   alternative, always a new one, piles up windows.

### 10.7 Phases

| Phase | Work | Tests |
|---|---|---|
| **1. Share the view** *(done)* | Move everything into `UsdaView`, with the host callbacks; `UsdaViewPanel` becomes a thin host. No change in behaviour | The existing USDA panel tests pass as they are; `UsdaView` runs without a dock manager |
| **2. The preview dialog** *(done)* | `UsdaPreviewDialog` and `showUsdaPreview()`: a private `Stage`, the text at once, the stage behind it on a worker thread, Load Payloads, one dialog per file, Open as Stage, the remembered size | Previewing opens no app stage (`StageManager` and the current stage unchanged); text shows before the stage is ready, and Show fills in after; a broken file; closing during a load; a prim path jumps to its `def`; a shared layer's edit reaches the preview; an open stage of the same file is untouched |
| **3. Everywhere** *(done)* | Show USDA in the Outliner's Composition submenu, the Attribute Editor's arc menu, the Dependencies and Layers panels, the USDA link menu, and File > Show USDA... | Each menu has Show USDA below Open as Stage, enabled as it should be |

### 10.8 As built

- **Shared:** `UsdaView` (usdcc_usd_ui) holds what `UsdaViewPanel` used to. The panel is now about
  90 lines around it, plus Follow Selection. Its tests passed unchanged through the move, apart
  from the link menu gaining Show USDA.
- **A file with no layer:** a File source now reads a text file no layer has open without parsing
  it, and resolves its links with `ArResolver::CreateIdentifier()` against the file. So following
  a link in the USDA panel got faster too.
- **Measured** on the generated 82 MB file, not open anywhere: **the text in 0.26 s, the stage,
  without payloads, after 25.5 s** (its 190,000 references don't exist, so each is a failed
  lookup, and a real file may differ).
- **Known limits:**
  - A usdc or usdz file has to be opened and exported for its text, on the UI thread, before the
    dialog shows. Text files don't.
  - The stage loads on a worker thread, as `StageOpener` does, and shares layers with the app's
    stages. Editing a layer the load is reading at that moment is the same race opening a stage
    has today, more reachable here since the dialog isn't modal and the app stays usable.
  - Missing references in a previewed file send USD's warnings to the Log panel, as opening
    it would.
