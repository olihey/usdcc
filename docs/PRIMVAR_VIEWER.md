# Primvar Viewer (investigation)

Status: **phases 1 and 2 are built** (the model, its Python calls and the validation rules: §13; the panel: §14),
after the phase 0
spike (§5.2.1). The rest is a plan: §10 has the recommendation, §12 the questions.

A primvar is any attribute named `primvars:*`: data a renderer interpolates over a prim's surface, curve
or volume and hands to shaders. That covers UV sets, vertex colors, skin weights, per-face ids and
per-object renderer switches, so "a primvar viewer" is several tools. This looks at what primvars are
used for, what usdcc shows of them today, how they depend on each other, and what to build.

Researched in November 2026 against the OpenUSD 26.08 headers this app builds with (`usdGeom/primvar.h`,
`primvarsAPI.h`, `usdImagingGL/engine.h`, `hd/sceneIndexPluginRegistry.h`, `hd/primvarSchema.h`,
`usdValidation/*/validatorTokens.h`) and usdcc's Attribute Editor, Material Editor preview, viewport and
Mesh Tools plugin. Besides §5.2.1 and §13 it's from reading.

## 1. What a primvar is made of

One primvar is up to five things that only mean something together:

| Part | Where | Notes |
|---|---|---|
| Values | `primvars:st` | Any value type; almost always an array |
| Indices | `primvars:st:indices` | Optional. Values are then a table, and the indices have one entry per element |
| Interpolation | metadata on the values | `constant` (1), `uniform` (per face / curve), `varying`, `vertex` (per point), `faceVarying` (per face corner) |
| Element size | metadata | Values per element: 4 skin influences per point is `elementSize = 4` |
| Unauthored values index | metadata on the indices | Which index means "no value" |

- The **type's role** says how to read the numbers: `texCoord2f[]`, `color3f[]`, `normal3f[]`, `point3f[]`,
  `vector3f[]` are all floats.
- **Constant primvars inherit** down the hierarchy (`UsdGeomPrimvarsAPI::FindPrimvarsWithInheritance()`);
  the others don't.
- Values and indices can each have **time samples**, at different times.
- A name can have namespaces (`primvars:skel:jointWeights`, `primvars:arnold:visibility:camera`).

## 2. Use cases

| Use | Typical primvars | Type, interpolation | What a person wants to see |
|---|---|---|---|
| **Texture coordinates** | `st`, `st1`, `uv`, `UVMap`, `map1` | `texCoord2f[]` (older files `float2[]`), faceVarying, usually indexed | The UV layout over the texture; UDIM tiles; flipped, overlapping, stretched or missing UVs; which set a material reads |
| **Color** | `displayColor`, `displayOpacity`, vertex color sets (`Cd`, `color`) | `color3f[]`, `float[]`; any interpolation | The color on the surface; whether it's per object, face or point |
| **Masks for shading** | `dirt`, `wetness`, `ao`, `curvature` | `float[]`, vertex or faceVarying | A heat map with its range; min, max, NaNs |
| **Reference positions** | `rest`, `Pref`, `Nref` | `point3f[]`, vertex | That it's there and matches the point count; the offset from `points` |
| **Orientation** | `normals`, `tangents`, bitangents or a tangent sign | `normal3f[]`, `vector3f[]` / `float4[]` | As color, or as short lines on the surface |
| **Skinning** | `skel:jointIndices`, `skel:jointWeights`, `skel:geomBindTransform` | `int[]` + `float[]` with the same element size; a constant matrix | The weight of one joint as a heat map; unnormalised or out-of-range influences |
| **Ids and partitions** | Houdini's `name` / `path`, object and material ids, group flags | `int[]`, `string[]`, `token[]`, uniform or constant | One color per distinct value; the list of values and how many elements have each |
| **Curves and points** | `widths`, a root UV per curve, `age`, `pscale` | uniform or vertex on `BasisCurves` / `Points` | The same as meshes, with curves' own element counts |
| **Per-instance variation** | primvars on a `PointInstancer` | one per instance | Color by instance |
| **Renderer and object switches** | `arnold:*`, `ri:attributes:*`, `doNotCastShadows`, user data for shaders | scalars, constant, inherited | Which prims have which value, and **where it's inherited from**. Nothing to draw on a surface |
| **From a material** | primvars authored on a `Material` | constant | That the bound material supplies it (Hydra's `HdsiMaterialPrimvarTransferSceneIndex`) |

Two things follow:

1. **There are three kinds of view**, and no one of them covers the table:
   - a **list** (what's there, is it valid, where from, who reads it) for every row,
   - **values in a table** for every row,
   - **drawn on the geometry**, in 3D for most and in 2D for UVs.
2. **Constant primvars are a hierarchy question**, not a geometry one. They need the list and the
   inheritance source, which the Attribute Editor is closest to having.

What other tools do, for reference: Houdini has the Geometry Spreadsheet (a table per domain), visualizers
(color, markers, vectors per attribute) and a UV view; Blender has the Spreadsheet, attribute overlays and
the UV editor; Maya has the UV Editor and Component Editor. usdview shows a primvar as any other attribute
and draws only `displayColor`. The split is the same everywhere: table, overlay, UV.

## 3. What usdcc has

- **Attribute Editor** ([ATTRIBUTE_EDITOR.md](ATTRIBUTE_EDITOR.md) §5 rule 6): a Primvars section, the
  interpolation as a note on each row, and a frame around primvars sharing a first name (`st` with
  `st:indices`). It lists **authored and schema properties of the prim only**: an inherited constant
  primvar doesn't show on the prim that gets it.
- **`ArrayEditorDialog`**: a virtual table of one array, any element type, editable, fine with millions of
  rows. It knows nothing of indices, interpolation or the mesh: `st` and `st:indices` are two dialogs.
- **Material Editor preview** ([MATERIAL_EDITOR.md](MATERIAL_EDITOR.md) §7.9): `primvarsReadBy(material)`
  already answers "which primvars does this material read, and which are UV sets", and the preview lists
  the ones its shape lacks.
- **Mesh Tools** (`src/plugins/blender_mesh_tools/src/conversion.h`): snapshots a mesh's primvars with
  indices, element size and interpolation as owned data, validates them, and writes them back after a
  topology change.
- **Viewport**: draws `displayColor` as Hydra does. No "color by", no element picking
  (`UsdImagingGLEngine`'s pick result has the prim and instance, **not the face or point**), no component
  selection. The overlay renderer draws world-space lines and triangles (gizmos, colliders).
- **Validation** ([VALIDATION.md](VALIDATION.md)): usdcc's rules and OpenUSD's validators. OpenUSD 26.08
  ships **no primvar validator**: `usdGeomValidators` has stage metadata, subsets and encapsulation only.
- **Python**: nothing of usdcc's own; `pxr.UsdGeom.PrimvarsAPI` works as anywhere.

Missing, then: validity at a glance, inherited primvars, values next to their indices and elements, any
drawing of a primvar other than `displayColor`, and everything about UVs.

## 4. Primvars that depend on other things

"Depends on" means five different things. Each needs its own handling.

### 4.1 Structural: the parts of one primvar, and the topology

The value count must match what the interpolation asks of the geometry:

| Interpolation | Mesh | BasisCurves | Points | PointInstancer |
|---|---|---|---|---|
| constant | 1 | 1 | 1 | 1 |
| uniform | faces (the size of `faceVertexCounts`) | curves | 1 | the whole value |
| varying | points | `ComputeVaryingDataSize()` | points | instances |
| vertex | points | points | points | instances |
| faceVarying | sum of `faceVertexCounts` | as varying | points | instances |

- times the **element size**;
- with indices, it's the **index count** that must match, and every index must be inside the values
  (or be the unauthored values index).

A PointInstancer's constant value is whole too: OpenUSD applies it as it is to every instance. Other prims
have no count where USD doesn't give one (a Cube's faces; varying on NURBS curves).

So a primvar depends on `faceVertexCounts`, `points` or `curveVertexCounts`, on its own indices, and on
two pieces of metadata. Break any one and renderers drop the primvar, usually silently.

Composition makes it worse: values, indices and interpolation can come from **different layers**. Authoring
flattened values in a stronger layer over a weaker layer's indices gives a broken primvar unless the
indices are blocked (`UsdGeomPrimvar::BlockIndices()`) in the same edit.

### 4.2 Paired: primvars that are one piece of data in two attributes

| Group | Rule |
|---|---|
| `skel:jointIndices` + `skel:jointWeights` | Same interpolation and element size; indices inside the Skeleton's `joints` (`UsdSkelBindingAPI::ValidateJointIndices()`); weights sum to 1 |
| `tangents` + bitangents or sign + `normals` + a UV set | Tangents are computed from one UV set and the normals; same interpolation |
| `displayColor` + `displayOpacity` | Independent interpolations are legal; both constant or both matching is what's meant |
| `primvars:normals` over `normals`, `primvars:widths` over `widths` | The primvar wins when both exist (`pointBased.h`, `curves.h`, `points.h`). The loser is dead data |
| `primvars:x` + `primvars:x:idFrom` | A string primvar whose value is a relationship's target (`SetIdTarget()`) |

### 4.3 Derived: stale after something else was edited

Normals (from points and topology), tangents (from normals and UVs), `rest` (a copy of points at bind
time), `extent` (from points and widths). USD records none of this. Editing the source leaves them wrong
without any error.

### 4.4 Consumers: who reads it

- **Materials**, by name: `varname` of `UsdPrimvarReader_*`, `geomprop` of MaterialX's geompropvalue,
  the primary UV set implicitly. `primvarsReadBy()` has this. A renamed or missing UV set is the most
  common "my texture is gone".
- **The renderer itself**: `displayColor`, `displayOpacity`, `normals`, `widths`, and its own namespace
  of constant switches.
- **UsdSkel**: the `skel:` group.

### 4.5 Sources: where the value comes from

Authored on the prim; inherited from an ancestor (constant only); on an instancer for its instances; from
the bound material. Inside a native instance it's read-only.

### 4.6 How to handle them

1. **One model, Qt-free: `primvar_info`.** For a prim at a time, a record per primvar, the inherited ones
   included:
   - name, type, role, interpolation, element size, indexed, time-varying;
   - element count expected (and from which attribute) against the count found;
   - source (§4.5) and the layer of each part's strongest opinion;
   - relations (§4.2, §4.3) from a small rule table, by name;
   - readers: the materials bound to the prim that read it (`primvarsReadBy()`), and what they read that
     the prim lacks;
   - problems, each with a severity and a sentence.

   Everything else (the list, the table, the viewport, validation, the Attribute Editor's note) reads this.
2. **Show a primvar as one thing.** Values, indices and metadata are one row with one status
   ("faceVarying, 24 of 24, indexed into 14 values"), which opens to its parts. The Attribute Editor's
   frames are the same idea for editing single attributes.
3. **Validate.** The structural and paired rules as usdcc validation rules, since OpenUSD has none. The
   panel shows the same findings live for the selection.
4. **Edit the group, not the attribute.** Every editing command (§7) takes a whole primvar, writes all its
   parts to the edit target in one undo step, and blocks what would otherwise leak through from a weaker
   layer. The raw array dialog stays for whoever wants the attribute alone.
5. **Flag, don't fix, what's derived.** After an edit to points, topology or a UV set: "Normals may be
   stale" on the row, with the command that recomputes where one exists (Mesh Tools' Recalculate Normals).
   No automatic recompute: USD doesn't say what was derived from what, so a guess would overwrite
   authored data.
6. **Visualize pairs together.** Skin weights are shown per joint, from both primvars; a UV set with the
   texture of the material that reads it.

## 5. Drawing a primvar on the geometry

Hydra and every delegate draw `displayColor` on unshaded geometry, at any interpolation. So the general
answer is: **turn the chosen primvar into colors on the CPU and give it to Hydra as `displayColor`**,
with the same interpolation, and with the same indices (only the value table is converted, which is cheap
for indexed data).

| Data | Colors |
|---|---|
| float, double | A ramp over a range: automatic (min–max of what's shown) or typed |
| int, bool, string, token | One color per distinct value |
| color3f, color4f | As they are |
| normal, vector | `0.5 * v + 0.5`; or lines (§5.3) |
| point (`rest`) | Position in its bounds; or distance from `points` as a ramp |
| texCoord2f | U and V as red and green; a checker needs a texture (§5.3) |
| element size > 1 | One component picked; for skin weights, one joint |
| constant | The whole prim one color: by value across prims |

How the colors get to Hydra:

### 5.1 Options

| | A. Scene index filter | B. A preview stage in the panel | C. Session layer of the scene | D. Debug material |
|---|---|---|---|---|
| How | A filtering scene index after UsdImaging replaces `primvars/displayColor` and drops the material binding of the prims it's on | The Material Editor's trick: a stage of its own, masked to the selected prim, sharing the scene's layers; the colors in its private session layer | Author `displayColor` and unbind materials in the scene's session layer | Bind a `UsdPreviewSurface` reading the primvar, by A or B |
| In the real viewport, in context | yes | no: its own small 3D view | yes | yes |
| Touches the stage | no | no | **yes**: undo, change notices, every panel, shared stages | no |
| Renderers | all that draw `displayColor` | same | same | those with `UsdPreviewSurface` |
| All types and interpolations | yes | yes | yes | colors and vectors only: no ramp, no ints |
| Per pixel (checker) | no | no | no | yes |
| Cost | new Hydra code | a second engine, already built for the preview | — | — |
| Risk | the hook (below) | low | — | — |

**C is out**: a display mode must not write to the user's stage.

**A is the recommendation.** Its hook was the open risk; the spike (§5.2.1) settled it, so B isn't needed.

### 5.2 The hook for A

`UsdImagingGLEngine` has no public "append a scene index". What there is:

- `HdSceneIndexPluginRegistry::RegisterSceneIndexForRenderer(rendererDisplayName, callback, ...)`, with an
  empty name for all renderers. The callback gets the scene index to wrap and a `renderInstanceId`.
- The engine gives each render index an id (`UsdImagingGLEngine_%s_%p`, seen in `usd_usdImagingGL.dll`).
- The callback runs while the engine makes its render index, inside calls usdcc makes on the GUI thread
  (construction, `SetRendererPlugin()`), so `HydraViewportWindow` can mark "this is mine" around them
  rather than parse the id. Other engines (the selection overlay's, the Material Editor preview's) get no
  filter.
- The filter is idle until a viewport sets a primvar name; then, for each prim that has it, it supplies
  `displayColor` lazily from the primvar's own data source, so time samples and edits follow without
  usdcc tracking them.

The simplest form needs no path mapping at all: **"color everything by primvar `X`"**, per viewport.
Prims without `X` go grey. Instances and prototypes come for free, as the filter sees Hydra's prims.

### 5.2.1 The spike (phase 0, 2026-11-25): it works

Built as §5.2 says (`src/cpp/usd/src/primvar_color_spike.*`, hooked into `hydra_viewport_window.cpp`;
uncommitted, not for merging as it is) and run in the app on a stage with a grid mesh (`st` faceVarying
and indexed, a float per point, an int per face), a triangle (`st` per point, a constant float, a constant
string), a Cube with no primvars, two materials and a dome light. A script switched the primvar and the
renderer and saved the viewport's own picture after each.

| Checked | Result |
|---|---|
| The callback is reached, with an id per engine | Yes: `UsdImagingGLEngine_GL_<pointer>`, `UsdImagingGLEngine_Embree_<pointer>` |
| The claim tells the viewport's engine from others | Yes: the selection overlay's engine came through "not claimed", and its outline draws over the colors |
| The engine is in scene index mode | Yes: the filter sees the task controller's prims (`/_UsdImaging_GL_.../renderTask_*`, its camera and lights) beside the stage's |
| **Storm** draws it | `st` as red and green (faceVarying with indices, and per point), the float as a ramp (per point), the int as a color per value (per face), a type without colors magenta, prims without the primvar grey; materials gone while it's on |
| **Embree** draws it | The same, for `st` and the int |
| Another primvar, the engine as it is | Yes: dirtying `primvars` and `materialBindings` of the gprims is enough; back to none gives the materials back |
| An edit to the primvar follows | Yes, with no code of usdcc's between: the values were changed from Python and the next frame had them |
| A renderer switch | The new render index calls back again and gets a new filter; the name set on it before the scene fills shows on the first frame |

Found on the way:

- **`HdPrimTypeIsGprim()` is false for implicit surfaces** (`cube`, `sphere`, ...), which at this point in
  the chain aren't meshes yet: the first run left the Cube in its material. The filter needs its own list,
  or to sit after the delegate's conversion.
- **The colors are lit**: Storm's camera light and Embree's dome shade them. Reading values wants them flat
  (`params.enableLighting = false` on Storm; not tried), and the view transform applies to them as to any
  color, so a legend has to go through it too.
- **Embree needs a light to show anything**: on the stage without one the meshes were black with or
  without the filter. Not the filter's doing, but Color By will meet it.
- The spike dirties by walking every prim; the real one keeps the gprims' paths from the added and removed
  notices.

**Not checked:** native instances and point instancers, curves and points, time samples, a heavy mesh
(the colors are made again on every sync of the primvar), two viewports with different primvars at once,
the Material Editor's preview (a viewport too, so it claims a filter), `displayOpacity`, other delegates.

### 5.3 What `displayColor` can't do

- **Checker and texture on a UV set**: per pixel, so it needs a material (D): a `UsdPreviewSurface` with a
  `UsdUVTexture` and a reader of the chosen set, added and bound by the same filter. Later.
- **Vectors as lines** (normals, tangents, velocities): the viewport's overlay renderer, as the collider
  overlay does, for the selection only. Later.
- **A legend**: the ramp and its range, or the distinct values, on the HUD ([VIEWPORT_HUD.md](VIEWPORT_HUD.md)).

Not this way: render-delegate AOVs (`primvars:st` exists in Embree; each delegate differs) and Storm's
shaders (Storm only).

## 6. The UV view

A 2D panel area showing a UV set of the selected meshes.

- **Which sets**: every primvar with two float components, `texCoord2f[]` first. The set a bound material
  reads is marked.
- **Drawn**: the faces' edges in UV space, through the indices; filled faintly so overlaps darken.
  Any interpolation: faceVarying and vertex give a layout, uniform and constant give points.
- **Behind it**: a grid with the 0–1 tile marked, and UDIM tiles (1001, 1002, ...) when UVs leave it;
  a checker; or a texture, from the `UsdUVTexture` nodes of the bound material that read this set
  (`<UDIM>` paths per tile).
- **Diagnostics**, as colors on the layout and as counts:
  - flipped faces (signed area), degenerate faces (zero area);
  - UV borders (edges whose two sides have different UVs);
  - outside 0–1, and which UDIM tiles are used;
  - later: overlaps between shells, stretch (3D area against UV area), texel density.
- **Subdivision**: the layout is the cage's. `faceVaryingLinearInterpolation` changes what a renderer
  makes of it at the borders; the view says which rule is set.
- **Drawing it**: `QPainter` with a cached picture is enough to start (the Curve Editor and Dope Sheet are
  `QPainter`); a mesh with millions of faces needs the layout built on a worker and drawn at the zoom's
  level of detail, or GL through a `QWindow` as the viewport is. Not a `QOpenGLWidget`: PLAN.md §6 item 12.
- **Linked to 3D**: hovering a face in the UV view outlines it in the viewport (overlay lines). The other
  way needs face picking in the viewport, which `UsdImagingGLEngine`'s pick doesn't give: a ray against
  the selected mesh on the CPU, or Storm's element id buffer. Later.

## 7. Editing

USD writes whole arrays: every edit, however small, sets the full values (and indices) on the edit target.
That's fine for commands and for a drag that ends in one write, and it rules nothing out below.

| Level | What | Verdict |
|---|---|---|
| **1. Structure** | Create, remove, block, rename, duplicate (`st` → `st1`); change the type's role (`float2[]` → `texCoord2f[]`); change interpolation with conversion (vertex → faceVarying, uniform → faceVarying, constant → any); add indices (merge equal values) or flatten them; set element size; copy from another prim with the same topology | **Do.** Small, each one a command on a whole primvar (§4.6 point 4), and it's what fixes most broken files |
| **2. Values in the table** | Edit a cell; fill, add, multiply, clamp, remap the selected rows | **Do**, after the read-only table. The per-type cell editors exist |
| **3. UV transforms** | Move, rotate, scale, flip selected UVs or shells in the UV view; fit to 0–1; move to a UDIM tile | **Later.** Needs selection and tools in the 2D view; worth it once the view is used |
| **4. Unwrap, relax, pack** | New UVs from the geometry | **Not here.** Mesh Tools is where Blender's algorithms are ([BLENDER_MESH_TOOLS.md](BLENDER_MESH_TOOLS.md) lists UV unwrapping as later work) |
| **5. Painting** | Colors or weights with a brush in the viewport | **No.** A DCC feature: brushes, falloff, symmetry, component picking |

Rules for all of them:

- One undo step; refused with the reason on an instance proxy, a locked layer, or a result that §4.1
  would call broken.
- **Time samples**: the viewer reads at the stage's time. Editing a primvar that has time samples is
  refused at first, rather than guessing between "this sample" and "all".
- Where the strongest opinion of any part is in another layer, the command says what it will override.

## 8. The panel

One new view panel, **Primvars**, following the selection as the Attribute Editor does:

```
┌ Primvars ────────────────────────────────────────────────────────────┐
│ ☰  Stage ▾                                   [Show in Viewport ▾]    │
├──────────────────────────────────────────────────────────────────────┤
│ Name            Type          Interp.      Elements        Source    │
│ st              texCoord2f[]  faceVarying  24 of 24 ✓ idx  local  ◉M │
│ displayColor    color3f[]     constant     1 ✓             /World    │
│ skel            (2)           vertex       8 × 4 ✓         local     │
│ dirt            float[]       vertex       7 of 8 ⚠        local     │
├──────────────────────────────────────────────────────────────────────┤
│ [Values] [UV] [Info]                                                 │
│  the table of the picked primvar's domain / the UV view / its        │
│  parts, layers, relations, readers, statistics, problems             │
└──────────────────────────────────────────────────────────────────────┘
```

- **List**: a row per primvar or paired group; inherited and material-supplied ones in italics with their
  source, which is a link that selects it. `◉M`: a bound material reads it. A last row for what a material
  reads and the prim lacks.
- **Values**: rows are the elements of the picked primvar's domain (faces, points, face corners), columns
  are **all primvars of that interpolation**, plus `points` for vertex. An indexed primvar shows the value
  with its index. Virtual, as `ArrayEditorDialog`'s model is. Copy as text; sort and filter later.
- **Info**: statistics (min, max, mean, distinct values, NaN and infinite counts) computed off the GUI
  thread ([PANEL_BUSY.md](PANEL_BUSY.md)).
- **Show in Viewport**: sets the active viewport's color-by (§5); the choice lives in the viewport's
  display options ([VIEWPORT_DISPLAY.md](VIEWPORT_DISPLAY.md) §4): per viewport, in sessions, in the
  options menu as **Color By ▸**.
- Several prims selected: the list is the union, with "3 of 5 prims"; Values and UV take the meshes that
  have the picked primvar.

Elsewhere:

- **Attribute Editor**: a primvar row's note gains the ⚠ of `primvar_info`, and the Primvars section a
  button that opens this panel on it. Inherited constant primvars could be listed there too, read-only.
- **Validation**: the rules of §4.6 point 3.
- **Material Editor**: its "missing primvars" line can link here.
- **Python**: a `usdcc.primvars` module (§9.2), and color-by on `usdcc.app`'s `Viewport`.

The tabs are **primvar views** (§9.2): Values and Info are built in, the UV view comes from a plugin, and
any of them can be opened as a panel of its own.

## 9. Extending it

Yes: the panel can be a host, and the UV view can be a plugin. usdcc already ships features as plugins
(Mesh Tools, V-Ray Render, RTX Render), and both plugin kinds can add panels, tools, menu
entries and validation rules today ([PLUGINS.md](PLUGINS.md) §1, [PLUGIN_SDK.md](PLUGIN_SDK.md)).

### 9.1 What works without anything new

A plugin can already register a view panel that follows the selection, read primvars with `pxr`, edit
them as one undo step, add validation rules, and add a viewport tool that draws lines. So a UV viewer
*could* be written today as a stand-alone plugin. It would then be alone: its own idea of which primvar
is picked, its own validity checks, no part in the Primvars panel, no shared highlight with the table or
the viewport. The extension points below are what make plugins part of one tool.

### 9.2 Extension points

Each is a registry the built-in parts go through too, as [PLUGINS.md](PLUGINS.md) §2 has it ("the existing
calls are the API"): an interface the built-ins can't live with is found out at once.

| # | Extension point | C++ (`Host`) | Python | The plugin gets or gives |
|---|---|---|---|---|
| 1 | **Data** (no registry; built, §13) | `usdcc/usd/primvar_info.h`, Qt-free, exported | `usdcc.primvars.info(prim)`, `.values(prim, name, flattened=False)`, `.element_count(prim, interpolation)` | The records of §4.6: validity, expected counts, source, relations, readers |
| 2 | **Shared state** (the focus built, §14) | on `Stage`, with signals | `usdcc.primvars.focus()`, `set_focus()`, `element_selection()`, `set_element_selection()`; events `primvar_focus_changed`, `element_selection_changed` | Which primvar is picked for a stage; which elements (prim, domain, indices) are selected or hovered |
| 3 | **Primvar views** (built in C++, §14) | `addPrimvarView(type)` | `usdcc.primvars.register_view(id, label, cls, applies=None)`, `cls` a `usdcc.ui.PrimvarView` | A tab in the Primvars panel: a widget told the stage, prims, primvar and time, with session state. `applies(records)` decides when the tab shows (the UV view: two float components) |
| 4 | **Primvar commands** | `addPrimvarMenuAction(label, run, applies)` | `usdcc.primvars.add_menu_action(label, callback, applies=None)` | An entry in the list's context menu and the panel's options, one undo step, as the prim menu's entries are. The context has the prims, the primvars and the element selection |
| 5 | **Authoring** (no registry) | `usdcc/usd/primvar_edit.h` | `usdcc.primvars.author(prim, name, values, indices=None, interpolation=..., element_size=1)` | Writes a whole primvar as a group (§4.6 point 4): refuses what §4.1 would call broken, blocks weaker indices |
| 6 | **Colorizers** | `addPrimvarColorizer(...)` | `usdcc.primvars.register_colorizer(id, label, function, applies=None, options=...)` | A Color By mode (§5): values in, colors out, with options described as data, as a tool's are |
| 7 | **Viewport overlays** | `addViewportOverlay(id, label, build)` | `usdcc.app.add_viewport_overlay(id, label, build)` | World-space lines and triangles over the frame, switched per viewport like Show Colliders, without being the active tool. `build(stage, time, selection)` returns what a tool's gizmo does |
| 8 | **Relations** (built, §13) | `PrimvarRelations::instance().set(...)` | `usdcc.primvars.register_relation(id, label, members, ...)` | A row for §4.2's table: a studio's or a renderer's own groups (`primvars:groom:*`), shown as one row and checked together |
| — | **Validation rules** | (a USD plugin's validators) | `usdcc.validation.rule(...)` | Exists |

Points 2 and 7 are the ones that don't follow from the panel itself, and the ones a UV plugin most needs:
without a shared element selection a hover in the UV view can't light up a face in the viewport or a row
in the table.

### 9.3 Limits

- **Color By's Hydra code stays in the app.** A colorizer only turns values into colors. A C++ one is
  called wherever Hydra asks (any thread: it must be a pure function). A **Python one can't be**: one
  Python, one GIL ([SCRIPTING.md](SCRIPTING.md) §3.2), and Hydra syncs on worker threads. So Python
  colorizers run on the GUI thread, ahead of the frame, **for the selected prims only**, and the filter
  gets the cached colors.
- **Python and big meshes.** A Python view gets whole arrays (`Vt` arrays, which numpy reads through
  the buffer protocol), never a call per element. That's fine for statistics, plots and commands. A UV layout of a
  million faces drawn from Python isn't.
- **Python UI needs PySide6** built for the app (`tools/build_pyside.py`), as every Python panel does.
- **C++ plugins match the app's build** (`built_for`), so the C++ side is an API, not an ABI: recompiled
  with each release. Python is the surface that stays put. Until the UV plugin has been through editing,
  the primvar calls are marked provisional (the manifest's `"api"` stays 1).
- **Element selection and undo.** The prim selection is part of a stage's undo history; the element
  selection would follow it (cleared when the prims change), or stay out. §12 question 7.

### 9.4 The UV view as a plugin

**Recommended: yes, a C++ plugin shipped and on by default**, `org.usdcc.uv-view` in `src/plugins/uv_view`,
as Mesh Tools is.

| | In the app | As a shipped plugin |
|---|---|---|
| Work before the first UV pixel | none | points 1, 2, 3 and 7 |
| Proves the API | no | yes: the largest client there'll be |
| Size of `usdcc_usd_ui` | grows with every UV tool | unchanged |
| Can be swapped or switched off | no | yes; a studio's own UV tool registers the same way |
| Unwrap from Mesh Tools | reaches into the app | a primvar command (point 4) the UV view shows without knowing it |
| Isolation | — | little: it shares the app's libraries and can crash it as any C++ plugin can |

It uses 1 (the layout's data), 2 (focus and element selection), 3 (its tab, or its own panel), 4 and 5
(its transform tools, later) and 7 (the face under the cursor, in 3D). It needs nothing of 6.

**C++ rather than Python**, for the reason in §9.3. A small **Python example** goes beside
`measure_example`: a histogram view of the picked primvar and a colorizer, to keep the Python side honest.

The cost is in the order of work, not its amount: the registries come before the UV view instead of
never. My estimate is a fifth more on phases 2 and 3 (a guess, not measured), and the Values and Info
tabs are cleaner for it.

## 10. Recommendation

1. Build the **`primvar_info` model first**. It is the answer to the dependency question, and every view
   and every plugin reads it.
2. A **Primvars panel that hosts views**: list with validity and sources, then the values table, both
   through the registry plugins use (§9.2).
3. **Color By in the viewport** through a scene index filter (§5.2), which the spike showed working on
   Storm and Embree (§5.2.1).
4. A **read-only UV view** with diagnostics, **as a shipped C++ plugin** (§9.4).
5. **Editing in two steps**: structural commands, then values in the table. UV tools after that; no
   unwrap, no painting.

A viewer is certain; an editor is realistic at levels 1 and 2, and level 3 is a decision for later.

## 11. Phases

| Phase | Contents | Size | Checked by |
|---|---|---|---|
| **0. Spike** — **done** | The scene index callback on usdcc's engine: `displayColor` replaced on Storm and Embree (§5.2) | S | In the app, §5.2.1: route A for phase 5 |
| **1. Model** — **done** | `primvar_info`, exported: records, expected counts for Mesh, BasisCurves, Points, PointInstancer; inheritance and material sources; the relation table and its registry; problems. `usdcc.primvars`' read calls. Validation rules. Test stages with each kind of broken primvar | M | Unit tests, Python tests |
| **2. Panel: list and info** — **done** | The panel, following the selection; the list; the primvar focus on `StageManager`; the view registry with Info as its first view, statistics on a worker; Open in Own Panel; the Attribute Editor's ⚠ and link | M | UI tests; in the app on the shader ball |
| **3. Values** | The table per domain as a registered view, indexed values, copy. The Python example plugin (histogram view) | M | UI tests, a mesh with a million points; the example's test |
| **4. Element selection and overlays** | The shared element selection, the table's rows following it; the viewport overlay registry, with the selected elements as its first overlay | M | UI tests; in the app |
| **5. Color By** | The filter (from the spike, with what §5.2.1 found and left unchecked), the colorizer registry with the color maps of §5, the display option, menu, session, HUD legend | M–L | Tests of the mapping; in the app on Storm and Embree |
| **6. UV view plugin** | `src/plugins/uv_view`: layout, grid and UDIM tiles, checker and texture, flipped / degenerate / borders, hover linked to 3D through phase 4 | L | Tests of the layout and diagnostics; in the app, and without the plugin |
| **7. Editing: structure** | `primvar_edit` and the commands of §7 level 1, through the command registry | M | Unit tests per command, across layers |
| **8. Editing: values** | Cell edits and bulk operations | M | UI tests |
| Later | Checker in the viewport, vector lines, face picking in the viewport, UV transforms, overlaps and stretch, Unwrap from Mesh Tools | | |

Phases 1 to 3 need nothing from Hydra and are useful alone. 5 is independent of 4 and 6; 6 needs 4 only
for the linked hover.

## 12. Questions

1. **One panel or two?** Settled by §9.2 point 3 if views are taken: the UV view is a tab, and any tab
   opens as a panel of its own.
2. **Color By per viewport, for everything shown, by name** (recommended, §5.2) or only on the selection?
   By name needs no path mapping and compares prims; selection-only keeps the rest of the scene shaded.
3. **How far should editing go in the first version?** Recommended: none until phase 7; then structure.
4. **Time-sampled primvars**: refuse edits at first (recommended), or edit the sample at the current time?
5. **Inherited primvars in the Attribute Editor** too, or only in the new panel?
6. **Curves and points in the first version**, or meshes only? The model has them (§13); the views are
   still open, the UV view being meshes only either way.
7. **Is the element selection part of undo**, as the prim selection is? Recommended: no at first; it's
   cleared when the prim selection changes.
8. **All eight extension points, or the UV plugin's five first** (data, shared state, views, authoring,
   overlays)? Recommended: those five as their phases come; commands, colorizers and relations with the
   phase that builds their first built-in.
9. **The UV view: shipped plugin** (recommended, §9.4) **or in the app** with the registries still open
   to others?

## 13. As built: phase 1

- **`primvar_info.h`** (`usdcc_usd`, Qt-free): `primvarInfo(prim, time, options)` gives a `PrimvarSet`: a
  `PrimvarInfo` for each primvar with an opinion (a schema's fallback isn't one), the groups they form, what
  the bound materials read and the prim lacks, and the problems of each. `primvarElementCounts()` has §4.1's
  table. At the earliest time unless told.
- **Sources:** local, inherited, and the bound material's, in that order. One declared locally without a value
  doesn't hide an inherited one. Problems are found where a primvar is authored, so one mistake on a parent
  isn't repeated on everything under it.
- **Materials** are the ones for each purpose and the ones of `materialBind` subsets, read with the Material
  Editor's `primvarsReadBy()`; what a renderer always has (`displayColor`, `normals`, ...) is never missing.
- **Layers:** the strongest for the values, the indices and the interpolation, each. An index outside the values
  names them when they differ: values authored over a weaker layer's indices.

| Problem | Severity | When |
|---|---|---|
| `ElementCount` | error | The count isn't what the interpolation and element size need. Says which interpolation would fit |
| `IndexRange` | error | Indices outside the values, but for the unauthored values index |
| `IndicesType` | error | Indices that aren't `int[]` |
| `Interpolation`, `ElementSize` | error | A token that isn't one; a size below 1 |
| `Incomplete` | error | Joint indices without weights, or the other way round |
| `InterpolationDiffers`, `ElementSizeDiffers` | error | Between members of a group that have to agree |
| `JointRange` | error | Joint indices outside `skel:joints`, or the Skeleton's |
| `NoInterpolation` | warning | None authored, so constant, and as many values as another needs |
| `ConstantArray` | warning | Constant, several values, and of a type that's one value an element (a color, a UV) |
| `NothingToInterpolate` | warning | Not constant, on an Xform or Scope |
| `WeightSum` | warning | Joint weights that don't add up to 1 |
| `NonFinite` | warning | NaN or infinity |
| `IndicesOnly` | warning | `primvars:x:indices` without `primvars:x` |
| `IdTarget` | warning | A path to something that isn't there |
| `Overrides` | info | `primvars:normals` or `widths` beside the attribute, which isn't used then |

- **Relations** (`PrimvarRelations`): skinning, display color, tangents and normals are built in. A relation can
  ask for all its members, the same interpolation and the same element size, say what it's derived from, and
  in C++ have a check of its own. **Python ones have no check:** the model is read on worker threads; a
  validation rule does that.
- **Validation:** `usdccValidators:Primvars` and `usdccValidators:PrimvarGroups`, registered with OpenUSD's
  registry when the rules are first asked for, so in the app, `usdcc.validation.run()` and `usdcc_node validate`,
  in every profile that doesn't turn them off. Not in `usdchecker`, which doesn't load usdcc. What a material
  reads and a prim lacks isn't a rule: walking the networks for every prim would cost too much.
- **Python:** `usdcc.primvars`, in the app and in a plain Python, on any `Usd.Stage`. Provisional (§9.3).
- **`test_data/broken_primvars.usda`** has a prim for each problem but `Interpolation`, `IdTarget` and a bad
  `ElementSize` value, named after it.
- **Not handled:** a PointInstancer's primvars on its prototypes' prims; several times at once (a topology that
  changes is checked at one); values from value clips name no layer; staleness (§4.3) is only named
  (`derivedFrom`), not detected.

**Checked:** `primvar_info_test` and `primvars_python_test`; no problems on any stage under `resources` and
`test_data` but the broken one. On 20,000 meshes with an indexed UV set and a color: the two rules 0.19 s, OpenUSD's
0.32 s without them, with a build running beside. A mesh of 2 million faces with 8 million indices: 10 ms, 2 ms
without the scan. **Not tried:** Linux, a production stage, value clips.

## 14. As built: phase 2

- **The Primvars panel** (`PrimvarsViewPanel`, panel type `primvars`, in the Stages menu): the selected
  prims' primvars, a row each, under their group's row when they're in one (Skinning, Display color).
  Columns: name, type, interpolation, elements ("24 of 24, indexed into 14"), source ("here", "from World",
  "material Red") and the materials that read it. A dot in the problem's colour, the messages as its
  tooltip; inherited and the material's in italics; a row for what a material reads and the prim lacks.
  The toolbar says how many, on what, and what's wrong ("3 primvars on SkinningValues, 1 error, 1 warning").
- **Several prims:** the union by name, each row saying on how many ("(on 2 of 3)"), each problem which
  prim it's on. At most 100 prims are read (`PrimvarViewContext::kMaxPrims`).
- **Following:** the selection at once; edits and the time after 100 ms, coalesced
  (`PrimvarContextTracker`, which any view or panel can use).
- **The primvar focus** is the stage's (`Stage::primvarFocus()`, `primvarFocusChanged`), not
  `StageManager`'s as §11 had it: every panel on the stage shares it, and it stays when the selection
  changes. Picking a row sets it; a group's row focuses its first member. Python: `usdcc.primvars.focus()`
  and `set_focus()`. No app event for it yet.
- **Views** (`PrimvarViews`, `PrimvarView`): tabs under the list, each shown where its `applies()` says.
  **Info** is built in: the parts and their layers, the source, elements, readers, what it's derived from,
  its groups and problems, and statistics (min, max, mean for each component, NaN and infinity,
  distinct values and the commonest), worked out on a worker thread from the flattened values, or from the
  values when indices are outside them. Numbers in the user's locale.
- **Open in Own Panel** (the tab bar's button and menu): every view is a panel type too,
  `primvarView.<id>` ("Primvar Info"), with a choice of the primvar in its toolbar. A plugin's view gets one
  as it's added, and loses it when removed.
- **C++ plugins:** `Host::addPrimvarView(type)`. **Python views aren't there yet:** phase 3, with the
  example plugin.
- **Attribute Editor:** a primvar row's note gets a sign and the problem's colour ("⚠ vertex"), the
  messages as its tooltip; the Primvars section's header has a button to the panel, and each primvar row's
  menu Show in Primvars Panel, which focuses it.
- **Sessions** keep the panel's split and tab, and what each view saves.

**Checked:** `primvars_panel_test` (list, groups, union, missing, edits, focus across panels, Info and its
statistics, a plugin's view coming and going, Open in Own Panel, sessions, the Attribute Editor's notes) and
`python_primvars_focus`. In the app, with a test profile: the Standard USD Shader Ball's meshes (`st`,
23,352 of 23,352, its interpolation from `subdiv.usda`; three selected, the union) and the broken stage, with
its notes in the Attribute Editor. **Not tried:** Linux, a big selection's speed, a plugin's view in the app.
