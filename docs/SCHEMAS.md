# Schemas

Status: **phases 1 to 4 are built** (§12): the Schemas panel, its data, the stage in it, and generating
a codeless library, which has no UI yet (phase 5). The rest is a plan; §9 has the decisions still
open, §10 the phases.

Three things, in the order to build them:

1. A **Schema Explorer**: a panel showing every schema USD knows, what it defines and where it comes
   from, with the stage's own use of schemas beside it.
2. A **Schema Editor** for *codeless* schemas: write a schema in a form, generate its plugin with no
   compiler, and have it in usdcc at the next start.
3. **Promote to Schema:** turn a prim's custom attributes into a schema.

Everything marked *checked* below was run against this USD build (pxr 0.26.8, the pin in
`deps/usd.json`), in a fresh Python per case. §11 lists what wasn't.

## 1. What USD offers

| Way to define data | Needs | In the file itself? |
|---|---|---|
| **C++ schema** | `schema.usda`, `usdGenSchema`, a compiler, a plugin with a library | No: every reader needs the plugin |
| **Codeless schema** | `schema.usda` with `skipCodeGeneration = true`, `usdGenSchema`. No compiler | No: every reader needs the plugin |
| **Class prim** that others inherit | Nothing but USD | Yes |
| **`custom` attributes** | Nothing | Yes |

- **"Schemaless" is not a USD term.** What needs no C++ is a *codeless* schema. It is still a plugin,
  two text files: `plugInfo.json` (the types) and `generatedSchema.usda` (their properties).
- **A schema never travels with a file.** A file names its types and applied schemas; what they mean
  comes from the plugins of whoever opens it.
- **A codeless schema has no classes,** in C++ or in Python: no `StudioAsset(prim)`, no
  `GetStatusAttr()`. It is used by name (`prim.ApplyAPI("StudioShotAPI")`, `prim.GetAttribute(...)`).
  usdcc only ever asks the schema registry, so for usdcc the two kinds are the same.

**The kinds of schema**, and how many this build has (*checked*, 140 in 19 plugins, all with a
library: a stock OpenUSD has no codeless schema):

| Kind | Count | Example | Can a user make one? |
|---|---|---|---|
| Concrete typed: a prim type | 64 | `Mesh` | Yes |
| Abstract typed: a base for others | 16 | `Gprim` | Yes |
| Single-apply API | 45 | `PhysicsRigidBodyAPI` | Yes |
| Multiple-apply API, with an instance name | 8 | `CollectionAPI:lightLink` | Yes |
| Non-applied API: C++ helpers only | 5 | `ModelAPI` | No: without code it is nothing |
| The two roots | 2 | `Typed`, `APISchemaBase` | No |

What a schema can also say, all of it plain metadata in `schema.usda`:

- **Built-in API schemas:** a type or an API that always comes with others (`RectLight` with `LightAPI`).
- **Auto-apply:** an API that puts itself on every prim of some type. This build has one:
  `TyphoonRenderSettingsAPI` on `RenderSettings`.
- **Can only apply to:** the types an API is allowed on.
- **Versions:** `Capsule_1` beside `Capsule`, a family ([CREATE_MENU.md](CREATE_MENU.md) §1).
- **Per property:** type, fallback, `allowedTokens`, documentation, display name and group, `uniform`.

## 2. What was checked

A test library `studio` with a prim type (`StudioAsset`, an Xformable), a single-apply API, a
multiple-apply API and an API auto-applied to Mesh; then a second library `show` built on it.

**It works:**

- **`usdGenSchema` needs no compiler** for a codeless library. It writes `plugInfo.json` and
  `generatedSchema.usda`, and nothing else.
- **Registered before the first stage, all four behave like USD's own:** `IsA(Xformable)`, fallbacks,
  `allowedTokens`, `ApplyAPI()` by name, an instance of the multiple-apply one
  (`studioTag:dept:value`), and the auto-applied API on every Mesh with nothing authored.
- **A library can build on another codeless one:** `show` sublayers `@studio/schema.usda@`, its
  `ShowHero` inherits `StudioAsset` and has `StudioShotAPI` built in, and its `ShowLookAPI` refuses an
  Xform in USD's own words ("can only be applied to prims of the following types: ShowHero").
- **The deploy's own Python runs it:** `python\python.exe -m pxr.Usd.usdGenSchema`, with the deploy's
  `bin` on `PATH`. The deploy has the generator, its templates (`bin/usd/usd/resources/codegenTemplates`)
  and every stock `schema.usda`. It lacks only `jinja2` (§9, decision 1).
- **A file whose schemas aren't loaded keeps its data.** The values read as authored, the type name
  stays, and `fallbackPrimTypes` in the file's metadata made the unknown `StudioAsset` compose as an
  Xform.
- **`--validate` says whether the generated files are current:** exit 0 when they match
  `schema.usda`, 1 after an edit.

**The traps:**

| # | Trap | So |
|---|---|---|
| 1 | **Registered after any stage has used the schema registry, a schema half works:** its type is known, it has no definition, `ApplyAPI()` throws. As [PLUGINS.md](PLUGINS.md) §3.1 found | A new or changed schema takes effect at the next start. Nothing can reload one |
| 2 | **The generated `plugInfo.json` isn't usable:** `LibraryPath`, `ResourcePath` and `Root` are `@PLUG_INFO_...@` placeholders that OpenUSD's CMake fills in | usdcc fills them in once (`""`, `"."`, `"."`). *Checked:* generating again keeps them |
| 3 | **A type name another plugin has breaks that type.** A codeless `Cube` generates without complaint; at the first stage USD reports "Cannot set alias 'Cube'", and `Cube` no longer means UsdGeom's | The editor refuses a name that is registered elsewhere, before generating |
| 4 | **Redeclaring an inherited property with another type is accepted in silence:** `int visibility` on an Xformable replaces the token | The editor warns, naming the schema the property comes from |
| 5 | **A library that builds on another needs that one registered while generating:** otherwise "Could not find the defining layer for schema: StudioAsset" | The generator runs with the base library's folder on `PXR_PLUGINPATH_NAME` |
| 6 | **`usdGenSchema` always warns that `__init__.py`, `CMakeLists.txt` and `module.cpp` are missing,** codeless or not | Those three lines are dropped from what the user sees |
| 7 | **It can't run inside usdcc.** It sets an environment variable USD reads once, and ends with `sys.exit()` | A child process |
| 8 | **Run as `python -m pxr.Usd.usdGenSchema`, a library that builds on another fails** with "list index out of range": importing `pxr.Usd` first has OpenUSD read `USD_DISABLE_PRIM_DEFINITIONS_FOR_USDGENSCHEMA` before the script sets it. *Found in phase 4.* OpenUSD's own `bin/usdGenSchema` runs the file as a script | The child gets the variable from the start |

**What `usdGenSchema` does catch,** each with exit 1 and a message naming the prim: an API not named
`...API`, a multiple-apply API with properties and no `propertyNamespacePrefix`, a class that is
neither typed nor an API, and any USDA syntax error, with file, line and column.

## 3. What there is today

- **Using schemas is covered.** The Create menu lists every registered prim type
  ([CREATE_MENU.md](CREATE_MENU.md)), Applied Schemas applies and removes API schemas
  ([APPLIED_SCHEMAS.md](APPLIED_SCHEMAS.md)), and the Attribute Editor's sections come from the schema
  registry ([ATTRIBUTE_EDITOR.md](ATTRIBUTE_EDITOR.md) §5). A plugin's schema appears in all three with
  no code.
- **A plugin can bring one:** the folders in its manifest's `"usd"` are registered at the top of
  `main()`, before any stage ([PLUGINS.md](PLUGINS.md) §3.1). View > Plugins lists the prim types and
  API schemas each brought.
- **Missing:** a place to look at a schema, and anything that writes one. Today that is a text
  editor, OpenUSD's documentation and a command line.

## 4. The Schema Explorer

A view panel, **Schemas**, like Dependencies or the Layer Editor.

```
┌ Schemas ──────────────────────────────────────────────────────────────────────────────┐
│ [filter...            ]  Show: (All ▾)   │  PhysicsRigidBodyAPI          single-apply API │
│ ▾ Geometry (usdGeom)                     │  usdPhysics · C++ · version 0                  │
│     ▸ Imageable            abstract      │  "Applies physics body attributes to any..."   │
│         ▸ Xformable        abstract      │                                                │
│             Camera                       │  Comes with:  (nothing)                        │
│             ▸ Gprim        abstract      │  Auto-applies to:  (nothing)                   │
│     MotionAPI              API           │  Can apply to:  any prim                       │
│ ▾ Physics (usdPhysics)                   │                                                │
│     PhysicsRigidBodyAPI    API           │  Property              Type      Fallback      │
│     PhysicsDriveAPI        API, multiple │  physics:velocity      vector3f  (0, 0, 0)     │
│ ▾ studio            codeless · editable  │  physics:kinematicEn…  bool      false         │
│     StudioAsset                          │  physics:simulationO…  rel                     │
│     StudioShotAPI          API           │                                                │
│ ▾ This stage                             │  On this stage: 12 prims   [Select]            │
│     Not registered: OldRigAPI (3 prims)  │  [Apply to Selection]  [Show Source]           │
└───────────────────────────────────────────────────────────────────────────────────────────┘
```

**The tree,** by plugin under the friendly names Applied Schemas already has
(`schemaCatalog()`'s table), typed schemas nested by inheritance, API schemas flat below them.

- A plugin with no library is marked **codeless** (`PlugPlugin::IsResource()`, *checked*), and
  **editable** when its folder is one usdcc can write (§5.1).
- **Show** filters by kind: prim types, API schemas, abstract ones, codeless ones, used on this stage.
- Versions of one family sit together, as their names sort: Capsule, then Capsule_1.

**The details,** for the chosen schema:

- Its kind, plugin, version and documentation.
- **Inherits:** the chain to `Typed`, each a link.
- **Comes with:** its built-in API schemas. **Auto-applies to** and **Can apply to:** the type lists.
- **Properties:** name, type, fallback, `uniform`, allowed tokens, documentation, and the schema that
  declares each, so an inherited one reads "from Imageable". The Attribute Editor works that out
  already ([ATTRIBUTE_EDITOR.md](ATTRIBUTE_EDITOR.md) §5, rule 4).
- **On this stage:** how many prims have the type or the API, with **Select**.

**The actions:**

- **Create** (a concrete type) and **Apply to Selection** (an API): the Create menu's and Applied
  Schemas' own calls, so the rules, questions and undo steps are theirs.
- **Show Source:** the plugin's `schema.usda` in the Show USDA dialog
  ([USDA_VIEW.md](USDA_VIEW.md) §10), at the schema's class. *Checked:* it is at
  `<resource path>/<plugin name>/schema.usda` for OpenUSD's own plugins. Where a plugin ships none,
  `generatedSchema.usda` instead. **Show Generated Schema** opens `generatedSchema.usda`, what USD
  reads, with every inherited property written out.
- **Edit**, for an editable one: §5.

**This stage,** at the bottom of the tree, is what the open file uses:

- Every type and API schema on it, with counts.
- **Not registered:** a type or an applied schema USD doesn't know, with its prims. Applied Schemas
  shows that per prim, as an amber pill; here it is the whole stage at once, which is the question
  when a file arrives from somewhere else.
- The file's `fallbackPrimTypes`, when it has them.

**Where the data comes from:** `schema_info.h` in `usdcc_usd`, Qt-free, as `applied_schemas.h` is.
`UsdSchemaRegistry` for kind, family, version, definitions and the type lists, `TfType` for
inheritance, `PlugRegistry` for the list of schemas, the plugin and its paths. It loads no library: all of it is plugin
metadata and `generatedSchema.usda`.

**Python:** `usdcc.usd.schemas()` and `usdcc.usd.schema(name)`, returning the same data as
dictionaries, so the MCP server ([MCP.md](MCP.md)) can answer "what does this schema define".

## 5. The Schema Editor

For codeless schemas only. It edits a `schema.usda` and generates the plugin from it.

### 5.1 Where a schema lives

In a usdcc plugin: its own, or beside the Python of one that has it.

```
studio/
  usdcc-plugin.json            "usd": ["usd"]
  usd/
    plugInfo.json              generated; Root ".", ResourcePath "."
    generatedSchema.usda       generated
    studio/schema.usda         the source the editor edits
```

- ***Checked:*** that layout registers, and another library's `@studio/schema.usda@` finds the
  source through it, as it finds OpenUSD's own.
- **One library per `usd` folder.** A plugin with two lists two folders.
- **New Schema Library...** in View > Plugins, next to **New Plugin...**, asks for a name and a prefix
  and writes the folder in the user's plugins folder. A library in a studio's folder
  (`USDCC_PLUGIN_PATH`) is editable when the folder is writable.
- **Distribution is the plugin folder.** Copied to a studio's plugin path it is in every usdcc.
  The `usd` folder alone, on `PXR_PLUGINPATH_NAME`, serves any other USD application: it is a plain
  USD plugin and has nothing of usdcc in it.

### 5.2 The form

```
 Library: studio      Prefix: Studio      [Generate]   ● changed since last generate
┌───────────────────┬───────────────────────────────────────────────────────────────┐
│ StudioAsset       │ Name [StudioShotAPI]   Kind (Single-apply API ▾)              │
│ StudioShotAPI   ● │ Documentation [Shot data.                                  ]  │
│ StudioTagAPI      │ Comes with [+]                Auto-applies to [+]             │
│ [+ Type] [+ API]  │ Can apply to [+]                                              │
│                   │                                                               │
│                   │ Property                Type      Fallback   Tokens   Group   │
│                   │ studio:shot:frameStart  int       1001                Shot    │
│                   │ studio:shot:camera      rel                                   │
│                   │ [+ Attribute] [+ Relationship]                                │
└───────────────────┴───────────────────────────────────────────────────────────────┘
```

- **A schema:** name, kind (prim type, abstract type, single-apply API, multiple-apply API), what it
  inherits, documentation, built-in APIs, auto-apply and can-apply-to lists. The lists are pickers
  over the registered schemas. A multiple-apply API also has its property prefix.
- **A property:** name, type, fallback, `uniform`, allowed tokens, documentation, display name and
  group.
  - The type list is USD's own, as the Attribute Editor takes it ([ATTRIBUTE_EDITOR.md](ATTRIBUTE_EDITOR.md) §4).
  - The fallback's editor is the Attribute Editor's for that type, so a color gets a swatch and a
    token with allowed tokens a combo.
- **It edits the layer through Sdf,** so whatever it doesn't show survives: comments are lost on
  save, as with any USDA file USD writes, but unknown metadata and `customData` are kept.
- **Edit as Text** opens `schema.usda` in the Script Editor. The form reads it again on save, and a
  syntax error shows at its line.
- **Undo** is the form's own, as it has no stage.

### 5.3 Generate

1. Save `schema.usda`.
2. Run the checks of §5.4. An error stops here.
3. Run `python.exe -m pxr.Usd.usdGenSchema -q <lib>/schema.usda .` in the `usd` folder, as a child
   process, with the folders of the libraries it builds on in `PXR_PLUGINPATH_NAME` (trap 5). Into a
   temporary copy first, so a failure leaves the working plugin as it was.
4. Fill in `plugInfo.json`'s three placeholders if they are there (trap 2).
5. **Try it,** in a second child process: register the folder, make a prim of each type, apply each
   API, and report what USD said. This catches what only loading shows (trap 3).
6. Move the files into place, add `"usd": ["usd"]` to the manifest if it's missing, and say the
   schema is ready **at the next start**, with a **Restart** button (trap 1).

What `usdGenSchema` printed is in the Log panel, without the three lines of trap 6. An error names
its schema, and the form selects it.

**Until the restart** the Explorer shows the library as **generated, not loaded**, reading the new
`generatedSchema.usda` as a file. After it, the Explorer shows it like any other.

**Out of date:** a `schema.usda` newer than what was generated shows the dot above. On start,
`usdGenSchema --validate` would say the same for a library edited by hand, though it costs a process
per library, so it runs only when the Explorer opens.

### 5.4 The checks the editor makes

Before generating, since USD makes them too late or not at all:

| Check | Why |
|---|---|
| A schema's name isn't registered by another plugin | Trap 3 |
| A name is a valid identifier; an API's ends in `API` | `usdGenSchema`'s rule, said earlier and in place |
| A property's name isn't inherited with another type | Trap 4: a warning, with **Use Anyway** |
| Property names of a library share a namespace (`studio:`) | A warning. Without one, two studios' `status` collide |
| A fallback is one of the allowed tokens | Nothing else checks it |
| The library's prefix differs from the registered ones | The `TfType` names (`StudioStudioAsset`) come from it |

### 5.5 Changing a schema that files use

A schema isn't in the files, so files don't follow a change:

| Change | Existing files |
|---|---|
| A property added | Read its fallback |
| A fallback changed | Prims with no authored value read the new one |
| A property removed or renamed | The authored values stay under the old name, as plain attributes no schema explains. The Attribute Editor shows them under Custom |
| A property's type changed | Authored values of the old type no longer match |
| A schema renamed or removed | Its prims have an unknown type or an unregistered API (the amber pill) |

- **The editor says which of these a Generate would do,** against the loaded version of the
  library, before doing it.
- **Version instead of change:** USD's answer to an incompatible change is a new version
  (`StudioAsset_1`), with the old one kept. The editor offers **New Version** on a schema. Migrating
  a file's prims to it is a script, not part of this plan.
- **`fallbackPrimTypes`:** for a new prim type that inherits a known one, the editor can offer to
  have usdcc write the fallback into files it saves, so they open as that type where the plugin
  isn't. *Checked* that USD honors it; not how `UsdStage::WriteFallbackPrimTypes()` chooses.

## 6. Promote to Schema

From a prim: **Make Schema from Attributes...** in the Attribute Editor's Custom section menu.

- It lists the prim's properties no schema defines (the Custom section's own list), each with a
  checkbox, its type, and its value as the proposed fallback.
- It asks for the library (an editable one, or a new one), the API's name, and whether a namespace
  should be added to the names.
- It writes the API into `schema.usda` and opens the editor on it. Nothing is generated yet.
- **After the restart,** a second step offers to apply the new API to the prims that have those
  attributes, in one undo step. Renaming attributes into a namespace on existing prims belongs to
  that step.

## 7. Later

- **Attribute templates.** A class prim with attributes, inherited by prims that want them, is a
  schema in everything but the registry. *Checked:* the prim gets the attributes, their fallbacks,
  allowed tokens and display groups, with no plugin, and it travels with the file. It needs
  authoring help of its own (make a class from a prim, inherit it), and belongs with
  [COMPOSITION_ARCS.md](COMPOSITION_ARCS.md).
- **C++ schemas: scaffold only.** `usdInitSchema` and `usdGenSchema` write a library's sources and
  its CMake file. usdcc could run them from a codeless library ("Export as C++ Project"). Building
  it is the user's, with the plugin SDK ([PLUGIN_SDK.md](PLUGIN_SDK.md)).
- **Schemas from shaders:** `usdgenschemafromsdr` makes API schemas from Sdr nodes, which is how
  renderers publish their light and settings attributes. It is in the OpenUSD install, not in the
  deploy.
- **Checking a stage against its schemas:** a token outside its allowed list, a value of the wrong
  type, an API on a type it can't apply to. OpenUSD's validators (`usdValidation`) are the place to
  start.
- **Icons and Create menu entries from the editor:** a new prim type gets its base type's icon and
  lands in Create > Plugins. Both have Python calls today (`set_create_entry()`,
  [PRIM_ICONS.md](PRIM_ICONS.md) phase 2); the editor could write them into the plugin.

## 8. What it doesn't do

- **No reload.** Trap 1 is USD's, and there is no way around it in one process.
- **No editing of OpenUSD's own schemas,** or of any plugin with a library: its C++ was generated
  from its `schema.usda`, and the two have to agree.
- **No renderer support.** A schema is data. Storm, or any delegate, draws a `StudioAsset` as the
  Xformable it is and ignores the rest.

## 9. Decisions to make

1. *Decided in phase 4: vendored, as recommended (§12).* **Where `jinja2` comes from.** `usdGenSchema` imports it; the deploy doesn't have it.
   Recommended: **ship it in the deploy** (`jinja2` and `markupsafe`, both BSD-3-Clause, 1.3 MB),
   so Generate works on a machine with no network. The alternatives:
   - Install it through the shared packages folder when the editor first needs it
     ([PYTHON_PACKAGES.md](PYTHON_PACKAGES.md)): nothing more in the deploy, but a download before
     the first Generate.
   - Write the two files without `usdGenSchema`: no dependency, but its rules reimplemented and
     kept in step with each OpenUSD.
2. **A view panel, or a tab in Help > USD Runtime?** Recommended: **a view panel.** The Explorer
   acts on the stage and the selection, which a dialog about the runtime shouldn't.
3. **The editor inside the Explorer, or a window of its own?** Recommended: **inside**, replacing
   the details of an editable schema when Edit is pressed, so the tree is the same in both.
4. **Require a namespace on property names?** Recommended: **warn, don't refuse** (§5.4). Many
   studios have attributes without one already, and Promote has to take them as they are.
5. **Promote: keep the attribute names, or move them into a namespace?** Recommended: **keep them
   by default.** A schema that matches existing files is the point, and renaming is a change to
   every file.
6. **Restart from the app?** Recommended: **yes**, a Restart button that goes through the usual
   unsaved-changes questions and comes back with the session ([SESSIONS.md](SESSIONS.md)).

## 10. Phases

| Phase | Work | Done when |
|---|---|---|
| **1. The Explorer's data** *(done)* | `schema_info.h` (`usdcc_usd`, Qt-free): every schema with its kind, plugin, codeless or not, family and version, inheritance, built-in, auto-apply and can-apply-to lists, properties with their declaring schema, source paths; `stageSchemaUse(stage)` with counts and the unregistered ones. `usdcc.usd.schemas()`, `schema()` | Tests: the 140 schemas by kind; a codeless test plugin told from a C++ one; an inherited property's owner; a stage with an unknown type and an unregistered API |
| **2. The Explorer** *(done)* | The Schemas view panel: tree, filter, Show, details, links between schemas, Show Source | Every schema of §1 is found by name; a plugin's codeless schema shows as one; the source opens at the class |
| **3. The stage in it** *(done)* | This stage, Select, Create, Apply to Selection | Counts follow edits; the unregistered list matches the amber pills; Create and Apply make the same undo steps as their menus |
| **4. Generating** *(done)* | `schema_library.h`: reading and writing `schema.usda` through Sdf, the checks of §5.4, the two child processes, the placeholders, moving into place. `jinja2` as decided | Tests against the deploy's Python: the `studio` and `show` libraries of §2 generate and load; each trap of §2 is caught with its message; a failed Generate leaves the old files |
| **5. The editor** | The form, New Schema Library..., Edit as Text, "generated, not loaded", what a Generate changes for existing files (§5.5), Restart | A library made, generated and, after a restart, creating prims and applying APIs, by a script driving the widgets |
| **6. Promote** | Make Schema from Attributes..., and applying the API after the restart | A prim's custom attributes become an API whose fallbacks are their values; the prims get it in one undo step |

Phases 1 to 3 need nothing from 4 to 6 and are useful alone.

## 11. Not checked

- **In usdcc itself.** Every check was in a plain Python with this OpenUSD, or with the deploy's
  `python.exe`. That the app's plugin scan registers the layout of §5.1 follows from
  [PLUGINS.md](PLUGINS.md) §3.1, and wasn't run.
- **Linux.** The Linux deploy leaves `*.cmake` files out of `lib/usd`; the codeless path needs only
  the `plugInfo.json` template, which isn't one.
- **How the app shows a prim of an unknown type** today, in the Outliner and the Attribute Editor.
- **An authored `custom` attribute that a schema then defines:** whether it still reads as custom,
  which Promote's second step depends on.
- **Schema versions made by hand** (`StudioAsset_1` in a codeless library), and
  `WriteFallbackPrimTypes()`.
- **A codeless library on an older USD.** Codeless schemas exist since 21.08; what this build
  generates wasn't loaded in another version.

## 12. As built

Phases 1 and 2.

- **`schema_info.h`** (`usdcc_usd`, Qt-free): `allSchemas()`, `schemaInfo(name)`, `schemaKindName()`,
  `schemaCategoryName()` (the Applied Schemas menu's table, moved here and shared), and
  `schemaSourceSearchPaths()`.
  - **The list comes from `PlugRegistry::GetAllDerivedTypes()`,** not `TfType`'s: `TfType` knows a
    plugin's types only once something has declared them. *Checked:* in `usdcc_tests`, `TfType`
    gave 124 of the 140.
  - **Who declares a property,** as the Attribute Editor's categorizer does: a built-in API first,
    multiple-apply instances before the rest, then the most basic typed schema. A built-in API's
    properties are its, so RectLight's `inputs:intensity` is LightAPI's and its
    `collection:lightLink:includes` is `CollectionAPI:lightLink`'s.
  - **An API's definition lists the API itself** (a multiple-apply one as
    `CollectionAPI:__INSTANCE_NAME__`); it is left out of its built-in APIs.
  - **A type's auto-applied APIs are listed apart** from those its `schema.usda` builds in.
  - **Fallbacks are shown as USDA writes them:** strings and tokens quoted, assets in `@`s, `true` and
    `false` for a bool.
  - **A schema with no definition** (a non-applied API, a root) takes its documentation from its
    plugin's `generatedSchema.usda`, where `usdGenSchema` keeps it as `customData.userDocBrief`.
  - **The usd plugin's schemas are under Core** here, not the "Collections & Color" of the Applied
    Schemas menu: the plugin also has the roots, `ModelAPI` and `ClipsAPI`.
- **`usdcc.usd.schemas()` and `schema(name)`:** the same data as dicts (`name`, `kind`, `base`,
  `plugin`, `codeless`, `built_in_apis`, `auto_applied_apis`, `properties` with `declared_by`, ...).
- **`SchemaExplorerViewPanel`** (`usdcc_usd_ui`), the view panel type `schemas`, "Schemas" in the
  panel menus.
  - The tree: a bold row per category, its plugins in the tooltip and "codeless" in the Kind column
    when one is. Typed schemas nest under their base when it's in the same category, so Mesh is
    Geometry / Imageable / Xformable / Boundable / Gprim / PointBased / Mesh; API schemas follow,
    flat. Prim types have their Outliner icon.
  - **Filter** matches the name or the `TfType` name, ignoring case; a match keeps its ancestors,
    dimmed. **Show:** All, Prim Types, Abstract Types, API Schemas, Codeless. The toolbar says how
    many are shown.
  - The details: kind, plugin, `TfType`, version; Inherits, Built-in APIs, Auto-applied, Applies
    itself to, Can apply to, Instances (a multiple-apply API's property namespace), Derived, Built
    into, Files; then the documentation. Every schema name is a link.
  - The properties, below: name, type (with `uniform`), fallback and Declared by, dimmed when it isn't
    the schema's own. The tooltip has display name and group, allowed tokens and documentation.
    Double-click goes to the declaring schema.
  - The right-click menu: Show Source, Show Generated Schema, Copy Name, Copy Type Name. Options:
    Refresh, Expand All, Collapse All.
  - Going to a schema the filter hides clears the filter.
  - A session keeps the chosen schema, the filter and Show.
- **A plugin's `schema.usda` now opens with its sublayers.** It sublayers others by plugin
  (`@usd/schema.usda@`), which `usdGenSchema` finds in the plugins' resource folders, and a stage
  opened from it didn't, with a warning. `stageResolverContext()` gives such a file (and only such a
  file: `<resources>/<plugin>/schema.usda` of a registered plugin) those folders as search paths.
- **Tests:**
  - `schema_info_*` (`usdcc_tests`): every kind, inheritance, declaring schemas, built-in,
    auto-applied and can-apply-to lists, versions, an instance's schema, an unknown name.
  - `schema_info_codeless`, run as the ctest `usdcc_schema_info` with
    `test_data/schemas/usdccTestSchemas` on `PXR_PLUGINPATH_NAME`: a codeless library generated by
    `usdGenSchema` (a prim type on an abstract base, single-apply, multiple-apply and auto-applied
    APIs). In `usdcc_tests` itself it is skipped with a line saying so: registered for every test,
    its prim type would land in Create > Plugins, which `create_prim_test` says USD's own never do.
  - `schema_explorer_panel` (`usdcc_ui_tests`): the nesting, details, properties, links, filter,
    Show, session state, both files, and that `schema.usda`'s stage has its sublayers.
  - `python_app_api` (`usdcc_python_tests`): `schema()` and `schemas()`.
- **In the app:** a script opened the panel on a stage and chose RectLight and CollectionAPI; the
  pictures show the tree, details and properties as above, in the dark theme.

### Phase 3: the stage in it

- **`stageSchemaUse(stage)`** (`schema_info.h`): every registered type and API schema the stage's prims
  have, to their prims; the types and applied schemas it names that USD doesn't know; and what such a
  type composes as instead. It walks every defined prim that isn't a class, inactive ones and instance
  proxies included. An API counts whether authored, built in or auto-applied, and two instances of
  one schema count a prim once (a RectLight is one prim for `CollectionAPI`).
- **This stage**, last in the tree, with a stage: a row per schema it uses with how many prims, then
  **Not registered** in amber, the Applied Schemas pill's colour. It opens by itself when something
  isn't registered. **Show: On This Stage** lists only what the stage uses.
- **It follows the stage:** a resync (a type or an applied schema changed, a prim added or removed)
  counts again, coalesced. Behind another tab it only notes the edit, and counts when shown.
- **The details** of a schema, with a stage, start with **On this stage** (how many, **Select**),
  then **Create** for a prim type (where it would go: "at the root", "under /World", or why not) or
  **Apply** for an applicable API ("Apply to Selection (2 prims selected)", or "Select prims to apply
  it to"). They change with the selection.
- **An unregistered one's details** say what that means: an unknown type's prims compose as the
  stage's `fallbackPrimTypes` type, or have no schema; an unknown API adds nothing. Then its prims,
  each a link that selects it. It has no properties.
- **Create** is the Create menu's: `prim_actions::createTarget()` and `createPrim()`, the "Create Cube"
  undo step, then the prim selected.
- **Apply to Selection** is the Attribute Editor's + menu, on several prims: the questions moved from
  `PropertySheet` into **`askAndApplySchema()`** (`apply_schema_ui.h`), which both use.
  - One undo step for all of them: `prim_actions::applySchema()` and `applySchemaWithTarget()` take a
    list of paths ("Apply MotionAPI to 2 prims"; one prim keeps "Apply MotionAPI to Mesh").
  - A prim it can't go on (has it already, USD says it can't apply, an instance proxy) is left out,
    and the question names it. When that's every prim, it says why and changes nothing.
  - A multiple-apply schema's instance name must suit every prim it's applied to; MaterialBindingAPI
    asks for one material for all.
- The right-click menu has **Select Prims Using It (n)**, **Create *Type*** and **Apply to Selection**
  too; an unregistered row has Select and Copy Name.
- The panel type takes the app's `UndoManager` now.
- **Tests:** `schema_info_stage_use` (`usdcc_tests`: counts, classes left out, inactive prims and
  instance proxies counted, unknown types and APIs, a fallback type); `schema_explorer_stage`
  (`usdcc_ui_tests`: the group and its counts, Select, Create under the selection and its undo step,
  the counts following an edit and its undo, Apply to two prims in one step, applying again refused
  with why, a prim left out and named, Show: On This Stage, an unregistered one's details and links,
  and its row going when its prims do). `schema_pills` still passes, through `askAndApplySchema()`.
- **In the app:** on a stage with a Cube carrying `MotionAPI` and an unknown `OldRigAPI`, and a prim of
  an unknown type: This stage listed them with counts, the two unknown ones in amber, and each one's
  details as above.
- **Not done:** the panel isn't in any shipped workspace; open it from a panel menu. Prims inside an
  unloaded payload aren't counted, as they aren't on the stage.

### Phase 4: generating

No UI yet: the Schema Editor (phase 5) is what will call it.

- **`jinja2` is vendored** (§9, decision 1): `jinja2` 3.1.6 and `markupsafe` 3.0.4 in
  `src/python/usdcc/_vendor`, beside `packaging`, each with its `LICENSE.txt` (BSD-3-Clause). Pure
  Python, 560 KB; `markupsafe` without its optional C speedups. They came from the build Python's
  install, every file checked against its `RECORD`. So dev builds, CI and the deploy all have it,
  with no download and no change to how a deploy is made; `tools/deploy.py` now requires them.
  The child process puts `usdcc/_vendor` on `PYTHONPATH`, as `usdGenSchema` imports `jinja2` as a
  top-level package; nothing in the app does.
- **`schema_library.h`** (`usdcc_usd`, Qt-free):
  - `SchemaLibraryDef`, `SchemaClassDef`, `SchemaPropertyDef`: a library as the editor will see it.
  - `readSchemaLibrary()` and `writeSchemaLibrary()` through Sdf. Writing updates the file in place,
    so what the definition doesn't cover is kept (the layer's comment, a class's other customData);
    classes and properties it no longer lists go; a fallback that doesn't parse stops it before
    anything is written. *Found:* USDA writes a prim's properties sorted by name, whatever order
    they were in, so a definition has them by name too.
  - `parseFallback()` parses a fallback as USDA would, with USD's own message when it can't;
    `usdaValueText()` writes one back (the Explorer's fallbacks use it too).
  - `checkSchemaLibrary(library, bases)`: §5.4's checks, and more: two classes or properties of one
    name, an unknown base, value type or built-in API, an API not named `...API`, a multiple-apply
    API with properties and no prefix, allowed tokens on a non-token attribute, auto-apply on a
    prim type. Errors stop Generate; warnings (a replaced inherited property, no namespace, an
    unknown type to apply to) are said. `bases` are the libraries it builds on, not registered
    yet, whose classes count as known.
  - `fillPlugInfoPlaceholders()`, `findSchemaLibrary()`, the paths of a library's files.
- **`generateSchemaLibrary()`** (`schema_generator.h`, `usdcc_usd_ui`, as it uses `QProcess`), §5.3:
  1. reads and checks the source, with the base libraries' sources;
  2. runs `usdGenSchema` with the app's Python and `sys.path` (`SchemaPython`), into a temporary
     folder holding the current `plugInfo.json`, with the base libraries on `PXR_PLUGINPATH_NAME`;
  3. fills in the placeholders;
  4. runs `python -m usdcc._schema_check` on the result: registers it, makes a prim of each type,
     applies each API (a multiple-apply one as an instance), and reports any USD error;
  5. puts both files in place, each through a `.new` and an `.old`, putting back what was there if
     either fails.
  It blocks, up to two minutes per process; phase 5 runs it off the UI thread. The output shown
  is `usdGenSchema`'s without its boilerplate (trap 6).
- **Tests:**
  - `schema_library_*` (`usdcc_tests`): the test library read; a round trip unchanged; edits (a
    property added, a fallback changed, a class removed) read back as written, a class's own
    customData kept; a bad fallback refused with the file untouched; fallbacks of several types;
    each check; the placeholders.
  - `schema_generator` (`usdcc_python_tests`, with the app's own Python): `studio` generates, with its
    placeholders filled, every schema tried and nothing left over; `show`, which builds on it, is
    refused by the check without it and generates with it, its API applying to its type;
    `usdGenSchema`'s own failure (a base whose file isn't sublayered) is reported and leaves the old
    files; a name another plugin has is stopped before `usdGenSchema` runs; no Python, and a
    library whose name isn't its folder's.
  - **The deploy's Python:** the local deploy's `python.exe`, with this build's `usdcc` package on
    `PYTHONPATH` (the deploy predates the vendored files), generated `test_data/schemas/usdccTestSchemas`
    and `_schema_check` made or applied all five schemas without an error.
- **Not done:** the manifest's `"usd"` entry (§5.3 step 6) and finding a library's base libraries
  from its sublayers are the editor's, in phase 5. A schema.usda written by hand that Sdf can read
  but the definition can't express (a non-applied API, `nonApplied`, ...) reads, and is written
  back with what the definition holds.
