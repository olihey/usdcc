# Material Editor (plan)

A node graph view panel for UsdShade networks: create shader nodes, connect them, group them into
NodeGraphs and lay them out. The stage is the only data store. The graph draws what is in the stage,
and every edit is a USD edit through `UndoManager`, so the Outliner, the Attribute Editor and undo
keep working without changes.

Status: **plan**. Nothing here is built yet. §2 and §3 were researched in November 2026 against the
local USD build (`USD_INSTALL`, `pxr.h` 0.26.8), the local OpenDCC checkout, and the upstream repos
named in each section.

## 1. Scope

- **Edits:** Material, NodeGraph and Shader prims; input/output connections; NodeGraph interface
  inputs and outputs; per-renderer Material terminals (`outputs:surface`, `outputs:mtlx:surface`,
  `outputs:arnold:surface`, ...).
- **Layout lives in USD:** `UsdUINodeGraphNodeAPI` (`pos`, `size`, `displayColor`,
  `expansionState`, `stackingOrder`, `icon`, `docURI`) and `UsdUIBackdrop` (`description`). Layout
  therefore survives a save and round-trips to other DCCs.
- **Shader systems:** USD preview (`glslfx`), MaterialX (`mtlx`), Arnold (`arnold`), V-Ray and Cycles.
  MDL (RTX) is out of scope, see §3.4.
- **Not in scope:** shader preview swatches, editing `.mtlx` documents, and OSL authoring.

## 2. The node graph framework

### 2.1 Candidates

| Library | License | Activity | Qt6 | Model/view | Fit |
|---|---|---|---|---|---|
| [QtNodes](https://github.com/paceholder/nodeeditor) (paceholder/nodeeditor) | BSD-3 | 3.7k★, tag 3.0.16 (Feb 2026), commits through Jul 2026 | yes (default) | **yes**: we subclass `AbstractGraphModel` | **Recommended** |
| OpenDCC [`opendcc.ui.node_editor`](https://github.com/shapefx/OpenDCC) | Apache-2.0 | 188★, last push Dec 2025, upstream still Qt5 | local fork only | yes: abstract `GraphModel` with string ids | Fallback, and source to port from |
| [QuickQanava](https://github.com/cneben/QuickQanava) | BSD-3 | 1.4k★, release Sep 2024 | 6.5+, QML | no: owns its graph | Weak: QML, mirrors data |
| [NodeGraphQt](https://github.com/jchanvfx/NodeGraphQt) | MIT | 1.8k★, Feb 2026 | via Qt.py | no: owns its data | Pure Python. UX reference only |
| [noodles](https://github.com/facebookexperimental/noodles) | MIT | new (Apr 2026), experimental | GL, host supplies context | yes | Revisit in 2027 |
| imgui-node-editor, imnodes | MIT | active | ImGui | — | Not embeddable in a widgets app |

- No other credible Qt6 C++ node editor turned up. The other Qt repos found are stale (2016–2023),
  tiny or GPL.
- vcpkg has none of these. We pick one, pin a commit and add an overlay port.
- **Other open-source USD-aware editors:**
  - OpenDCC's `usd_editor.material_editor` and `usd_node_editor` are the closest prior art. They
    cover UsdShade, NodeGraph grouping, backdrops and Graphviz layout. They depend on OpenDCC's undo
    router and stage watcher, so they are code to port from, not a library to link.
  - [QuiltiX](https://github.com/PrismPipeline/QuiltiX) is a Python MaterialX editor built on
    NodeGraphQt.
  - The MaterialX Graph Editor (ImGui) is a reference for MaterialX node UI.
  - LookdevX and the Omniverse graph are closed or tied to Kit.

### 2.2 Recommendation: QtNodes with a UsdShade graph model

- **Why it fits:**
  - It has the only maintained Qt6 widgets codebase with a real model/view split.
  - `AbstractGraphModel` provides the hooks we need: `connectionPossible`, `detachPossible`,
    `nodeData`/`portData` roles and `nodeFlags`.
  - Painting is pluggable (`AbstractNodePainter`, `AbstractNodeGeometry`), so nodes can look like the
    rest of the app's theme.
- **Gaps we fill ourselves:**
  - **Ids:** `NodeId` and `PortIndex` are unsigned ints. The model keeps a two-way map between `NodeId`
    and `SdfPath`, and between port index and input/output name. The map is rebuilt when prims are
    renamed or reparented.
  - **Undo:** QtNodes' own undo is a `QUndoStack` on `BasicGraphicsScene` that saves and reloads JSON.
    We don't use it. Every model mutation goes through `UndoManager::apply()`. A node drag is an edit
    session: `beginEdit()` on press, live `pos` sets, `endEdit()` on release.
  - **NodeGraphs:** QtNodes groups live only in the scene, so we don't use them. We model a NodeGraph
    as a subnet instead:
    - From outside it is one node, whose ports are its interface `inputs:`/`outputs:`.
    - Double-clicking it enters it.
    - A breadcrumb bar walks back up.
    - Inside, the interface shows as two edge nodes ("inputs" and "outputs").
    - Houdini and Katana work this way, and it matches UsdShade's encapsulation rules.
  - **Backdrops:** our own `QGraphicsItem` behind the nodes, backed by `UsdUIBackdrop`. Moving one
    moves the nodes on it.
  - **Tab search, Graphviz auto-layout, shake to disconnect:** ported from OpenDCC (Apache-2.0, with
    attribution in `THIRD_PARTY_LICENSES`).
- **Known issues to handle in the spike (§5, phase 0):**
  - QtNodes issue #529: a `QOpenGLWidget` viewport in ADS tabs turns black. Keep the default raster
    viewport.
  - Node moves no longer reach the model (#525/#526). Pin a commit that includes fix PR #533, or patch
    it in the overlay port.
- **Fallback:** if the spike shows QtNodes fighting us, especially on custom geometry or on
  subnet-style navigation, vendor OpenDCC's `ui.node_editor` instead. It is about 3.2k lines and its
  only dependencies are OpenDCC's namespace macros, a hash helper and Graphviz. It already has string
  ids and tab search. The cost is that we own an unmaintained fork.

## 3. Which shading nodes exist: the Sdr registry

### 3.1 The automatic part

`SdrRegistry` (`pxr/usd/sdr`) is the single, renderer-neutral catalog of shader node definitions.
Ndr was removed in 25.08, so everything below is Sdr.

- **Discovery is per plugin:** each renderer's USD plugin registers an `SdrDiscoveryPlugin` and an
  `SdrParserPlugin`. Its nodes appear once its `plugInfo.json` is on `PXR_PLUGINPATH_NAME`.
  `PXR_SDR_DISABLE_PLUGINS` can turn one off.
- **Listing:**
  - `GetAllShaderNodes()` (26.03+) or `GetShaderNodeIdentifiers()` + `GetShaderNodeByIdentifier()`.
  - The default `SdrVersionFilterDefaultOnly` keeps only the default version, which matters for
    MaterialX.
  - `RunQuery(SdrShaderNodeQuery().SelectDistinct(...))` with `SdrShaderNodeQueryUtils::GroupQueryResults`
    returns nested groups that map directly onto a palette tree.
- **What the render delegate supports:**
  - `HdRenderDelegate::GetShaderSourceTypes()` lists the shading systems it reads, for example
    `glslfx` and `mtlx` for Storm.
  - `GetMaterialRenderContexts()` lists its Material terminals.
  - Filtering Sdr nodes by those systems answers "which nodes does this delegate offer" without a
    hand-written list.
  - V-Ray and RTX don't run as Hydra delegates in-process, so their plugin manifest declares the same
    two lists.
- **Node fields** (26.05 renamed family → *function* and sourceType → *shadingSystem*):
  - `GetShadingSystem()`
  - `GetContext()` (surface, displacement, volume, pattern, light, ...)
  - `GetRole()` (texture, math, primvar, ...)
  - `GetFunction()`, `GetLabel()`, `GetHelp()`
  - `GetDomain()`/`GetSubdomain()`
  - `GetOpenPages()`/`GetPagesShownIf()`
  - `GetCategory()` and `GetDepartments()` are deprecated.
- **Property fields, for ports and the Attribute Editor:**
  - `GetTypeAsSdfType()`, `GetDefaultValueAsSdfType()`
  - `IsConnectable()`, `GetValidConnectionTypes()`, `CanConnectTo()`
  - `GetPage()` (":" nests), `GetLabel()`, `GetHelp()`, `GetWidget()`, `GetHints()`, `GetOptions()`
  - `GetShownIf()` (an `SdfBooleanExpression`)
  - `IsAssetIdentifier()`, `IsDynamicArray()`, and vstruct membership.

### 3.2 Categories in the palette

There are three levels: **shading system → context → role**. Leaves show the node's label, and the
identifier appears in the tooltip.

| Level | Source | Example |
|---|---|---|
| 1. Renderer | `GetShadingSystem()`, mapped to a display name and icon by a renderer profile (§3.3) | MaterialX |
| 2. Context | `GetContext()`. Read it from `GetMetadataObject()` for usda-defined nodes, whose `GetContext()` returns "usda" | Surface / Pattern / Displacement |
| 3. Group | `GetRole()`. For MaterialX this is the nodedef's `nodegroup` (pbr, math, procedural2d, adjustment, compositing, ...). For Arnold it is the node's `classification` | pbr |

- **Fallbacks:**
  - A node without a role goes to "Other".
  - A group holding a single node collapses into its parent.
  - MaterialX defines one nodedef per signature (`ND_add_float`, `ND_add_color3`, ...). Its leaves group
    by function ("add"), and the type is picked in a submenu or by the first connection.
- **Hidden:** contexts `light`, `lightFilter`, `displayFilter`, `sampleFilter`, `integrator` and
  `displaydriver`, plus nodes that the renderer marks hidden (Arnold `hide`, `usd.hide`).
- **Tab search** matches label, function and identifier across every visible system. The current
  renderer's systems rank first.

### 3.3 Per renderer

| Renderer | Sdr plugin | Shading system | Identifier | Group source | Available in usdcc's process |
|---|---|---|---|---|---|
| USD preview | `UsdShadersDiscoveryPlugin` (built in) | `glslfx` | `UsdPreviewSurface`, `UsdUVTexture`, `UsdPrimvarReader_float` | `role` in `sdrMetadata` | yes |
| MaterialX | usdMtlx (built in, `--materialx`) | `mtlx` | `ND_standard_surface_surfaceshader` | `nodegroup` → role; context from the output type | yes |
| Arnold | arnold-usd `nodeRegistryArnold` | `arnold` | `arnold:standard_surface` | `classification` → role; `ui.groups` → pages | only once arnold-usd is built against our USD and the Arnold core DLL loads (its discovery calls `AiBegin`) |
| Cycles | **none** (Blender's Hydra delegate ships no Sdr plugin) | ours: `cycles` | `cycles:principled_bsdf` (the delegate also accepts `cycles_*`) | ours, from `ccl::NodeType` | needs generated definitions (§3.4) |
| V-Ray | **none** in the App SDK's USD bundle | ours: `vray` | `vray:BRDFVRayMtl`, to be confirmed against a V-Ray USD export | V-Ray `PluginCategory` (bsdf, material, texture, uvwgen) | needs generated definitions (§3.4) |
| MDL / RTX | Omniverse `rtx.usdmdl` (Kit only) | `mdl` | `.mdl` asset + subIdentifier | — | no. RTX is served through MaterialX/preview for now |

A **renderer profile** is a small table per shading system. Plugins can add profiles, so the V-Ray
and RTX plugins bring their own. Each profile holds:

- the display name and icon;
- the Material terminal prefix (`""`, `mtlx`, `arnold`, `cycles`, `vray`);
- the default surface node for "New Material" (`UsdPreviewSurface`, `ND_standard_surface_surfaceshader`,
  `arnold:standard_surface`, ...);
- extra hidden contexts.

### 3.4 Renderers without an Sdr plugin: generated definitions

OpenUSD has no JSON parser, so the least-work route reuses the built-in
`UsdShadeShaderDefParserPlugin`:

1. **Generate one `.usda` per renderer**, using `usdShaders/shaders/shaderDefs.usda` as the model:
   - typed `inputs:`/`outputs:` with defaults, `doc` and `connectability`;
   - `sdrMetadata` (label, page, role, help, options, shownIf, renderType);
   - an `info:<system>:sourceAsset` that resolves. The file itself will do, but a def without one is
     dropped.
2. **Register each node at startup** with `SdrRegistry::AddDiscoveryResult()`:
   - identifier `cycles:principled_bsdf`; subIdentifier = the prim name, since prim names can't hold
     ":";
   - discovery type `usda`, shading system `cycles`, uri = the file.

   This needs no DLL and no `plugInfo.json`, so a Python plugin can do it. A 40-line
   `SdrDiscoveryPlugin` modelled on usdShaders' is the cleaner alternative if registration ever has to
   happen before the app starts.

Where each generated file comes from:

- **Cycles:** `ccl::NodeType::types()` gives every node's sockets, with `ui_name`, type,
  `default_value`, `enum_values` and the `LINKABLE` flag. A small build-time tool that links Cycles
  writes `cycles_shaders.usda`, and the file ships with the build.
- **V-Ray:**
  - The App SDK enumerates `getPluginTypesOfCategory()` and `getPluginMeta()`, which give each
    parameter's type, default and description, plus UI guides: display name, enum strings, rollout,
    enable/hide conditions and min/max.
  - It has to run in the existing V-Ray helper process, because V-Ray's DLL names clash with usdcc's.
  - The helper writes `vray_shaders.usda`. It is cached in the user data folder per V-Ray version,
    regenerated when that version changes, and registered by the V-Ray plugin.
- **Arnold:** nothing to generate once arnold-usd is built for our USD. It ships as an optional plugin
  whose manifest puts its `plugInfo.json` on the path.

## 4. The panel

- **`MaterialEditorViewPanel`** is a view panel type, opened from Stages > Material Editor.
  - It follows the current stage and the selection: selecting a Material, NodeGraph or Shader (or a
    prim with a bound material) shows the enclosing Material.
  - A pin keeps the panel on one Material.
- **Selection is two-way:**
  - Selecting nodes selects their prims, so the Attribute Editor edits them.
  - The Attribute Editor already reads Sdr metadata for limits and connections
    (ATTRIBUTE_EDITOR.md §4.3).
- **Ports:**
  - A Shader node shows the **Sdr** inputs and outputs, not only the authored ones. They are grouped
    into collapsible pages by `GetPage()`.
  - Unconnectable inputs (`connectability = "interfaceOnly"`, or `IsConnectable()` false) get no
    port.
  - Connecting an unauthored input creates it (`UsdShadeShader::CreateInput` with the Sdr type).
  - `shownIf` hides ports whose condition is false. When `expansionState` is closed, only connected
    ports show.
- **Connection rules:** reuse `shade_connections.h`. It already allows matching value types and
  reaching any enclosing Material or NodeGraph. Ports are coloured by type role (color, float, vector,
  token, asset, surface/displacement).
- **Edits:** each of these is one `prim_actions` call and one undo step: create, delete, rename,
  duplicate, copy/paste, connect, disconnect, group into a NodeGraph, ungroup, promote an input to the
  interface, add/remove a Material terminal, create a backdrop, and auto-layout.
  - Creating a node authors `info:id` and the `UsdUINodeGraphNodeAPI` position, and nothing else.
  - Deleting a node also removes connections to it.
- **Live refresh:** a `UsdNotice::ObjectsChanged` listener scoped to the shown Material. It updates the
  model incrementally, so edits from Python, the Attribute Editor or undo show at once.
- **Python:**
  - `usdcc.shading.catalog()`, a pybind11 binding of the Qt-free catalog (§5, phase 1).
  - `register_renderer_profile()`, so plugins can add profiles.
  - Definitions are registered by calling `Sdr` directly.

## 5. Phases

| Phase | Deliverable | Exit criteria |
|---|---|---|
| **0. Spike** | QtNodes overlay port pinned to a commit with #533; a throwaway panel with a minimal `AbstractGraphModel` over one Material, inside an ADS dock | Nodes draw in the app theme; dragging writes `ui:nodegraph:node:pos` through an edit session and undoes in one step; no black viewport in a floating or tabbed dock. **Decide QtNodes vs OpenDCC here** |
| **1. Node catalog** | Qt-free `ShaderNodeCatalog` in `usdcc_usd`: Sdr query, version filter, the grouping of §3.2, renderer profiles, delegate filtering; Debug > Shader Nodes browser | Unit tests for the grouping of the preview and MaterialX nodes; counts match `GetAllShaderNodes()` minus the hidden ones |
| **2. Read-only graph** | `MaterialEditorViewPanel`, `UsdShadeGraphModel`, node painter, Sdr ports and pages, selection sync, NodeGraph enter/exit, breadcrumb | Shows the test_data materials, MaterialX and preview, matching `usdcat`; follows selection both ways |
| **3. Editing** | Create (palette and tab search), connect, disconnect, delete, rename, duplicate, copy/paste, Material terminals, live refresh | Every edit is one undo step; a Python edit shows at once; Storm renders a network built only in the editor |
| **4. Organisation** | Group into a NodeGraph and promote its interface, ungroup, backdrops, Graphviz auto-layout, shake to disconnect | Group/ungroup round-trips without changing the render; layout saved and reloaded |
| **5. Renderers** | Arnold plugin build; Cycles definition generator; V-Ray definition export in the helper; per-renderer "New Material" | Palettes for all five systems; an Arnold and a V-Ray material built in the editor render in their panels |

Phases 1 and 0 can run in parallel. Phase 5's generators only depend on phase 1.

## 6. Open questions

1. **V-Ray identifiers:** check `vray:<PluginClass>` against a USD file exported by V-Ray for Houdini
   or Maya before generating definitions.
2. **Cycles in the build:** is Blender's Cycles Hydra delegate (`3rd_party/cycles` in the USD build)
   meant to ship with usdcc? If not, Cycles is limited to the preview and MaterialX nodes it remaps,
   and phase 5 drops its generator.
3. **Arnold:** do we ship arnold-usd built against our USD, or find a user's install? Its Sdr plugin
   needs the Arnold core at discovery time, so startup gets slower when it is on the path.
4. **RTX/MDL:** is MaterialX plus preview enough for ovrtx? ovrtx's own material-editor example uses
   MaterialX.
5. **Unauthored defaults:** a port shows the Sdr default. Should "Reset" remove the authored value
   (the current Attribute Editor behaviour), or author the Sdr default explicitly for renderers that
   don't read Sdr?
