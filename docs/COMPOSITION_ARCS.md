# Composition Arcs — Plan

Status: **all three phases done** (§8), with the recommended choices in §6.

See and edit a prim's composition arcs (references, payloads, inherits, specializes, variant
sets): read-only in the Outliner's context menu, editable in a new Attribute Editor section,
with "Open as Stage" for referenced files, and a panel that lists every asset a stage uses.

## 1. What there is today

- The Outliner colours names of prims with arcs dark yellow (docs/OUTLINER_NAMES.md), its context
  menu switches variants and loads/unloads payloads (docs/OUTLINER_CONTEXT_MENU.md), and Prim
  Info counts the arcs.
- Nothing lists the arcs themselves, says where they point, or edits them.
- Reusable pieces: the Attribute Editor's asset field (`AssetEditor`: browse, relative/absolute
  toggle, the resolved path under it), `properties::iconButton()`, `prim_actions` (undoable
  edits), `StageOpener` (opens a file off the UI thread, as File > Open Stage does).

## 2. One model of a prim's arcs

A Qt-free helper, `composition_arcs.h` in `usdcc_usd`, used by everything below:

```cpp
struct ArcInfo {
    PcpArcType type;            // Reference, Payload, Inherit, Specialize, Variant
    std::string assetPath;      // as authored ("./chair.usd"); empty for an internal arc
    std::string resolvedPath;   // through ArResolver, relative to the introducing layer; empty if missing
    SdfPath primPath;           // the target prim; empty = the asset's default prim
    SdfLayerOffset layerOffset; // offset and scale, for references and payloads
    SdfLayerHandle introducedIn;// the layer whose list op adds it
    std::string listPosition;   // "prepended", "appended", "explicit"
    bool editable;              // introduced in the stage's edit target, on this prim
    bool nested;                // comes from inside another arc (a reference's own references)
    bool loaded;                // for payloads
};
std::vector<ArcInfo> compositionArcs(const UsdPrim& prim);
```

From `UsdPrimCompositionQuery(prim).GetCompositionArcs()`: each arc's type, target layer and
prim, introducing layer and its list editor (`GetIntroducingListEditor()`), which gives the
authored `SdfReference`/`SdfPayload` (asset path, prim path, layer offset). The root arc and
implicit arcs (an inherit seen again inside a reference) are left out. A **payload that isn't
loaded** isn't in the prim index, so payloads also come from the prim's specs
(`GetPrimStack()`, each spec's payload list).

## 3. The Outliner: a Composition submenu (read-only)

In the prim context menu, after Variants, for a single prim with any arcs:

```
 Composition              ▸  Reference   chair.usd  </Chair>          ▸  Type        Reference
                             Reference   (this file)  </_proto>          Asset       ./props/chair.usd
                             Payload     heavy.usd  (unloaded)            Resolved    C:/show/props/chair.usd
                             Inherit     </_class_Prop>                   Prim        /Chair
                             ─────────                                    Offset      +10, scale 1
                             via chair.usd:                               Defined in  shot.usda (prepended)
                             Reference   leg.usd  </Leg>                  ─────────
                                                                          Open as Stage
                                                                          Copy Asset Path
```

- One entry per arc: its type, the file name (or "(this file)"), the target prim, and
  "(unloaded)" or "(missing)" when that applies. Nested arcs come after a "via <asset>:" line.
- Each entry's submenu shows the details as disabled lines, then two actions (§6, question
  3): Open as Stage and Copy Asset Path (Copy Prim Path for an inherit or a specialize).
- A missing asset says "(missing)" (a menu can't colour one entry), with the reason in the
  entry's tooltip.

## 4. The Attribute Editor: a Composition section (editable)

Below the Prim header, above the property categories. Shown for every prim that can be edited
(not an instance proxy); collapsed by default when the prim has no arcs.

```
Composition                                                         [+]  ← References, Payloads,
  Reference   [./props/chair.usd          ][↗][📂][⇄]  [×]                 Inherits, Specializes
              C:/show/props/chair.usd
              Prim  [/Chair        ]   Offset  [10.0]  Scale  [1.0]
  Payload     [./heavy.usd                ][↗][📂][⇄]  [Loaded ☑]  [×]
  Inherit     [/_class_Prop               ]                         [×]
  Variant     look  [red ▾]
  ─ from other layers ─
  Reference   ./base.usd  </Base>     in base.usda       (read-only look)
```

- **One block per arc** authored on this prim:
  - The asset field reuses `AssetEditor`: ↗ opens it as a stage, 📂 browses, ⇄ toggles
    relative/absolute, and the resolved path (red when missing) shows under it.
  - Then the target prim (empty means the default prim) and the layer offset and scale.
  - Inherits and specializes have only a prim path.
  - A payload has a Loaded checkbox, through `prim_actions::load()`/`unload()`.
  - Variant sets have their selection here as well as in the Outliner.
- **Add** (the section's +): a menu with Reference, Payload, Inherit and Specialize.
  - A reference or payload starts with a file dialog. Its default is a path relative to the edit
    target layer, through the same relative/absolute rule as the asset field.
  - An inherit or specialize starts with the selected prim's path if another prim is selected,
    else an empty field.
  - All are added with `AddReference()` and the like, at the front of the prepend list, as
    USD does.
- **Change:** an edited field replaces the item in its list op, keeping its position
  (`SdfReferencesProxy::ReplaceItemEdits()` on the edit target's prim spec).
- **Remove** (×): `RemoveReference()` and the like, which removes it from the edit target's
  lists. An arc from another layer can't be removed from here (§6, question 2).
- **Read-only:** arcs from other layers and nested arcs are listed under "from other layers",
  in the read-only look, with the layer they're in. Their ↗ and copy still work.
- **Undo:** every edit is one step (`UndoManager::apply()`; arcs are layer edits). The prim
  recomposes, so the panel rebuilds as on any resync and keeps the prim selected.
- **Edit target:** the app always edits the root layer today, so "editable" means authored in
  the root layer. When an edit-target picker exists, this follows it with no other change.

Later changes to the section:

- An editable reference or payload is in a small frame of its own; every other arc is a plain row.
- Each arc has one ⋮ button at its row's right edge instead of × and ↗: Remove (an arc authored
  here), Open as Stage and Reveal in Explorer (a file). Arcs from other layers have it without
  Remove, and show their resolved path under the row like the asset field.
- An empty Prim field says "defaultPrim". Its ⋮ menu has Use defaultPrim (when a prim is set)
  and Select Prim from Stage..., which opens the file (no payloads loaded) in `PrimPickerDialog`:
  its prims as a tree with the Outliner's icons and types, the default prim in bold, a name
  filter, Use Prim and Cancel.

## 5. Open as Stage

Opens the arc's resolved file with `StageOpener` (off the UI thread, with progress, as File > Open
Stage), so it shows in a viewport and becomes the current stage (docs/CURRENT_STAGE.md). If that
file is already open as a stage, its viewport is brought to the front instead (§6, question 4).
An internal arc ("this file") and a missing asset have it disabled.

## 6. Decisions to make

1. **Which arcs?** Recommended: **all of the prim's arcs** in the Outliner submenu, with nested
   ones grouped under "via <asset>"; in the Attribute Editor, the ones authored on this prim,
   editable when they're in the edit target and read-only otherwise. Implicit arcs left out
   everywhere.
2. **Removing an arc another layer adds?** Recommended: **not in this plan**. It needs a
   "delete" list op in the edit target (`GetDeletedItems()`), which hides the arc without
   touching the other layer. That's powerful but easy to misread, so a later "Block" action
   with a clear label.
3. **Actions in the Outliner's "read-only" submenu?** Taken: **two**, Open as Stage and Copy
   Asset Path. They change nothing; the details stay read-only. (Show in Attribute Editor was
   dropped.)
4. **Open as Stage on a file already open?** Recommended: **bring its viewport to the front**.
   File > Open Stage opens a second copy, and that would be surprising here. The Shift key
   could force a new copy.
5. **Reordering arcs** (strength order) by dragging, like the transform stack? Recommended:
   **later**. Order matters but is rarely changed by hand.

## 7. A tool for every asset the stage uses: yes (done)

A **Dependencies** view panel (a ViewPanel type in the registry, like the Outliner, on the
Stages menu). It lists every external file the stage uses, grouped by file:

| File | Type | Used by | State |
|---|---|---|---|
| `./props/chair.usd` | reference | 12 prims | |
| `./heavy.usd` | payload | 3 prims | 1 unloaded |
| `./textures/wood.png` | asset attribute | 4 prims | |
| `./missing.usd` | reference | 1 prim | missing |

- **Source:** `UsdUtilsExtractExternalReferences()` on each layer in the stage's layer stack
  and on the layers they bring in, which also finds unloaded payloads and sublayers. The prims
  using each file come from walking the stage with `compositionArcs()`. Asset-valued
  attributes (textures, ...) are a column filter, off by default.
- **Actions:** select the prims using a file, open it as a stage, reveal it in Explorer, copy
  its path, and load or unload a payload everywhere it's used.
- **Missing files** in red at the top, so a broken stage shows at a glance.
- Refreshes on the stage's change notices (resyncs), coalesced like the Outliner.

As built (`dependencies.h` in usdcc_usd, `DependenciesViewPanel` in usdcc_usd_ui, the
"dependencies" panel type):

- `stageDependencies()` walks the stage with `compositionArcs()` rather than
  `UsdUtilsExtractExternalReferences()`: it gives the prims too, and reads unloaded payloads and
  missing files from the specs. Sublayers come from every used layer's sublayer list. The files
  inside an unloaded payload aren't listed, as that would mean opening it.
- Files are told apart by where they resolve (or, when missing, where they were looked for), so
  two spellings of one file are one row. An instance's contents count once per prototype, under
  its first instance's paths, so a heavily instanced stage isn't walked instance by instance.
- Columns: File, Type, Used by, State (missing, n unloaded), Path. Each file's prims are its
  children (a sublayer's are the layers that list it); double-click selects them.
- Right-click: Select Prims Using It, Open as Stage (not for an asset attribute's file), Reveal
  in Explorer, Copy Path, Copy Asset Path, and for a payload Load Everywhere / Unload Everywhere
  (one undo step each).
- Options: Show Asset Attributes (saved; a UDIM set counts as found), Refresh. Refreshes 100 ms
  after the last change notice, and on value changes only while asset attributes are shown.

## 8. Phases

| Phase | Work | Tests |
|---|---|---|
| **1. See them** *(done)* | `compositionArcs()` (`composition_arcs.h`, usdcc_usd: from the prim stack's list ops, so unloaded payloads and missing files are there; a layer that's open already, such as an anonymous one, counts as found); the Composition submenu (`prim_context_menu.cpp`); Open as Stage through `setOpenStageHandler()`, which `main.cpp` sets (`StageOpener`, or the open stage's viewport brought forward); Copy Asset Path | `compositionArcs()` on a stage with each arc type, nested references, an unloaded payload, a missing file; the submenu's entries |
| **2. Edit them** *(done)* | `AttributesViewPanel::buildCompositionSection()`, with `prim_actions::addArc()`/`changeArc()`/`removeArc()` (the entry found in the edit target's list op, replaced in place so it keeps its position and custom data). Each reference or payload is a file row (asset field anchored to the edit target, open ↗ and remove ×), then Prim, Time Offset and Time Scale rows, and Loaded for a payload; inherits and specializes are a path row; variants a picker. Read-only arcs (other layers, nested) come after "From other layers". The section's + adds a reference or payload (file dialog, relative path) or an inherit or specialize (path dialog). The arcs are part of the panel's layout signature, so any arc change rebuilds the section | Each edit and its undo; relative paths; an arc from a sublayer read-only |
| **3. Dependencies panel** *(done)* | §7: `stageDependencies()` and `DependenciesViewPanel` | Grouping, prims per file, missing and unloaded files, the actions |
