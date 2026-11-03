# USDA View — Plan

Status: **phase 1 done** (§9), with the recommended choices in §8, except the big-file limit (§9.2). A saved 82 MB layer shows in about 0.1 s (§9.2); one that must be exported takes as long as USD's export.

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

| Phase | Work | Tests |
|---|---|---|
| **1. Show it fast** *(done, §9.2)* | `UsdaViewPanel` (Show menu: the stack's layers, Flattened Layer Stack, Flattened Stage); a read-only text view sharing `CodeEditor`'s gutter and current line (moved to usdcc_ui); the viewport highlighter with the background `"""` scan; Find and Go to Line; refresh (auto under the limit, else the Out of date bar, keeping the position); Flattened Stage off the UI thread with Cancel; the big-file limit | Highlighting of each token kind, including a `"""` string across lines; only visible blocks highlighted; the three Show modes' text; refresh after an edit; a generated 100 MB layer within the budget (times written to this file) |
| **2. Links** | Link hit-testing and hover; click, Ctrl/middle-click, the link menu; Back/Forward with scroll positions; non-layer files opened by the system; prim-path clicks selecting; Follow Selection with the prim index | Resolving relative links per layer; a missing file's link; history; the prim index (nested prims, variants); a prim-path click selecting |
| **3. Editing** (later) | Editable layers with Apply and Revert, parse errors at their line, Apply as one undo step | Apply and undo; a parse error leaves the layer untouched |
