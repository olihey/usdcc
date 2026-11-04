# Attribute Editor Revamp — Plan

*Status: all five phases done (see §12); §9 lists what remains out of scope. Tracked as milestone M13
in [PLAN.md](PLAN.md). Last updated: 2026-11-01.*

## 1. Goal

Replace today's `AttributesViewPanel` (a three-column `QTableWidget` with text-parsed
values) with a property editor in the style of the reference mockup:

- **One row per property:** label on the left, a type-specific editor on the right.
- **Collapsible categories:** Transform, Material, Level of Detail, Visibility, Physics, …
- **Typed editors:** colored X/Y/Z prefixes on vector fields, a swatch plus R/G/B for
  colors, a filled slider bar for ranged floats, an asset field with browse/link, and an
  array summary with a square Edit (pencil) button, always the row's rightmost control.

The work also adds:

1. An editor for **every USD value type**, plus array variants, with a read-only
   fallback for anything not covered.
2. **Meaningful categories**, derived from USD's own schema information where it exists.
3. A **transform stack** section for `UsdGeomXformable` prims: show, edit, add, remove
   and reorder xformOps.
4. **Per-op targeting** for the Move, Rotate and Scale tools. For example, with two scale
   ops, the Scale tool edits only the one the user picked.
5. A **Debug menu** with a "USD Data Types…" dialog showing every type's editor side by
   side.

### Where we start from

These facts constrain the plan:

- `AttributesViewPanel` lists `prim.GetAttributes()` in a table and parses text back via
  `TfType` dispatch. Arrays, matrices, quaternions and asset paths are read-only.
- `UndoManager::apply()` snapshots the *whole* edit-target layer to a string before and
  after each edit. That is fine for one discrete edit. It is far too slow for a value
  scrubbed 60 times a second, and it would flood the undo stack.
- **Gizmo drags are not undoable today.** The tools call `UsdAttribute::Set()` directly.
- The panel refreshes only when the selection or the undo stack index changes. **It does
  not update during a gizmo drag.** There is no `UsdNotice` listener anywhere.
- Each tool finds its target op with "first op of type X, create if none" (in
  `TransformGizmoTool::getOrCreateOp()` and the Rotate tool's `findRotateOp()`). The
  gizmo frame is always the prim's full local-to-world transform.

## 2. Architecture

New code lives under `src/cpp/usd/`. The split follows the project's "Qt only in UI
classes" rule:

| Qt-free (target `usdcc_usd`, testable headlessly, bindable to Python) | Qt (target `usdcc_usd_ui`) |
|---|---|
| `ValueTypeInfo`: classifies an `SdfValueTypeName` (scalar kind, component count, role, axis labels, array-ness) | Widget kit: `DragNumberField`, `SliderField`, `ColorSwatch`, `AssetPathField`, `TokenComboField`, `MatrixField`, `ArraySummaryField` |
| `PropertyCategorizer`: prim → ordered categories → property names | `PropertyEditor` base class and `PropertyEditorFactory` (type → editor) |
| `XformStack`: read, add, remove, reorder and rename xformOps on a `UsdGeomXformable` | `PropertyRow` (label, editor, status badges, context menu) and `CategorySection` (collapsible header) |
| `StageChangeListener`: wraps `TfNotice` / `UsdNotice::ObjectsChanged` in a `usdcc::core::Signal` | `TransformStackSection`, `ArrayEditorDialog`, `DataTypeGalleryDialog` |
| Undo edit sessions (see §3) | The rewritten `AttributesViewPanel` |

### 2.1 Editor contract

```cpp
class PropertyEditor : public QWidget {
public:
    virtual void setValue(const VtValue& value) = 0;  // never emits valueEdited
    virtual void setReadOnly(bool readOnly);
    virtual void setMixed(bool mixed);                // reserved for multi-selection (§9)
Q_SIGNALS:
    void editStarted();                               // press on a field / start typing
    void valueEdited(const VtValue& value);           // live, possibly many times
    void editFinished();                              // release / Enter / focus out
    void editCanceled();                              // Esc: restore the value from editStarted
};
```

An `AttributeBinding` connects one editor to one `UsdAttribute`:

- `editStarted` begins an undo edit session.
- `valueEdited` calls `attr.Set(value, UsdTimeCode::Default())` live, so Hydra updates
  the viewport while scrubbing.
- `editFinished` commits the session; `editCanceled` rolls it back.

External changes arrive through `StageChangeListener`. They call `setValue()` on every
editor except the one currently being edited, so keyboard focus and in-progress typing
survive a gizmo drag or an undo.

**The panel never rebuilds its widgets on a value change.** It rebuilds only when the
selected prim changes, or when that prim's *set* of properties changes (a resync notice,
an op added, a schema applied).

### 2.2 Editor registry

`PropertyEditorFactory` resolves an editor in this order:

1. **Custom editors** registered for `(schema, property name)`. These are for special
   cases such as `xformOpOrder` (owned by the transform stack) or, later, plugin-provided
   editors (M9).
2. Editors for an exact `SdfValueTypeName`, including its role (`color3f`, `normal3f`,
   …).
3. Editors for the underlying C++ `TfType` with any role (`GfVec3f`).
4. The read-only fallback: the `VtValue` printed as text, marked "unsupported type". The
   panel therefore never silently drops a property.

The registry can be extended from C++ now and from Python in M7/M9.

## 3. Prerequisites: undo sessions and live refresh

These land first because every later phase depends on them.

### 3.1 Undo edit sessions

Extend `UndoManager` with two sets of calls:

- **Explicit sessions:** `beginEdit(stage, description)` starts recording the
  edit-target layer's changes, and `endEdit()` pushes one `LayerEditCommand` if anything
  changed. `cancelEdit()` reverts what was recorded.
- **One-shot calls:** the existing `apply()` stays for discrete edits, built on the same
  begin/end pair.

Both the property editors and the **gizmo tools** use sessions: press = begin,
release = end. That makes gizmo drags undoable, which closes part of M6.

The tools are Qt-free, so the viewport opens and closes the session around the tool's
press and release. `ToolResult` gets a flag such as `startedEdit`, so that a press that
only *selects* does not leave an empty session behind.

**Recording (replaced whole-layer snapshots).** The first version exported the whole
layer to text before and after every edit. On a 31 MB crate layer that took about 2 s
per export (129 MB of text), so every edit froze the UI for seconds and each undo step
held two copies. Now an `SdfLayerStateDelegateBase` subclass (`EditRecorder` in
`undo_manager.cpp`) is installed on the edit-target layer the first time it is edited.
SdfLayer routes every primitive change through it before applying it, and while a
session is open it stores each change's inverse: the old field, dictionary-key or
time-sample value; a delete for a created spec; a full copy of a deleted spec's subtree;
the reverse move; the opposite child push or pop. Consecutive sets of one field keep only
the first old value, so a drag stays one entry. Undo replays the inverses newest first
in one `SdfChangeBlock`, and the recorder records *their* inverses as the redo list. It
also takes over the dirty flag of the `SdfSimpleLayerStateDelegate` it replaces. Cost is
proportional to the change: an edit, a 60-step drag, an undo or a redo on that scene now
take under a millisecond. Tests compare the whole layer text after undo and redo of
creates, deletes, renames, reorders, metadata and time samples.

### 3.2 Live refresh

`StageChangeListener` registers once per stage for `UsdNotice::ObjectsChanged` and
re-emits `ChangedInfoOnlyPaths()` and `GetResyncedPaths()` as a Qt-free signal. The
attribute panel filters for its prim:

- A changed property path → update that row's value.
- A resync of the prim → rebuild the panel.

This fixes the stale panel during gizmo drags and after edits from other panels or
scripts.

## 4. Per-type editors

The type list comes from `SdfSchema::GetInstance().GetAllTypes()` at runtime, not a
hard-coded list. A type that a future USD version adds shows up in the gallery as
"unsupported" instead of disappearing.

| USD types | Editor | Notes |
|---|---|---|
| `bool` | Checkbox | |
| `uchar`, `int`, `uint`, `int64`, `uint64` | `DragNumberField` (integer) | Clamped to the C++ type's range |
| `half`, `float`, `double`, `timecode` | `DragNumberField`, or `SliderField` when limits are known | Range from `UsdAttribute::GetSoftLimits()`/`GetHardLimits()` (USD 26.11) and, for shader inputs, Sdr metadata |
| `int2/3/4`, `half2/3/4`, `float2/3/4`, `double2/3/4` | N fields prefixed X Y Z W | Axis prefixes red/green/blue, as in the mockup |
| `point3*`, `vector3*`, `normal3*` | Same as `float3` | Role shown in the tooltip |
| `texCoord2*`, `texCoord3*` | N fields prefixed U V W | |
| `color3*`, `color4*` | `ColorSwatch` plus R G B (A) fields | Values are linear. The swatch shows an sRGB-encoded preview, and `QColorDialog` round-trips through that conversion |
| `quath/f/d` | 4 fields prefixed W X Y Z | Optional read-only Euler hint in the tooltip |
| `matrix2d/3d/4d`, `frame4d` | `MatrixField`: one-line summary that expands to an N×N grid | For a 4×4 transform, optionally show decomposed T/R/S read-only |
| `string` | Line edit | Multi-line popup for long text |
| `token` | `TokenComboField` when `allowedTokens` metadata exists, otherwise a line edit | Covers `visibility`, `purpose`, `orientation`, `subdivisionScheme`, … |
| `asset` | `AssetPathField`: path field, "…" browse, link button; a second line shows the resolved path | Resolved via `ArResolver`; red text when unresolved |
| `pathExpression` | Line edit with syntax validation | |
| `opaque`, `group` | Read-only label | These have no value to edit |
| any `T[]` | `ArraySummaryField`: `[N] v0, v1, v2…` plus **Edit** | Opens `ArrayEditorDialog` (below) |

**`ArrayEditorDialog`:** a virtualized `QTableView` with one row per element, edited with
that element type's editor. It supports add, insert, remove and resize, plus paste from
the clipboard (one element per line). It edits a copy and applies it as one undoable
edit on OK. It is virtualized because mesh `points` arrays can hold millions of
elements.

### 4.1 Row features shared by all types

- **Label:** the `displayName` metadata if authored. Otherwise a prettified base name:
  `primvars:displayColor` becomes "Display Color" and `xformOp:translate` becomes
  "Translate".
- **Tooltip:** full property name, value type, role, the schema's documentation string,
  and which layer holds the strongest opinion.
- **State badges:**
  - authored vs. schema fallback (fallback values in dimmed text)
  - **time-sampled**: no timeline exists yet, so editing is disabled and the badge
    explains why (see §10)
  - **connected** (a UsdShade input or attribute connection): shows the source path and
    is read-only, like the mockup's "Linked · …" sub-line
  - locked by a stronger layer: an edit would be overridden
- **Context menu:** Copy value, Paste value, Reset to default (`attr.Clear()` in the
  edit target), Copy property path, Show in USDA panel (once M8 exists).

### 4.2 The widget kit (matching the mockup)

- **`DragNumberField`:** a dark rounded field with an optional colored axis prefix.
  - A horizontal drag scrubs the value: Shift makes the step smaller, Ctrl larger.
  - A click without dragging enters text-edit mode. Esc cancels.
  - Number formatting follows the mockup (`0.00`, plus a unit suffix such as `°`).
- **`SliderField`:** a filled bar showing the value (e.g. "0.420"). Click-drag sets the
  value; double-click types it.
- **Other widgets:** `ColorSwatch`, the array summary, the token combo and the asset
  field reuse the existing theme tokens in `dark.qss`. New selectors are scoped by
  object name, the same way the existing `panelToolbar` and `titleBar` selectors are.
- **Section headers:** a chevron plus a bold title with a full-width separator, as in
  the mockup. Collapsed state is remembered per category name in `QSettings`.

## 5. Categories

`PropertyCategorizer` (Qt-free) assigns each property to a category. The first rule that
matches wins:

1. **Authored `displayGroup` metadata** (`UsdProperty::GetDisplayGroup()`, nested groups
   via `GetNestedDisplayGroups()`). This is USD's own mechanism, so an asset author's
   choice is always respected.
2. **Transform:** `xformOpOrder` and every `xformOp:*`. These are owned by the transform
   stack section (§6), not shown as plain rows.
3. **Applied API schemas:** for each schema in `prim.GetAppliedSchemas()`, look up
   `UsdSchemaRegistry::FindAppliedAPIPrimDefinition(name)->GetPropertyNames()`. Each API
   gets a section named after it ("Physics Rigid Body", "Material Binding", …). Several
   instances of a multiple-apply schema get one section per instance.
4. **Typed schema hierarchy:** walk the prim type's `TfType` ancestry from the base up,
   for example Imageable → Xformable → Boundable → Gprim → PointBased → Mesh. Look up each
   level's definition with `FindAbstractPrimDefinition` or `FindConcretePrimDefinition`.
   Each property goes to the *most basic* schema that declares it. Then map schemas to
   user-facing names:
   - Imageable → **Visibility** (visibility, purpose, proxyPrim)
   - Boundable → **Bounds** (extent)
   - Gprim → **Display** (displayColor, displayOpacity, doubleSided, orientation)
   - PointBased / Mesh / Sphere / Camera / Light / … → a section named after the schema
     (**Mesh**, **Camera**, **Light**, …)
5. **UsdShade:** `inputs:*` → **Inputs**, `outputs:*` → **Outputs**, on Shader, Material
   and NodeGraph prims.
6. **Primvars** that no schema declared → **Primvars**, each showing its interpolation.
7. Everything else (custom, unschematized) → **Custom**, sub-grouped by namespace
   (`userProperties:` → "User Properties").

The order puts Transform first, then the typed-schema sections from most derived to
most basic, then applied APIs, Primvars, Inputs/Outputs, and Custom last.

A **"Prim" header section** sits above all of these: name (the path in its tooltip), type, kind, specifier,
active, instanceable and applied schemas. Most of it is read-only; active, kind and
instanceable are editable.

**Panel toolbar additions:**
- A search field that filters rows by label or property name across categories.
- A **Show** menu: Authored only / All / include `hidden` properties.

Phase 1 starts with a spike that confirms rule 4's ancestry walk on real prims (Mesh,
Sphere, Camera, a light, a UsdShade shader). The registry calls exist in 26.11; what the
spike checks is that the ancestry and definitions line up as expected. If they don't,
rule 4 falls back to a curated name table for the core UsdGeom schemas.

The **Material** section in the mockup (the bound material's shader inputs shown inline
on the geometry prim) is a separate, later feature; see §9. Until then, a Material
section shows the material binding relationship with a "select material" button.

## 6. Transform stack section

The section appears for any `UsdGeomXformable` prim. It replaces the plain rows for
`xformOp:*` and `xformOpOrder`.

```
Transform                                   [Reset XForm Stack ☐]  [+]
  ≡  Translate              X 0.00   Y 1.24   Z 0.00            [×]
  ≡  Translate (pivot)      X 0.00   Y 0.50   Z 0.00            [×]
  ≡  Rotate XYZ             X 0°     Y 35°    Z 0°              [×]
  ≡  Scale                  X 1.00   Y 1.00   Z 1.00            [×]
  ≡  Scale (second)  ◆      X 2.00   Y 2.00   Z 2.00            [×]   ← tool target
  ≡  Translate (pivot) ⁻¹   (inverse of "pivot", no value)      [×]
```

**Rows** follow `GetOrderedXformOps()` order:
- **Label:** a friendly op name, the suffix in parentheses, and "⁻¹" for `!invert!` ops.
- **Editor by op type:**
  - translate, scale, rotateXYZ (and the other five Euler orders) → 3 fields
  - rotateX/Y/Z → 1 field
  - orient → quaternion
  - transform → matrix
- **Precision:** float, double or half, shown in the tooltip.
- **Inverse ops** have no editor. They share their attribute with the forward op.

**Actions** (all undoable; the logic lives in Qt-free `XformStack` so it is unit-tested):
- **Reorder:** drag a row by its "≡" handle, or use Move Up/Down in its context menu.
  This calls `SetXformOpOrder(newOrder, resetsXformStack)`.
- **Remove** ("×"): takes the op out of `xformOpOrder`. The attribute itself is also
  removed (`prim.RemoveProperty`) but only when *all* of these hold:
  - it is authored in the current edit target,
  - no remaining op references it (a pivot's inverse op shares the attribute),
  - no weaker layer defines it.

  Otherwise it is left in place, unused. This matches USD's rule that ops missing from
  `xformOpOrder` are ignored.
- **Add** ("+"): Translate, Rotate (XYZ, XZY, YXZ, YZX, ZXY, ZYX, X, Y, Z), Scale, Orient,
  Transform, and a **Pivot** preset that inserts a `translate:pivot` /
  `!invert!translate:pivot` pair around the selected op.
  - New ops go *after the selected row* (or at the end) via `AddXformOp()` followed by a
    reorder.
  - A unique suffix is generated when the plain name is taken (`scale` → `scale:scale2`),
    and the user can rename it from the context menu.
  - Precision defaults to USD's own default (double).
- **Reset XForm Stack** checkbox toggles `resetXformStack`.
- **Context-menu extras:**
  - Reset op to identity.
  - "Bake stack to single matrix" (one `transform` op holding the composed local
    matrix).
  - "Convert to common TRS" (`UsdGeomXformCommonAPI`), enabled only when the stack is
    compatible.

**Selecting a row** sets the **tool target op** (§7), shown with the ◆ marker.

## 7. Tools acting on a specific xformOp

### 7.1 Target state

A per-stage "target op" lives next to the selection in `Stage`/`StageManager`:
`setTargetXformOp(primPath, opName)`, plus a `targetXformOpChanged` signal. An empty
name means **Auto**, which is today's behaviour: first op of the matching type, created
if missing.

The target is cleared when the selection changes to a different prim, or when the op
disappears from the stack.

`ToolContext` gets a `targetOpName` field (`TfToken`), filled by the viewport, so the
Qt-free tools receive it like the selection.

There are **two ways to set it**, kept in sync:
1. Select a row in the Attribute editor's transform stack.
2. A **Target** combo in the viewport's tool options bar. It lists Auto plus every op on
   the selected prim that the active tool can drive.

### 7.2 Which tool can drive which op

| Tool | Drivable op types |
|---|---|
| Move | `translate` (any suffix, including pivots) |
| Rotate | `rotateXYZ`, the five other Euler orders, `rotateX/Y/Z` (only that axis's ring is drawn and pickable), `orient` (composed as a quaternion and written back exactly) |
| Scale | `scale` |
| Move, Rotate and Scale | a `transform` matrix op, each tool editing only its part of it (see §12, "Matrix ops") |

**When the target and the active tool don't match** (e.g. a scale op is targeted while
Move is active), selecting a row **switches to the tool that can drive it**:
translate → Move, rotate/orient → Rotate, scale → Scale. If the user then picks an
incompatible tool by hand, the gizmo is drawn dimmed and does nothing. The options bar
explains: "Target is a Scale op; choose Auto or a Translate op." Silently editing a
different op would defeat the purpose of targeting.

### 7.3 The maths: the op's own frame

Today the gizmo frame is the prim's whole local-to-world matrix. That is correct only
when the edited op is the first in a plain TRS stack.

For a target op at index k, split the composed transform. USD uses row vectors, and
ops listed earlier in `xformOpOrder` are applied *last*:

```
world = Inner · Op_k · Outer · ParentToWorld
Inner = product of the ops after k   (applied to the point before Op_k)
Outer = product of the ops before k  (applied after Op_k)
```

Each op's matrix comes from `UsdGeomXformOp::GetOpTransform()`.

- **Gizmo frame for op k** = `Outer · ParentToWorld`, the space Op_k's result lives in.
  Its translation is the gizmo origin and its rows are the axes, stripped of scale as
  `GizmoFrame` already does. A pivot falls out of this: when the preceding
  `translate:pivot` is part of `Outer`, the gizmo sits at the pivot.
- **Move** on a translate op: convert the world-space delta into that frame (inverse of
  `Outer · ParentToWorld`, direction only) and add it to the op's value.
- **Rotate** on a rotate op: the composition the Rotate fix introduced
  (R' = Rot_axis(θ) · R_start), with axes taken from the op frame instead of the prim
  frame. The decomposition generalizes from XYZ to all six Euler orders, with the same
  nearest-continuous and gimbal handling. Single-axis ops add θ directly. `orient` ops
  compose quaternions.
- **Scale** on a scale op: per-axis or uniform ratio applied to that op only, with axes
  from the op frame.
- **Other ops** are only read, never written. The two-scale-ops case is exactly this:
  target `scale:second`, and `scale` keeps its value.

`TransformGizmoTool` gets an `opFrame(xformable, opIndex)` helper and a
`resolveTargetOp(ctx, allowedTypes)` that replaces `getOrCreateOp` and `findRotateOp`.
Auto mode keeps today's behaviour exactly.

### 7.4 Tests

The headless tool harness becomes a real test target (§8). It covers:
- two scale ops, each edited independently
- a pivot sandwich (rotate about a pivot)
- a target op in the middle of a 5-op stack, under a rotated and scaled parent
- all six Euler orders
- `rotateY` single-axis
- `orient`
- target cleared when the op is removed
- a mismatched tool/op leaves everything unchanged

## 8. Debug menu and the "USD Data Types" gallery

- A new top-level **Debug** menu goes after View. It is always present for now; a
  setting can hide it once M10's Settings dialog exists.
- **Debug > USD Data Types…** opens a non-modal `DataTypeGalleryDialog`:
  - **Contents:**
    - Every type from `SdfSchema::GetAllTypes()`, grouped as Scalars, Vectors,
      Points/Normals/Vectors, Texture Coordinates, Colors, Quaternions, Matrices,
      Strings & Paths, Other, and Arrays (one tab per group).
    - Each row is a real `PropertyRow`. It shows the type name, the C++ type and the
      role, and is bound to a real attribute on a prim in a scratch in-memory stage, so
      the editors are fully functional.
    - Each attribute is preset with a sample value; arrays get a few elements.
    - Types without an editor show the fallback and are counted in an "N unsupported"
      footer.
  - **Toggles that preview every row state:** read-only, fallback (unauthored),
    time-sampled, connected, and limits (soft limits set on the scalar rows to exercise
    the slider).
  - **Edits are recorded** in an in-dialog log line ("float3 → (1, 2, 3)"), so the
    `valueEdited` / `editFinished` sequence can be checked by eye.
- The dialog is also the widget kit's visual test bench. It lands *before* the panel
  rewrite (phase 2), so every editor is reviewed in isolation first.

## 9. Out of scope for this plan

- **Inline material editing:** the mockup's Material section shows the bound material's
  surface-shader inputs, resolved via `UsdShadeMaterialBindingAPI::ComputeBoundMaterial`
  and edited on the geometry prim. It is planned as a follow-up once the base editor is
  done.
- **Multi-selection editing:** the editor shows the first selected prim, as today. The
  editor contract already has `setMixed()` for later.
- **Relationship editing:** since done, in docs/RELATIONSHIPS.md.
- **Animation:** editing time samples and keyframing. This needs a timeline first.
- **Python and plugin-registered custom editors:** the registry is designed for them,
  but the bindings come with M7/M9.

## 10. Decisions taken as defaults

Each of these can be changed before the phase that needs it starts:

| # | Question | Default used by this plan |
|---|---|---|
| 1 | Time-sampled attributes, with no timeline yet | Show the value at `Default` (or the first sample if there is no default), editing disabled, with an "animated" badge |
| 2 | Units like "5.0 m" in the mockup | No distance units at first. Degrees get `°`. Unit display based on `metersPerUnit` is a later option |
| 3 | The link button on the asset field | Toggles between a layer-relative path and an absolute path. The resolved path is always shown below |
| 4 | Debug menu visibility | Always shown until a setting exists (M10) |
| 5 | A row selected for a tool the user hasn't picked | Auto-switch to the matching tool (§7.2) |
| 6 | Removing an op | Remove from `xformOpOrder`, and delete the attribute when that is safe (§6) |

## 11. Phases

Each phase ends with something usable and verified.

| Phase | Content | Verification |
|---|---|---|
| **1. Foundations** *(done)* | Undo edit sessions (§3.1), including **undoable gizmo drags**. `StageChangeListener` and live panel refresh (§3.2). `ValueTypeInfo`. A real test target replacing the scratch harnesses. Categorizer spike (§5) | Unit tests for sessions (one undo step per drag, cancel restores). The current table panel updates live during a gizmo drag. Undo of a Move drag works |
| **2. Widget kit + gallery** *(done)* | `DragNumberField`, `SliderField`, `ColorSwatch`, `TokenComboField`, `AssetPathField`, `MatrixField`, `ArraySummaryField` + `ArrayEditorDialog`; `PropertyEditor`, the factory and `PropertyRow`; theme additions. **Debug menu + USD Data Types dialog** (§8) | Every type from `GetAllTypes()` shows an editor or a counted fallback. The goal is zero unsupported types except `opaque`/`group`. Edits round-trip on the scratch stage. Screenshots reviewed against the mockup |
| **3. New panel** *(done)* | Rewritten `AttributesViewPanel`: scroll area, "Prim" header, `PropertyCategorizer` sections, search, Show filter, badges, context menu, collapsed state persisted | Categorizer unit tests on Mesh/Sphere/Camera/Light/Shader/custom prims. Live edit of each category reflected in the viewport. Undo of panel edits works |
| **4. Transform stack** *(done)* | `XformStack` (Qt-free) and `TransformStackSection`: values, add, remove, reorder, pivot preset, resetXformStack, bake/convert actions | Unit tests for every stack operation, including the inverse-op attribute-sharing case and weaker-layer opinions. All actions undoable |
| **5. Tool targeting** *(done)* | Target-op state, options-bar Target combo, row selection ↔ target sync, `opFrame`/`resolveTargetOp`, Euler orders, single-axis rotate, orient | §7.4 test list. Live check with a pivot-sandwich asset and a two-scale-op asset |

Phases 1 and 2 are independent and can run in parallel. Phase 3 needs both. Phases 4 and
5 need 3, but 5's maths (`opFrame`, Euler orders) can start against the tool tests
right after phase 1.

## 12. What was built

Where the result differs from the plan above, this section is the current truth.

### Phase 1

- **Undo edit sessions:** `UndoManager::beginEdit()`, `endEdit()` and `cancelEdit()`.
  Sessions nest, and `apply()` is built on them. A failed `apply()` now rolls the layer
  back instead of leaving a half-done edit in place.
- **Gizmo drags are undoable:** `ToolResult::startsEdit` is set by
  `TransformGizmoTool::mousePress()` when a handle was grabbed.
  - `HydraViewportWindow` emits `toolEditStarted`/`toolEditFinished` around the drag,
    including when switching tools cuts it short.
  - `ViewportViewPanel` turns that pair into one undo step. It now takes the
    `UndoManager` as a constructor argument.
  - `ScaleTool` used to create its scale op on press; it now creates it on the first
    move, so a press never edits outside the session.
- **Change notices:** `Stage::changed` re-emits `UsdNotice::ObjectsChanged` as plain
  `StageChanges` data, with `affectsPrim()` and `resyncsPrim()` helpers. It is a member
  of `Stage` rather than a separate class.
  - The current table panel uses it for coalesced live refresh. That replaces its old
    undo-stack connection, which was never re-pointed when the panel switched stages.
  - The viewport uses it to redraw. **This fixed a real bug:** the viewport only redrew
    on its own events, so undo/redo and edits from other panels didn't show until the
    next mouse move.
- **`ValueTypeInfo`** (`usdcc_usd`, Qt-free): type classification, the canonical type list,
  numeric and array access, sample values and value formatting.
  - `SdfSchema::GetAllTypes()` also returns legacy capitalized aliases (`Vec3f`,
    `PointFloat`, `Color`, …), which `canonicalValueTypes()` drops.
  - USD 26.11 has **112 canonical types** (57 scalar, 55 array), including `duration`
    (`GfDuration`) and `timecode` (`GfTimeCode`).
- **Test target:** `tests/usdcc_tests`, registered with CTest (`ctest -C RelWithDebInfo`).
  - It uses a small in-repo harness (`test_harness.h`) rather than GoogleTest, to avoid
    a new vcpkg dependency for now.
  - It contains the two former scratch tool harnesses plus new undo, stage-change,
    value-type and categorizer tests.
- **Categorizer spike** (`tests/categorizer_spike_test.cpp`): §5 rules 3 and 4 work as
  planned, using only `UsdSchemaRegistry`.
  - Mesh: Imageable → visibility/purpose/proxyPrim, Xformable → xformOpOrder, Boundable
    → extent, Gprim → doubleSided/orientation/displayColor/displayOpacity, PointBased →
    points/normals/…, Mesh → its own attributes. Applied ShadowAPI and
    PhysicsRigidBodyAPI take their own properties.
  - **Built-in API schemas own many properties of typed prims.** LightAPI owns a light's
    `inputs:intensity`, `inputs:color` and the light-link collections; NodeDefAPI owns a
    Shader's `info:id`. Those therefore land in API sections under rule 3. Phase 3 must
    name them well ("Light" rather than "LightAPI").
  - Shader `inputs:*` declared by the shader itself belong to no schema, so rule 5
    (Inputs/Outputs) is needed as planned.

### Phase 2

All under `src/cpp/usd/{include/usdcc/usd,src}/properties/`, in `usdcc_usd_ui`.

- **Widgets:**
  - `NumberField` is the planned `DragNumberField` and `SliderField` in one widget,
    since they share typed entry and the edit signals. It scrubs with Shift/Ctrl for
    fine/coarse steps, types on click, and cancels with Esc. In slider mode a press sets
    the value and a double-click types.
  - A **soft** range drives the slider but never clamps the shown or typed value; a
    **hard** range clamps.
  - `ColorSwatch` previews linear colors sRGB-encoded.
- **Editors** (`createPropertyEditor()`, in `property_editors.cpp`):
  - bool
  - numeric scalar, vector and quaternion, including colors (swatch plus R/G/B/A)
  - matrix (summary, expandable grid)
  - string, token and path expression (path expressions validated with `TfErrorMark`)
  - token combo, used when `allowedTokens` is authored
  - asset (Rel/Abs toggle, browse, resolved/unresolved sub-line)
  - array (summary plus `ArrayEditorDialog`)
  - read-only "(no value)" for `opaque`/`group`, and a read-only fallback for unknown
    types.
- **`ArrayEditorDialog`:** a virtual table that edits each element with that element
  type's editor, with Add, Insert, Remove, Resize and Paste Lines.
- **`PropertyRow` and `AttributeBinding`:** as specified in §2.1/§4.1. The binding works
  without an `UndoManager` for scratch stages.
- **Debug > USD Data Types** (`DataTypeGalleryDialog`):
  - The groups are a **sidebar list instead of tabs**, since ten groups overflowed a tab
    bar.
  - Result: **112 types, 0 without a dedicated editor.**
- **Theme:** `dark.qss` has a property-widget section.
- **Verified live:**
  - every group page and the preview toggles
  - scrubbing with the edit-signal log
  - soft-limit sliders
  - matrix expand
  - array summaries, the array dialog and the in-cell vector editor
  - a Move drag undone in one step via both Edit > Undo and Ctrl+Z
  - the table panel showing the dragged value without a reselect

### Phase 3

- **`PropertyCategorizer`** (`usdcc_usd`, Qt-free; `categorizeProperties()`,
  `propertyLabel()`, `propertyLabelInCategory()`, `schemaLabel()`) implements §5 with
  these refinements:
  - **Display groups:** `displayGroup` is honoured only when *authored* in a layer.
    Schema-declared groups (UsdLux's "Basic"/"Advanced", …) would otherwise override
    the schema sections.
  - **Nested schemas:** multiple-apply API instances (a light's
    `CollectionAPI:lightLink`/`shadowLink`) claim their properties before the API that
    builds them in. Otherwise LightAPI swallows 14 collection rows.
  - **Geometry:** PointBased is shown as "Geometry".
  - **Labels** drop what the section title already says (`userProperties:weight` →
    "Weight" in "User Properties", `collection:lightLink:includeRoot` → "Include Root").
    Schema-declared display names win ("Enable Shadows", "Linear Velocity", a light's
    "Diffuse Multiplier").
  - **Material binding:** `material:binding` isn't declared by MaterialBindingAPI, so it
    lands in a namespace section, "Material", labelled "Binding" — which is the interim
    the plan asked for.
  - Unit-tested in `tests/property_categorizer_test.cpp` on Mesh (with transform ops,
    applied APIs, a primvar, custom and display-grouped properties), Sphere, Camera,
    DistantLight, Shader and a typeless prim. It covers the options and the label rules.
- **`AttributesViewPanel`** rewritten:
  - **Layout:** a scroll area with a "Prim" header (name, type, specifier and applied
    schemas read-only; kind, active and instanceable editable and undoable), then one
    `CategorySection` per category.
  - **Rows:** `PropertyRow` and `AttributeBinding` with the phase 2 editors.
    - Rotation ops get a `°` suffix.
    - Primvars show their interpolation as a note.
    - Rows show the unauthored/animated/connected states, and Reset to Default is
      undoable.
    - Relationships show their targets as pills that select them, and can be edited
      (docs/RELATIONSHIPS.md).
  - **Search:** filters by label or name. While searching, sections are force-expanded
    and the Prim header hides.
  - **Options menu** (the panel's ☰ button, left of the stage dropdown): Authored Only
    and Hidden Properties, both persisted.
  - **Collapsed sections** are remembered per category key. A new
    `usdcc::ui::appSettings()` gives panels the app's INI settings file (it was private
    to `MainWindow`).
  - **Live updates:** value changes refresh rows in place. A resync rebuilds only if a
    "layout signature" (type, active, categories and property names) changed, and never
    during an edit gesture.
  - **Transform:** the xformOps are plain rows until phase 4 replaces them with the
    stack editor.
- **Verified live** with `test_data/cube_and_sphere.usda` and a richer scene (a mesh with
  applied APIs, a material binding, a primvar, a namespace and display groups; a distant
  light; a material and a shader with soft limits):
  - the categories, labels and editors (sliders, color swatch, asset field with resolved
    line, connected badge)
  - scrubbing Translate X moves the cube in the viewport, and Edit > Undo restores the
    panel and the viewport
  - search, Authored Only, and a collapsed section staying collapsed across prims
  - the binding's Select jumps to the material
  - deactivating a prim from the header, and undoing it

### Phase 4: transform stack

- **`XformStack`** (`usdcc_usd`, Qt-free) implements every §6 operation:
  - move, add (unique suffixes, identity starting values, inserted at an index)
  - remove, with the attribute deleted only when no remaining op uses it and the edit
    target holds its only opinion
  - addPivot (a `translate:pivot` / `!invert!` pair around an op)
  - rename (value, time samples and inverse references follow)
  - resetToIdentity, resetXformStack
  - bakeToMatrix, and convertToTRS (refused for sheared matrices)

  Bake and convert keep the `!resetXformStack!` marker, which `ClearXformOpOrder()`
  would otherwise drop.
- **Shared maths:** `xform_math` (`usdcc_usd`) has `eulerToMatrix`/`matrixToEuler` for all
  six Euler orders and `composeOps`. `matrixToEuler` checks every candidate by
  recomposing it, rather than trusting a hand-derived sign table. It keeps the result
  nearest-continuous with the previous value, and at gimbal lock holds the last-applied
  angle. The Rotate tool's Auto mode now uses it too. `usdcc_tools` links `usdcc_usd`
  for this; both are Qt-free.
- **`TransformStackEditor`** (`usdcc_usd_ui`) builds the Transform section:
  - **Header:** a Reset Stack checkbox and a square "+" button whose menu has every op
    type plus Bake and Convert.
  - **Rows:** one per op, with a "≡" grip, the label (suffix in parentheses, "⁻¹" for
    inverses), the value's editor (`°` on rotations) and a square "×" remove button.
  - **Context menu:** Move Up/Down, Use as Tool Target, Rename Suffix…, Reset to
    Identity, Add Pivot Around, Remove, Bake, Convert.
  - **Undo:** every action is one undo step.
  - Every xformable prim gets the section, even under Authored Only, since it is where
    ops are added. `xformOp:*` attributes missing from the order still show, dimmed
    (they have no effect) and marked "not in stack". When layers other than the edit
    target define the attribute, which is why removing its op could not delete it, the
    note names the strongest one ("not in stack · from render_default.usd"), and the
    tooltip lists them all. Right-click > Add to Stack puts the attribute back at the
    end of `xformOpOrder`, keeping its value (`XformStack::addExisting()`).
- **Reordering is an in-widget drag** (grip, drop line, move on release) rather than
  `QDrag`, since rows only move within their own list.
- **Fixed along the way:** reordering changes only `xformOpOrder`'s *value*, which USD
  reports as a value change, not a resync. So the panel now compares its layout signature
  (which includes the op order) on every update, not just on resyncs.

### Phase 5: per-op tool targeting

- **Target state:** the target lives on `Stage` (`targetPrim()`, `targetXformOp()`,
  `targetXformOpFor()`, `setTargetXformOp()`, `targetXformOpChanged`) rather than on
  `StageManager`, next to the selection it belongs to.
  - It clears itself when its op leaves `xformOpOrder` (checked on the stage's change
    notice).
  - `StageManager::setSelectedPaths()` clears it when the selection moves to another
    prim.
- **Tools:**
  - `ToolContext::targetOpName` carries the target, filled in by `HydraViewportWindow`
    from `ViewportViewPanel`.
  - `TransformGizmoTool` has `resolveTarget()`, `canDrive()`, `opFrame()` (Op_k · Outer ·
    ParentToWorld), `opValueSpace()` and precision-agnostic `readOpNumbers()` /
    `writeOpNumbers()`.
  - **Auto mode** (no target, or one naming an op not in the stack) runs exactly the old
    code paths, and the existing tool tests still pass.
  - **Move** adds the world delta converted into the op's value space. Space: World still
    gives world axes.
  - **Rotate** rotates the op's own matrix about the ring axis and converts back for all
    six Euler orders. Single-axis ops draw and pick only their ring and add the angle.
    Orient composes quaternions with sign continuity.
  - **Scale** writes the ratio to the targeted op only.
  - **A target the active tool can't drive** gives a grey gizmo; a press on it is
    swallowed with no edit.
- **UI:**
  - Clicking an op label in the stack toggles the target; the row turns blue with "◆".
  - The tool options bar has a **Target** dropdown (Auto plus the ops the active tool can
    drive) that follows the *current* stage. Picking a stack row whose op the active tool
    can't drive switches to the tool that can.
  - With an incompatible tool picked by hand, an orange hint explains the grey gizmo.
- **Tests:** 1,228 checks in total.
  - `xform_math_test` covers USD agreement, 200 random round trips per order, gimbal
    lock and continuity.
  - `xform_stack_test` covers every operation, including a weaker-layer opinion.
  - `xform_target_test` covers the §7.4 list: two scale ops, the pivot sandwich, a
    mid-stack target under a rotated and scaled parent, all six orders, rotateY, orient,
    target clearing, and a mismatched tool.
- **Verified live** (`twoscale.usda`: translate, rotateXYZ, scale, scale:second):
  - the stack rows
  - clicking "Scale (second)" marks it, switches to Scale and fills the Target dropdown
  - the gizmo follows the op's rotated frame, and a drag changed only `scale:second`
    (1.5 → 5.49)
  - drag-reorder
  - the context menu
  - "+ Add" → Rotate Y
  - picking Move by hand greys the gizmo and shows the hint

### Matrix ops

A targeted `transform` (matrix4d) op can be edited with all three tools; there is no
decompose/recompose step, so nothing the user didn't touch can drift.

- **Each tool edits only its part of the matrix:**
  - **Move** adds the drag (converted into the op's value space, as for translate ops) to
    the translation row. The 3×3 part is untouched.
  - **Rotate** turns the 3×3 part about the drawn ring axis (that axis's matrix row, in
    value space), applied after the matrix's own scale: A' = A · Rot. The object turns
    rigidly about the axis you see. Rows stay orthogonal and keep their lengths, so no
    shear appears even with non-uniform scale. The translation row is untouched.
  - **Scale** scales the grabbed axis's row (all three for uniform scale): the matrix's
    own scale, applied before its rotation. The translation row is untouched.
- **Tool choice:** `toolCanDrive()` accepts a matrix op for Move, Rotate and Scale.
  Picking a matrix row while Select is active switches to Move; with one of the three
  already active, the tool stays.
- **Tests:** `xform_target_test` [9] uses a matrix with rotation, non-uniform scale and
  translation, after a translate op and under a rotated parent. Move shifts the op origin
  by exactly the drag and leaves the 3×3 alone. Rotate matches A · Rot(drawn axis), keeps
  the translation, adds no shear and keeps the scale. Scale changes only the grabbed row.
  1,249 checks in total.
- **Verified live** with a box whose only op is a rotated, non-uniformly scaled matrix:
  - picking the row switches to Move with Target "Transform"
  - the Rotate rings follow the matrix
  - a ring drag turns the box about its own Y axis and it stays rectangular

## Later changes

- **Icon buttons.** An array's Edit, a transform op's Remove and the Transform section's Add
  are square icon buttons the height of a value field (`properties::iconButton()`, styled as
  `#propertyIconButton`): a pencil (an eye when read-only), the theme's ×, and a plus. A row
  puts its editor's button (`PropertyEditor::rowButton()`) after everything else, so Edit is
  always the rightmost control.
- **No counts in section headers**, and the Prim header shows the prim's **name**, with its
  path as the tooltip. The name is editable: committing it renames the prim as the Outliner
  does (`prim_actions::rename()`, one undo step), the selection follows the new path, and a
  name that isn't valid or is taken by a sibling is refused (logged; the field goes back).
  An instance proxy's name is read-only.
- **Read-only fields look different**: `#222225` with a faint 5% border, flatter than an
  editable field's recessed `#141416` and without its hover highlight. That's
  `QLineEdit:read-only`, `#propertyReadOnlyText`, `#propertyArraySummary` (an array's
  values, edited only through its Edit button) and `#propertyMatrixSummary` (a matrix, such as
  a Transform op's, edited in the grid it opens) in dark.qss, and `style::readOnlyFill()` for
  the self-painted number fields.
