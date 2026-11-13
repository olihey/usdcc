# Material Editor (plan)

A node graph view panel for UsdShade networks: create shader nodes, connect them, group them into
NodeGraphs and lay them out. The stage is the only data store. The graph draws what is in the stage,
and every edit is a USD edit through `UndoManager`, so the Outliner, the Attribute Editor and undo
keep working without changes.

Status: **phases 0 to 4 done** (§5.1): the panel draws a material's network, follows the selection and edits
it. The parameters pane and the material preview beside the graph are done too (§7, results in §7.8). MDL
(phase 5) and the other renderers (phase 6) are still a plan. §2 and §3 were researched in November 2026 against the
local USD build (`USD_INSTALL`, `pxr.h` 0.26.8) and the upstream repos named in each section. §3.5 was
tested on 2026-11-06 against that build, and against the files in the ovrtx 0.6 wheel and the MDL SDK
2026.0.2 package.

**OpenDCC's node graph isn't used:** not linked, not vendored, and not a source to port from. Everything
QtNodes doesn't provide is written here.

## 1. Scope

- **Edits:** Material, NodeGraph and Shader prims; input/output connections; NodeGraph interface
  inputs and outputs; per-renderer Material terminals (`outputs:surface`, `outputs:mtlx:surface`,
  `outputs:arnold:surface`, ...).
- **Layout lives in USD:** `UsdUINodeGraphNodeAPI` (`pos`, `size`, `displayColor`,
  `expansionState`, `stackingOrder`, `icon`, `docURI`) and `UsdUIBackdrop` (`description`). Layout
  therefore survives a save and round-trips to other DCCs.
- **Shader systems:** USD preview (`glslfx`), MaterialX (`mtlx`), MDL (`mdl`, §3.5), Arnold (`arnold`),
  V-Ray and Cycles.
- **Parameters and preview:** the selection's shading inputs, and the material on a shape under an
  HDRI, beside the graph (§7).
- **Not in scope:** thumbnails or swatches for shaders, editing `.mtlx` documents, and OSL authoring.

## 2. The node graph framework

### 2.1 Candidates

| Library | License | Activity | Qt6 | Model/view | Fit |
|---|---|---|---|---|---|
| [QtNodes](https://github.com/paceholder/nodeeditor) (paceholder/nodeeditor) | BSD-3 | 3.7k★, tag 3.0.16 (Feb 2026), commits through Jul 2026 | yes (default) | **yes**: we subclass `AbstractGraphModel` | **Recommended** |
| [QuickQanava](https://github.com/cneben/QuickQanava) | BSD-3 | 1.4k★, release Sep 2024 | 6.5+, QML | no: owns its graph | Weak: QML, mirrors data |
| [NodeGraphQt](https://github.com/jchanvfx/NodeGraphQt) | MIT | 1.8k★, Feb 2026 | via Qt.py | no: owns its data | Pure Python. UX reference only |
| [noodles](https://github.com/facebookexperimental/noodles) | MIT | new (Apr 2026), experimental | GL, host supplies context | yes | Revisit in 2027 |
| imgui-node-editor, imnodes | MIT | active | ImGui | — | Not embeddable in a widgets app |

- No other credible Qt6 C++ node editor turned up. The other Qt repos found are stale (2016–2023),
  tiny or GPL.
- vcpkg has none of these, so QtNodes comes from an overlay port, `cmake/ports/qtnodes`, at a pinned
  commit. vcpkg finds it through `vcpkg-configuration.json`.
- **Other open-source editors, for how they feel rather than for code:**
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
    We don't use it, and `GraphicsView` installs Undo, Redo, Delete, Cut, Copy and Paste actions on it,
    which we remove from each view. Every model mutation goes through `UndoManager::apply()`. A node
    drag is an edit session: `beginEdit()` on the first position change, because a press sends no
    signal, then live `pos` sets, and `endEdit()` on release (`nodeClicked`).
  - **NodeGraphs:** QtNodes groups live only in the scene, so we don't use them. We model a NodeGraph
    as a subnet instead:
    - From outside it is one node, whose ports are its interface `inputs:`/`outputs:`.
    - Double-clicking it enters it.
    - A breadcrumb bar walks back up.
    - Inside, the interface shows as two edge nodes ("inputs" and "outputs").
    - Houdini and Katana work this way, and it matches UsdShade's encapsulation rules.
  - **Backdrops:** our own `QGraphicsItem` behind the nodes, backed by `UsdUIBackdrop`. Moving one
    moves the nodes on it.
  - **Tab search and shake to disconnect:** written here, on the scene's own events.
  - **Auto-layout:** our own layered layout in `usdcc_usd`, Qt-free and unit-tested: nodes in columns by
    their distance from the Material, ordered to reduce crossings. It's a few hundred lines, against
    adding Graphviz and its dependencies to the build for one command.
- **Known issues, as handled in the spike:**
  - QtNodes issue #529: a `QOpenGLWidget` viewport in ADS tabs turns black. We keep the default raster
    viewport, a plain `QWidget`.
  - Node moves no longer reach the model (#525/#526). Fix PR #533 is still open, so the port pins
    master at `7bbcd3e` and applies the PR's source change as a patch. Drop the patch when it merges.
- **What a graph model has to do, or nodes come out wrong** (none of it reports an error):
  - Store `NodeRole::Size` from `setNodeData`, or nodes collapse to 39×39.
  - Return `NodeRole::Style`, or nodes are invisible and can't be clicked.
  - Return that style as a `QVariantMap` (`toJson().toVariantMap()`). The painter reads it with
    `QJsonDocument::fromVariant()`, which drops a `QJsonObject`, and the node draws as a black box.
- **What the view does on its own:**
  - It fits the whole graph each time it's shown, ignoring the scale range, and `setupScale()` does
    nothing until a range is set. The panel fits again after a show and keeps the scale readable.
- **Fallback:** if phase 2 shows QtNodes fighting us, especially on custom geometry or on subnet-style
  navigation, write the view directly on `QGraphicsScene`. `shade_graph.h` and everything in §3 don't
  depend on which view draws them, so that replaces only the drawing and interaction code. Phase 2 needed
  neither a patch nor a workaround for its own geometry, painter or navigation, so this looks unlikely.

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
  - V-Ray and native RTX use separate render panels rather than Hydra delegates.
    Planned shading profiles supply these lists independently of their engine
    process model; current native RTX does not yet declare a material catalog.
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
| 2. Context | `GetContext()`. Usda-defined nodes say "usda" there and in their metadata, so their outputs decide: `surface`, then `displacement`, then `volume`, else pattern (UsdPreviewSurface has both of the first two) | Surface / Pattern / Displacement |
| 3. Group | The `role` metadata, because `GetRole()` falls back to the node's name. For MaterialX this is the nodedef's `nodegroup` (pbr, math, procedural2d, adjustment, compositing, ...). For Arnold it is the node's `classification` | pbr |

- **Fallbacks:**
  - A node without a role goes to "Other".
  - A group holding a single node collapses into its parent.
  - MaterialX defines one nodedef per signature (`ND_add_float`, `ND_add_color3`, ...). Its leaves group
    by function ("add"), and the type is picked in a submenu or by the first connection.
- **Hidden:** contexts `light`, `lightFilter`, `displayFilter`, `sampleFilter`, `integrator` and
  `displaydriver`, plus nodes that the renderer marks hidden (Arnold `hide`, `usd.hide`), and usdHydra's
  old `Hw*` glslfx nodes, some of which have no outputs.
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
| MDL | **none** that USD can use (§3.5) | ours: `mdl` | `mdl:<module>::<name>`, for the catalog only. Prims use `.mdl` asset + subIdentifier | the module's folder; `anno::in_group` → pages | needs our parser and generated definitions (§3.5) |

A **renderer profile** is a small table per shading system. Plugins can add profiles, so the V-Ray
and RTX plugins bring their own. MDL's is built in, as its parser is. Each profile holds:

- the display name and icon;
- the Material terminal prefix (`""`, `mtlx`, `mdl`, `arnold`, `cycles`, `vray`);
- the default surface node for "New Material" (`UsdPreviewSurface`, `ND_standard_surface_surfaceshader`,
  `OmniPBR`, `arnold:standard_surface`, ...);
- which terminals "New Material" connects (MDL's are `surface`, `displacement` and `volume`);
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

   *Tested* with shading system `mdl`: the node comes back valid, with its inputs, outputs, default, label
   and page. **The subIdentifier is required:** without it the parser looks for a prim named after the
   identifier and the node comes back invalid, with only a warning. From Python, that's the full
   twelve-argument `Sdr.NodeDiscoveryResult`, with a version made by `GetAsDefault()`.

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
- **MDL:** parsed from the `.mdl` files themselves, §3.5.

### 3.5 MDL

**How it looks in USD** (from `BuiltInMaterials.usda` in the ovrtx wheel, and
[NVIDIA's guide](https://developer.nvidia.com/usd/MDLschema)):

```usda
def Material "OmniPBR"
{
    color3f inputs:diffuse_tint = (0.78, 0.65, 0.49) (displayGroup = "Albedo"  displayName = "Color Tint")
    token outputs:mdl:surface.connect = </World/Looks/OmniPBR/Shader.outputs:out>
    token outputs:mdl:displacement.connect = </World/Looks/OmniPBR/Shader.outputs:out>
    token outputs:mdl:volume.connect = </World/Looks/OmniPBR/Shader.outputs:out>

    def Shader "Shader"
    {
        uniform token info:implementationSource = "sourceAsset"
        uniform asset info:mdl:sourceAsset = @OmniPBR.mdl@
        uniform token info:mdl:sourceAsset:subIdentifier = "OmniPBR"
        color3f inputs:diffuse_color_constant.connect = </World/Looks/OmniPBR.inputs:diffuse_tint>
        token outputs:out
    }
}
```

There's no `info:id`. A shader names a file and a definition in it, its output is always `token outputs:out`,
and one shader can feed all three terminals. In that sample every one of the 16 MDL shaders is like this.

#### The issues

**Sdr doesn't know MDL, so the ports have to come from elsewhere.** *Tested:* on a shader authored as above,
`GetShaderNodeForSourceType("mdl")` returns nothing, and `GetShaderNodeFromAsset()` too, as no parser handles
`.mdl`. ovrtx's own USD doesn't register one either: its `plugInfo.json` files list parsers for usdLux, usdMtlx and
usdShade only. Its `omni.usdMdl.dll` has no `plugInfo.json` of its own, and it isn't a library we could load
anyway.

*Since 2026-11-11 the USD build has Gatling*, whose plugin registers an MDL discovery and parser plugin
(`HdGatlingMdlDiscoveryPlugin`, `HdGatlingMdlParserPlugin`) and ships `OmniPBR.mdl` and the other Omni modules. With
it, `definitionOf()` finds the shader above through `GetShaderNodeForSourceType("mdl")`, with no code of ours.
Without it there is still none. `shader_node_catalog_test` checks whichever applies, by whether Sdr lists `mdl` as
a source type. Our own parser (below) is still what makes MDL work without Gatling.

**Registered definitions are found by id, and MDL shaders have none.** *Tested:* after registering a definition
(§3.4), a shader with `info:id` finds it, and one with the `.mdl` asset still doesn't. Authoring an `info:id` to
make that work isn't an option, as that's what a renderer reads instead. So the catalog needs a lookup of its own (below).

**The definition files might not be on the machine.** `OmniPBR.mdl` and the other
88 files were inspected in an ovrtx package (`bin/library/mdl`, about 1.4 GB).
Native RTX does not install Python packages. Definitions may exist in an external
native runtime or an explicitly bundled runtime; redistribution must follow the
supplied SDK terms. A stage can name a module absent from the machine.

**Storm can't draw it.** Storm's material contexts are the universal one and `mtlx` (§3.1), so an MDL-only material
falls back to Storm's default look in the viewport. *Not tested.* Only the RTX panel shows it properly. And each material
edit reloads the native RTX snapshot, with heavy scenes potentially taking minutes
([RTX_RENDER.md](RTX_RENDER.md#architecture-and-limits)).

#### Where the definitions come from: our own parser

| | Text parser in `usdcc_usd` (**recommended**) | MDL SDK in a helper |
|---|---|---|
| Size | Nothing added | 113 MB `libmdl_sdk.dll` on a 388 MB deploy, or 109 MB `libmdl_core.dll` |
| Build | A few hundred lines of C++ | The BSD package's prebuilt DLL plus a small C++ tool. vcpkg has a `mdl-sdk` port, but it builds LLVM. Its Python binding is for 3.10, and ours is 3.12 |
| Speed | Reads text, in-process | Starts a process, then compiles the module |
| Correct on | Signatures, annotations and enums, as far as tested below | Everything, including a module that doesn't compile, and `.mdle` |

**The parser is enough for what the editor needs,** which is ports, labels and groups rather than compiling anything.
MDL has no preprocessor, and a signature's grammar is small. *Tested:* a rough parser of about 60 lines read all 89 real files:
**1,276 of 1,318 signatures, and all 42 misses are `(*)`**, presets that inherit a base material's parameters, which
a real parser follows (`export material OmniHair_Wet(*) = OmniHair(...)`). That's 4,641 parameters across 202 materials
and 1,116 functions. It reads 47 parameters of `OmniPBR` and 156 of `OmniSurface`.

The parser's limits: it can't tell a module is broken, which the renderer reports; it won't evaluate a default that's a real
expression, which then shows as no default; and it doesn't read `.mdle`. If those start to matter, the helper can replace it
behind the same interface, as nothing else sees where definitions come from.

**What it reads, and where it goes in the generated definition:**

| In the `.mdl` | Becomes | How common (of 4,641) |
|---|---|---|
| `anno::display_name` | `label` | 1,807 |
| `anno::in_group` | `page` (nested with ":") | 1,497 |
| `anno::description` | `help` | 1,377 |
| `anno::hard_range`, `soft_range` | limits | 429, 237 |
| `anno::ui_order` | port order | 238 |
| `anno::hidden`, `anno::unused` | no port | 123, 196 |
| `anno::enable_if` | ignored at first: it's an MDL expression, not an `SdfBooleanExpression` | 143 |
| `export material` / other return type | context `surface` / `pattern`, with the return type for `outputs:out` | |

**MDL types to USD types.** Only the first group is confirmed against an authored file. The rest are what I expect, and need checking
against a real MDL network before relying on them (§6, question 4):

| MDL | USD | Count | State |
|---|---|---|---|
| `float`, `bool`, `int` | `float`, `bool`, `int` | 1,350, 333, 232 | Seen in BuiltInMaterials |
| `color` | `color3f` | 447 | Seen |
| `material`, and a shader's output | `token` | 106 | Output seen; input expected |
| `float2`, `float3`, `float4`, `int2` | `float2`, `float3`, `float4`, `int2` | 377, 452, 321, 15 | Expected |
| `texture_2d` | `asset` | 250 | Expected |
| `string` | `string` | 105 | Expected |
| `float3x3`, `float4x4` | `matrix3d`, `matrix4d` | 20, 20 | Expected |
| an enum | `int`, with its names as options | about 200 | Expected |
| a struct (`texture_return`, 90) | `token`: no value, connection only | | Unknown |

Enums and structs can be declared in another module (`::OmniSurface::OmniImage::alpha_mode`), so the parser follows `import`s.

#### Into the catalog

1. **Searching:** a new setting, `Materials/mdlSearchPaths` (the `FolderList` type, on a Materials page), plus `MDL_USER_PATH` and
   `MDL_SYSTEM_PATH`, plus a configured native runtime's `bin/library/mdl` (external
   root or plugin runtime folder; no Python import). This is planned MDL catalog
   discovery, not implemented in the current phase. The folders' `.mdl` files are listed at startup without being read,
   and each is parsed the first time it's needed, so startup stays quick.
2. **Registering:** each parsed definition becomes a generated `.usda` in the user data folder, keyed by the file's path and time, and is registered as in §3.4 with
   identifier `mdl:<module>::<name>` and shading system `mdl`. So palette, tab search and Attribute Editor treat it as any other Sdr node.
3. **Finding a prim's definition:** `ShaderNodeCatalog::definitionOf(UsdShadeShader)` is the one way everything asks. It uses `info:id` if there is one, or else reads
   `info:mdl:sourceAsset` and its subIdentifier, parses the module if it hasn't, and returns the registered node. A bare `@OmniPBR.mdl@` is looked up in the search paths, as
   USD's resolver doesn't know them.
4. **A module that can't be found** still draws: the node shows its authored inputs and `outputs:out`, with a warning naming the module. Nothing is lost or rewritten.

A parser plugin (`SdrParserPlugin` for discovery type `mdl`) would make `GetShaderNodeForSourceType()` work for all USD code, Python included. It's cleaner, but needs a DLL with a `plugInfo.json` and I haven't
tested that USD would call it for an asset. `definitionOf()` doesn't stop that being added later.

#### In the editor

- **Palette:** shading system MDL → context (Materials, Functions) → the module's folder. Presets are listed under their base material. `anno::hidden` definitions are left out.
- **Creating:** an MDL node authors `info:implementationSource`, `info:mdl:sourceAsset` with the bare module name when a search path finds it (relative otherwise), its subIdentifier, and `token outputs:out`. **It authors no `info:id`.**
  "New Material" connects it to all three terminals, as Omniverse does.
- **Large nodes:** `OmniSurface` has 156 inputs, so a node's pages start closed, and only connected ports show (`expansionState`).
- **Connecting:** `shade_connections.h` compares value types, which works for matching USD types, but several struct types may all be `token`. So when both ends have definitions, MDL's own types decide.
- **The interface:** promoting an input to the Material copies `displayGroup` and `displayName` from the definition, which is how the sample's interface is authored.
- **Beside other terminals:** a Material can have `outputs:mdl:surface` and `outputs:surface` together, as the sample's OpenPBR one does with `mtlx`. Adding a preview shader this way is what makes MDL materials readable in Storm, and it's an explicit action, not automatic.

## 4. The panel

- **`MaterialEditorViewPanel`** is a view panel type, opened from Stages > (a stage) > Views > Material Editor.
  - It follows the selection: a Material, a NodeGraph or Shader (the level it's drawn on), or a prim with
    a bound material. A selection that's already on this level, or the level itself or one around it,
    doesn't move it, and neither does a prim with no material.
  - A pin keeps the panel where it is. Double-clicking a NodeGraph and the breadcrumb still work.
- **What's read and what's drawn are apart:** `shade_graph.h`, Qt-free, reads a level; `UsdShadeGraphModel`
  gives it to QtNodes; `ShaderNodeGeometry` and `ShaderNodePainter` lay it out and draw it.
- **Positions:** `ui:nodegraph:node:pos` counts in node sizes, not pixels ("assume that the size of a node
  is approximately 1.0x1.0", `nodeGraphNodeAPI.h`), so it's stored as pixels / 200. A node with no position
  goes in a column by its distance from the end of the graph, and stays there for as long as the panel
  shows that stage. A Material's Outputs node uses the Material's position. A level's other interface
  nodes have nowhere in USD to keep one, so theirs last as long as the panel does.
- **Selection is two-way:**
  - Selecting nodes selects their prims, so the Attribute Editor edits them.
  - **The Attribute Editor needs work for this, despite what its doc says.** ATTRIBUTE_EDITOR.md §4 says it
    reads Sdr metadata for shader inputs, but a search of `src/` finds no Sdr call. An unauthored input of an
    MDL or MaterialX node isn't an attribute yet, so it also needs to list the definition's inputs (§5, phase 3).
- **Ports:**
  - A Shader node shows the **Sdr** inputs and outputs, not only the authored ones. They are grouped
    into collapsible pages by `GetPage()`.
  - Unconnectable inputs (`connectability = "interfaceOnly"`, or `IsConnectable()` false) get no
    port.
  - Connecting an unauthored input creates it (`UsdShadeShader::CreateInput` with the Sdr type).
  - `shownIf` hides ports whose condition is false, unless authored or connected. `GetPagesShownIf()`,
    for whole pages, isn't read yet. When `expansionState` is closed or minimized, only connected ports
    show.
  - A page starts open if it holds something authored or connected. Opening or closing one lasts as long
    as the panel, and a closed page still lists what's connected, so a line always has somewhere to land.
  - A connection from outside the level, or from something missing, has no line. Its port names the
    source instead.
- **Connection rules:** reuse `shade_connections.h`. It already allows matching value types and
  reaching any enclosing Material or NodeGraph. Ports are coloured by type role (color, float, vector,
  token, asset, surface/displacement). Lines are all one grey so far: QtNodes' own per-type line colours
  are a hash of the type's name, not these.
- **Edits:** each of these is one call in `shade_actions.h` (`prim_actions` for rename and disconnect) and
  one undo step: create, delete, rename, duplicate, copy/paste, connect, disconnect, group into a NodeGraph,
  ungroup, promote an input to the interface, add an input, add/remove a Material terminal, create a
  backdrop, and auto-layout.
  - Creating a node authors `info:id` (MDL: its source asset instead, §3.5) and the
    `UsdUINodeGraphNodeAPI` position, and nothing else.
  - Deleting a node also removes connections to it. A prim a weaker layer defines is deactivated instead.
  - Connecting checks the types with `typesMatch()`, and refuses a loop and a level's input straight to its
    own output. Pulling a line off and dropping it elsewhere is one step, ending on the mouse's release.
  - Duplicate, paste, group and ungroup write a copy of the prim as composed into the edit target, rather
    than moving specs, so they work on a prim from a reference. The old one is removed or deactivated.
  - Paste drops a connection to anything that isn't in the Material it lands in.
  - Ungrouping sends an interface input's value to whatever read it, and follows connections through.
  - **Add Input...** (right-click the Material's node, the Inputs node or a NodeGraph, or the + on the
    Attribute Editor's Inputs header for a Material, NodeGraph or Shader) asks for a name, a type of
    `canonicalValueTypes()` (Array for its array type), a default value in the Attribute Editor's own
    editor for that type, allowed tokens (for a token), connectability, display name and group, and
    documentation (`AddShadeInputDialog`). It refuses a name that's taken, and on a Shader one its
    definition has. `shade_actions::addInput()` authors only what was given.
- **Keys, over the graph:** Tab searches nodes, Delete or Backspace deletes, F2 renames, Ctrl+D duplicates,
  Ctrl+C, X and V copy, cut and paste, Ctrl+G groups, Ctrl+Shift+G ungroups, L lays out, F frames. They're
  caught before QtNodes' own, whose Delete would edit its own copy. Right-click offers the same, plus
  what's for the port under the mouse.
- **Palette:** the Nodes button shows the tree beside the graph. Double-click adds a node, or drag one in.
  What the graph uses comes first.
- **Terminals:** a Material's node offers its shaders' profile terminals before they're authored, and
  `surface` when it's empty, so there's somewhere to drop a line.
- **Backdrops:** what's on one when it's pressed moves with it, as one step. Its corner resizes it, and
  double-clicking describes it.
- **Shaking** a dragged node takes its lines off, as one step with the move: three turns of at least 24
  pixels within 600 ms.
- **Attribute Editor:** a Shader's inputs are in its definition's pages, authored or not (§7.1).
- **Live refresh:** `Stage::changed`, for anything under the shown Material. A position only moves its
  node. Otherwise the level is read again on the next event loop pass, and the scene is replaced only if
  what's drawn differs, so a new value leaves it alone. During a mouse gesture a read waits for the
  release, as it would replace the item being dragged. Edits from Python, the Attribute Editor or undo
  show at once.
- **Python:**
  - `usdcc.shading.catalog()`, a pybind11 binding of the Qt-free catalog (§5, phase 1).
  - `register_renderer_profile()`, so plugins can add profiles.
  - Definitions are registered by calling `Sdr` directly.

## 5. Phases

| Phase | Deliverable | Exit criteria |
|---|---|---|
| **0. Spike** | QtNodes overlay port pinned to a commit with #533; a throwaway panel with a minimal `AbstractGraphModel` over one Material, inside an ADS dock | Nodes draw in the app theme; dragging writes `ui:nodegraph:node:pos` through an edit session and undoes in one step; no black viewport in a floating or tabbed dock. **Decide QtNodes vs our own `QGraphicsScene` view here** |
| **1. Node catalog** | Qt-free `ShaderNodeCatalog` in `usdcc_usd`: Sdr query, version filter, the grouping of §3.2, renderer profiles, delegate filtering, `definitionOf()`; Debug > Shader Nodes browser | Unit tests for the grouping of the preview and MaterialX nodes; counts match `GetAllShaderNodes()` minus the hidden ones |
| **2. Read-only graph** | `MaterialEditorViewPanel`, `UsdShadeGraphModel`, node painter, Sdr ports and pages, selection sync, NodeGraph enter/exit, breadcrumb | Shows the test_data materials, MaterialX and preview, matching `usdcat`; follows selection both ways |
| **3. Editing** | Create (palette and tab search), connect, disconnect, delete, rename, duplicate, copy/paste, Material terminals, live refresh; the Attribute Editor listing a definition's unauthored inputs | Every edit is one undo step; a Python edit shows at once; Storm renders a network built only in the editor |
| **4. Organisation** | Group into a NodeGraph and promote its interface, ungroup, backdrops, our layered auto-layout, shake to disconnect | Group/ungroup round-trips without changing the render; layout saved and reloaded |
| **5. MDL** | The parser, including `(*)` presets and enums across modules; generated definitions and their cache; `Materials/mdlSearchPaths`; MDL in `definitionOf()`, palette and "New Material"; MDL's own connection types | Parses all 89 files in the ovrtx wheel with nothing missed (a local check, as they aren't ours to commit; tests use small `.mdl` files written for them). BuiltInMaterials.usda and `omniverse_preview.usda` draw with their real labels and pages. **An OmniPBR material built in the editor renders in the RTX panel**, and a missing module draws with a warning |
| **6. Other renderers** | Arnold plugin build; Cycles definition generator; V-Ray definition export in the helper; per-renderer "New Material" | Palettes for all six systems; an Arnold and a V-Ray material built in the editor render in their panels |

Phases 1 and 0 can run in parallel. Phases 5 and 6 only depend on phase 1 for their definitions, and on phase 3 to author anything, and either can go first.

### 5.1 Results of phases 0 and 1

**Decision: QtNodes stays.** All three exit criteria pass with an unmodified library plus the #533 patch.
Everything that went wrong was a model requirement or a view default (§2.2), each fixed in a few lines
of our code. The spike only drew and moved nodes. Custom geometry and subnet navigation first get tried
in phase 2, so the fallback in §2.2 still stands.

| Criterion | Result | Checked by |
|---|---|---|
| Nodes draw in the app theme | Yes, after the fixes in §2.2. The first run drew black boxes | Screenshots from the deployed app; `usdcc_ui_tests` counts a node's body pixels |
| A drag writes `pos` through an edit session, one undo step | Yes. Only the dragged prim is authored, the step is "Move Node", Edit > Undo removes the API and Redo restores it | Script-sent mouse events in the deployed app, and `usdcc_ui_tests` |
| No black viewport | None in a floating window, or when its tab comes back. Beside the 3D viewport in one tab area it was tested only with a plain widget, offscreen | Floating: deployed app. Tabs: `usdcc_ui_tests` |
| Catalog grouping and counts | 814 nodes plus 23 hidden; systems `glslfx` and `mtlx` | `usdcc_tests`, deployed app |

**Phase 2.** The spike is gone, replaced by the panel in §4. `test_data/materials.usda` has a preview material
with a NodeGraph and interfaces, and a MaterialX one.

| Criterion | Result | Checked by |
|---|---|---|
| Shows the materials, matching `usdcat` | Yes. `usdcat --flatten` lists 10 connections, and all 10 are drawn across the file's levels | `usdcc_tests` counts them in a flattened export; real `usdcat` gave the same number by hand; screenshots from the deployed app |
| Follows selection both ways | Yes: a shader elsewhere, a bound prim, a shader in a NodeGraph, and a pin. Clicking a node selects its prim | `usdcc_ui_tests` with mouse events, and the deployed app with `usdcc.usd.select()` and script-sent clicks |
| NodeGraph enter and exit | Double-click and breadcrumb | Double-click: `usdcc_ui_tests`. Breadcrumb: both |

Not checked: the position scale against files from other DCCs, as none are on this machine (§6, 8).

**Phases 3 and 4.**

| Criterion | Result | Checked by |
|---|---|---|
| Every edit is one undo step | Yes, for all of those in §4 | `usdcc_tests` and `usdcc_ui_tests`, the panel's with mouse and key events |
| A Python edit shows at once | Yes: a script's colour reached the render, and connections, prims and removal reach the graph | Deployed app; `usdcc_ui_tests` |
| Storm renders a network built only in the editor | Yes. On an empty Material: Tab, typed `UsdPreviewSurface`, Return, L, a line dragged to the terminal. The only thing not from the editor was the colour, set by script | Deployed app, with script-sent keys and mouse. Sphere read (255, 65, 65) |
| Group and ungroup don't change the render | Yes: no channel differs by anything, on every third pixel of the viewport, after grouping and after ungrouping | Deployed app, from screen captures. Also `usdcc_tests` for a network with connections on both sides |
| Layout saved and reloaded | A second panel reads the same positions from the stage, and the attributes are stored | `usdcc_ui_tests`. Saving to a file and reopening wasn't run |

Only run in the offscreen tests or not at all: right-click menus (their contents are tested, not opening them),
dragging from the palette (not run), a real person's hand on any of it.

## 6. Open questions

1. **V-Ray identifiers:** check `vray:<PluginClass>` against a USD file exported by V-Ray for Houdini
   or Maya before generating definitions.
2. **Cycles in the build:** is Blender's Cycles Hydra delegate (`3rd_party/cycles` in the USD build)
   meant to ship with usdcc? If not, Cycles is limited to the preview and MaterialX nodes it remaps,
   and phase 5 drops its generator.
3. **Arnold:** do we ship arnold-usd built against our USD, or find a user's install? Its Sdr plugin
   needs the Arnold core at discovery time, so startup gets slower when it is on the path.
4. **MDL types:** the types marked "Expected" in §3.5, above all structs and enums, need a real MDL function network. BuiltInMaterials has only materials with simple inputs, and I couldn't
   find `omniverse_preview.usda`, with its 1,550 shaders, in the original research pass. It has since reproduced a ten-minute first-frame failure; see [RTX_RENDER.md](RTX_RENDER.md#architecture-and-limits). That's the first thing to investigate in phase 5.
5. **Unauthored defaults:** a port shows the Sdr default. Should "Reset" remove the authored value
   (the current Attribute Editor behaviour), or author the Sdr default explicitly for renderers that
   don't read Sdr?
6. **ovrtx and our search paths:** the planned `Materials/mdlSearchPaths` must reach the native renderer. Propagation is not implemented or validated; there is no Python RTX helper.
7. **The Omniverse MDL files' licence:** the native runtime includes NVIDIA's SDK terms; redistribution review remains separate from bundling support. Catalog discovery should read configured runtime/search roots. Without those files, there's no `OmniPBR` to offer, so "New Material" for MDL needs a module from a search path or says why it can't.
8. **Positions in other DCCs' files:** USD's docs say a node is about 1.0×1.0, and we store pixels / 200. Files
   that store pixels anyway would open with their nodes spread 200 times too far. Check a real export, and
   consider guessing the scale per file from how far apart its nodes are.

## 7. Parameters and preview

A pane on the right of the graph, in two parts: a live preview of the material on a shape, and the
shading parameters of what's selected. Both can be shown or hidden, like the Nodes palette on the left.
Not planned: thumbnails or swatches, on nodes or in the palette.

```
┌ Material Editor ───────────────────────────────┬────────────────┐
│ [Nodes] breadcrumb …         [Preview] [Params]│  ┌──────────┐  │
│                                                │  │  shape   │  │  HydraViewportWindow,
│              node graph                        │  └──────────┘  │  preview mode
│                                                │ ⏸ Shape▾ HDRI▾ │
│                                                ├────────────────┤
│                                                │ PropertySheet  │  Shading mode
│                                                │  (Sdr pages)   │
└────────────────────────────────────────────────┴────────────────┘
```

A vertical splitter separates the two parts. Its sizes, and which parts are shown, persist per panel.

### 7.1 Parameters

Parameters go in the pane, not on the nodes: widgets in a `QGraphicsProxyWidget` are slow, and a node
like `OmniSurface` has 156 inputs.

- **`PropertySheet`:** the rows of `AttributesViewPanel` become a widget of their own: row building,
  `AttributeBinding`, live refresh, search, Tab order. It takes a stage, the `UndoManager`, a prim path
  and a mode:
  - **Full:** what the Attribute Editor shows today (prim header, composition, transform stack, all
    categories). The Attribute Editor becomes a host for it.
  - **Shading:** only `inputs:`, in the Sdr pages.
- **Sdr metadata for authored inputs** (the gap in §4): label, page, help, options as a combo box, ranges,
  `shownIf`. Both editors get it. "Available Inputs" becomes part of the pages: an unauthored input shows
  its definition's default, dimmed, and editing it authors it.
- **What the pane shows:**
  - A selected node: its inputs.
  - Nothing selected, or the Material or NodeGraph itself: the level's interface inputs, the material's
    public parameters.
- **Edits** go to the main stage, through its `UndoManager` and edit target, never to the preview stage.

### 7.2 The preview stage

**Chosen: a masked stage over the scene's own layers.** It shares the root layer, and its own session
layer sublayers the scene's session layer and the preview scene:

```
previewSession (anonymous, the preview's own)
  subLayers = [ sourceStage session layer, preview scene layer ]
previewStage = UsdStage::OpenMasked(sourceStage root layer, previewSession,
                                     mask = { <material path>, /__usdccPreview })
previewStage->ExpandPopulationMask(...)   // follows connections out of the material
```

- **The material composes as in the scene:** the same layers, the scene's session layer (usdcc edits
  there, `session_stages.cpp`), variant selections, ancestral arcs, edits inside variants. Asset paths
  resolve the same way.
- **Live with no sync code:** the layers are the same `SdfLayer` objects, so an edit from the graph, the
  Attribute Editor, Python or undo reaches the preview through USD's change notices.
- **No path remapping:** the shape binds to the material at its own path.
- **The scene's lights and geometry are masked out.** Changing the material only changes the mask.
- **Kept in step by hand,** as they're per stage: muted layers, load rules (a material inside a payload)
  and `upAxis`/`metersPerUnit` (§7.3).
- **The preview session layer is strongest, and only the preview sees it.** It holds the shape's binding
  and the selected-node preview (§7.6). Nothing written there reaches the scene.
- **Not registered with `StageManager`,** like `loadPreviewStage()`. It is dropped when the panel's stage
  closes or changes, so it doesn't keep the scene's layers alive.
- **To test:** a material inside an instance prototype, selected through an instance proxy. If masking
  can't reach it, the preview says so.

Rejected:

| Option | Why not |
|---|---|
| Copy the composed material into a new stage | Has to be copied again on every edit, and asset paths need re-anchoring |
| Reference the material from the root layer | Misses the session layer, which usdcc edits |
| The shape as a layer on the scene's stage | Shows in the Outliner, bounds and viewport; the scene's lights reach it and its lights reach the scene; it could be saved |

`ShadingPreviewStage` builds and updates this, Qt-free and unit-tested in `usdcc_usd`.

### 7.3 Shapes

- **Folders:** `resources/preview/shapes/` ships a shader ball, sphere, cube, plane and a cloth over a
  ball. A new setting, `Materials/previewShapePaths` (`FolderList`), adds the user's folders. Each
  `.usd`, `.usda`, `.usdc` or `.usdz` is one entry, named after the file. Folders are listed when the
  menu opens.
- **Only the top of each folder is listed,** not its subfolders, so a shape can keep its textures and
  sublayers in a subfolder beside it without them showing in the menu. HDRI folders (§7.4) work the
  same way.
- **A name is taken once:** the first folder that has it wins, ignoring case, and the user's folders
  come before the shipped one. So a user's `Shader Ball.usda` replaces the shipped shader ball.
- **What a shape file holds:** its geometry under the `defaultPrim`, with UVs (`primvars:st`), Y-up, in
  metres.
- **What gets the material:** the prims in a `collection:preview` on the default prim, or every gprim when
  there's none. So the cloth can lie on a neutral ball, and a shader ball can keep its grey stand.
  The preview session layer binds the material to that collection (a collection binding).
- **Placing it:** referenced at `/__usdccPreview/Shape/Asset`, scaled by the shape's `metersPerUnit`
  over the scene's, so textures keep their real-world size. The camera frames its bounds. The preview is
  always Y-up, as the viewport's orbit camera is, so it doesn't turn for the scene's `upAxis`: only a shape
  file authored Z-up is turned.
- **A camera in the shape file:** the first perspective `Camera` under the default prim is looked through
  instead of framing the bounds: its position, direction and vertical field of view
  (`ShadingPreviewStage::shapeCamera()`). The orbit turns about the point on its line of sight nearest the
  middle of `collection:preview` (else the whole shape), and keeps its lens. The orbit camera has no roll,
  so a rolled camera is looked through level. Reset View in the ... menu goes back to it.
  - Everything else in the file renders too, its lights included, so a shape with a camera is usually a
    whole set, such as the Standard USD Shader Ball's box and lights.
- **The binding** is `strongerThanDescendants`, so a shape's own placeholder binding doesn't hide the
  material; what a shape keeps for itself it leaves out of `collection:preview`.
- **A file that won't load** gives a warning in the preview and keeps the last good shape.

### 7.4 HDRIs

- **Folders:** `resources/preview/hdri/` ships Studio, Sky and Neutral, 512×256 `.hdr` maps that
  `tools/gen_preview_assets.py` draws (soft boxes, a sky with a sun, an even grey), so they carry no
  licence of their own and cost 1.5 MB. A Poly Haven map, or any `.hdr` or `.exr`, can go in a folder of
  `Materials/previewHdriPaths` (`FolderList`). The script makes the shapes too.
- **Lighting:** a `DomeLight` at `/__usdccPreview/Env` with the chosen file, its rotation and its exposure.
  With no HDRI ("None"), a grey dome and a `DistantLight` key from above and in front.
- **Background:** the HDRI, or the panel's colour: Show as Background, the `domeLightCameraVisibility`
  render setting.
- **The shape's own lights** (any prim with `UsdLuxLightAPI` in the shape file,
  `ShadingPreviewStage::shapeHasLights()`):
  - Under an HDRI image they're deactivated (`active = false` in the preview's own layer), so the HDRI
    lights the shape alone. A mesh light stays, as it's geometry too.
  - With no HDRI (the grey dome and key light), they light it along with those.
  - The Light menu offers **Use scene lights**, which leaves only them: the dome and the key light are
    deactivated (`Environment::sceneLights`), and the HDRI's options in the menu are disabled.
  - It's on whenever a shape file with lights is chosen, so a lit set shows as it was lit. Unchecked,
    it stays off for that shape, also when the panel shows another stage; a session or a duplicate
    keeps what it had. For a shape without lights it does nothing.

The preview's toolbar holds Pause (§7.5) and a ... button. Its menu holds what's shown (Material or
Selected Node, §7.6), the Shape and Light submenus, and Reset View when the shape has a camera. A
right-click on the picture opens the same menu there (`HydraViewportWindow::contextMenuRequested()`,
as the native window gets no context menu event its container would see). The choice persists per panel (a session, a duplicate), and `Materials/previewShape` and
`Materials/previewHdri` name the ones a new panel starts with.

### 7.5 The view

- **`HydraViewportWindow` in a preview mode:** no tools or gizmos, no grid, a turntable orbit, framing
  on the shape, the preview's own camera and lights instead of the default lighting. It is already a
  `QWindow` in a container, so QtNodes' black-viewport issue (§2.2) doesn't apply.
- **Square:** the picture is as tall as the pane on the right is wide, so widening the pane grows it, and
  the Parameters take what's left below. The handle between them doesn't move. In a pane too short for
  that, the Parameters keep 120 px and the picture is wider than tall. With the Parameters hidden, the
  picture stays square at the top. `MaterialEditorViewPanel::fitPreview()` sizes it, from
  `ShadingPreviewWidget::heightForSquare()` (the toolbar and any warning line on top of the width).
- **Renderer:** the one of the viewport showing the same stage, the last focused if there are several. It
  follows that viewport's `rendererChanged()`. With no viewport open on the stage, Storm. A material
  the renderer can't draw (MDL in Storm, §3.5) gets a line saying so, rather than a default-grey shape.
- **Pause:** an icon button in the preview's toolbar, showing what a click does: pause bars while it
  renders, a play triangle while it's paused.
  - Paused, the last frame stays, fitted to the window. Nothing renders, and a progressive
    renderer's own threads pause too (`PauseRenderer()`, or `StopRenderer()` for one that can't
    pause), so it stops refining. A badge over the picture isn't possible: the
    picture is a native window, which no widget can draw over.
  - Edits still reach the masked stage, which is cheap. Hydra takes them on the next render.
  - Resuming renders at once.
  - It also draws nothing and pauses the renderer, without the button changing, while the preview
    is hidden, the panel's tab isn't showing or the main window is minimized
    (`HydraViewportWindow::isOnScreen()`), as every viewport does. Back on screen, it renders at once.
- **Without OpenGL** (Qt's offscreen platform, which the tests run on), the preview isn't embedded and
  says so. A native child window there gains nothing, and Qt's offscreen backing store crashes when one
  is moved into a floating dock.
  - The button's state persists per panel.

### 7.6 Previewing a selected node

A switch in the preview's ... menu, Material or Selected Node, shows one node's output on the shape:

- In the preview session layer, an `over` on the material adds a hidden shader and points a terminal at
  it: `outputs:surface` for a glslfx node, `outputs:mtlx:surface` for a MaterialX one. The material's other
  surface terminals are blocked (an empty connection list), so a renderer that prefers another context
  falls back to this one.
  - MaterialX: a colour drives `ND_surface_unlit`'s emission colour. Anything else that has one goes
    through `ND_convert_*` nodes first: a float shows as grey, vectors as colours. A BSDF or EDF goes
    into `ND_surface`; a surface straight to the terminal.
  - glslfx: a colour or vector drives the emission of a black `UsdPreviewSurface`. USD's preview nodes
    have nothing to convert with, so a float or a float2 output says it can't be shown.
  - Which output: `rgb` if the node has one, else its first colour, vector, then surface output. One
    its definition has but nobody authored is made in the preview's session layer.
- The scene never sees it. Switching back, or selecting something else, clears the `over`.

### 7.7 Phases

| Phase | Deliverable | Exit criteria |
|---|---|---|
| **P1. Property sheet** | `PropertySheet` out of `AttributesViewPanel`; Sdr labels, pages, options, ranges and `shownIf` for shader inputs; unauthored inputs in the pages; the interface view | The Attribute Editor behaves as before, with tests; a UsdPreviewSurface and a MaterialX surface show their Sdr pages and labels |
| **P2. Parameters pane** | The pane in the Material Editor, Shading mode, following the graph's selection and the pin | An edit there is one undo step and shows in the graph and the viewport |
| **P3. Preview** | `ShadingPreviewStage` (mask, session layer, mutes, load rules, up axis and units, binding, dome); the preview mode of `HydraViewportWindow`; following the viewport's renderer; pause | An edit from anywhere, including the scene's session layer, shows in the preview; nothing appears in the scene's stage or its saved layers; pause stops rendering and resume picks the edits up |
| **P4. Shapes and HDRIs** | The two folders, their settings and menus; the shipped shapes and maps | A user's folder shows up without a restart; `collection:preview` decides what gets the material; a broken file warns and keeps the last |
| **P5. Selected node** | §7.6 | A texture's and a float's output show on the shape in Storm, for glslfx and MaterialX, and the scene's layers stay unchanged |

P1 is useful on its own, in the Attribute Editor. P3 doesn't need P1 or P2.

### 7.8 Results

All five phases are done.

| Criterion | Result | Checked by |
|---|---|---|
| The Attribute Editor behaves as before | Yes: its 134 UI tests pass with it hosting a `PropertySheet`. The sheet now holds the focus while its rows are rebuilt, as the search field is outside it | `usdcc_ui_tests` |
| Sdr pages and labels | UsdPreviewSurface's inputs, labelled, unauthored ones dimmed; standard_surface's pages, with sliders from `uimin`/`uimax` and combos from string options | `usdcc_tests` (`shaderInputs()`), `usdcc_ui_tests`, the app |
| An edit in the pane is one undo step | Yes, also one that creates an input | `usdcc_ui_tests` |
| Edits from anywhere reach the preview, nothing reaches the scene | A root layer and a session layer edit show at once; the scene's layers export the same before and after a node preview | `usdcc_tests` (`shading_preview_test.cpp`) |
| Shapes and `collection:preview` | A collection keeps a stand's own material; without one everything shows it; a file with no `defaultPrim` is refused and the last shape stays; a metre is 100 units in a centimetre scene | `usdcc_tests`, the app |
| Pause | The last frame stays; a session and a duplicate keep it | `usdcc_ui_tests`, the app |
| Node preview in Storm | A texture's `rgb` (glslfx) and a multiply's colour (MaterialX) show on the cloth | the app, with Storm |

Not done or not right yet:

1. **The HDRI isn't drawn behind the shape** in Storm, although the setting is on and it lights the shape.
2. **`ND_surface_unlit` shows some shading** in Storm, so a node's colour isn't shown flat.
3. **Instance prototypes:** whether a mask reaches a material selected through an instance proxy isn't
   tested (§7.2).
4. **Two engines:** GPU memory with the viewport and the preview loading the same textures. Pause is the
   way out for now.
5. **Following the renderer** was checked with Storm only: the app has no other delegate built here.
