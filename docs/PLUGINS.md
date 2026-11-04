# Plugin System

Status: **phases 1 to 4 are built: Python plugins and USD plugins work. C++ plugins (phases 5 and 6) are still a plan.** This is milestone M9 in
[PLAN.md](PLAN.md), and answers its open question 4.

A plugin is a folder the app finds at startup, holding any mix of Python and USD plugins (schemas, render delegates, file formats), and later a C++
library. It adds to the app without forking it.

Marked "checked" below means run against this build (USD 26.11). §5 says what was tested and how.

## 1. Writing a plugin

```
notes/
  usdcc-plugin.json
  python/notes/__init__.py     imported, then activate(plugin) is called
  deps/                        third-party Python modules (§3.10)
  usd/plugInfo.json            USD part: schemas, render delegates
```

```json
{
  "id": "com.example.notes",
  "name": "Notes",
  "version": "1.2.0",
  "description": "Notes on prims",
  "api": 1,
  "python": { "module": "notes", "requires": ["numpy>=1.26,<3"] },
  "usd": ["usd"],
  "requires": ["com.example.base"]
}
```

Everything but `id`, `name`, `version` and `api` is optional, so a schema-only plugin has no code at all.

```python
# python/notes/__init__.py
import usdcc.app, usdcc.tools

def activate(plugin):
    usdcc.app.register_view_panel(plugin.id + ".panel", "Notes", NotesPanel)
    usdcc.app.add_menu_action("Tools/Add Note", add_note)
    usdcc.tools.register_tool(plugin.id + ".measure", "Measure", MeasureTool)

def deactivate(plugin):  # optional: at quit, for saving
    pass
```

[src/plugins/measure_example](../src/plugins/measure_example) is a working example of all of it. Try it with `usdcc --plugin-path src/plugins`.

**Found in, the first with an id winning (logged):** `--plugin-path` folders, `USDCC_PLUGIN_PATH` (a studio's), `%LOCALAPPDATA%/usdcc/plugins` (the
user's), then `plugins/` next to `bin/` in a deployed app. `--no-plugins` starts clean when one breaks the app. A deploy puts the examples in
`examples/plugins`, where they don't load unless asked for.

**What a plugin can add:**

| Extension point | Call |
|---|---|
| View panel types | `usdcc.app.register_view_panel(id, name, cls)` |
| Side panels | `usdcc.app.add_side_panel(panel, area, id=...)`. Give an `id`: otherwise the title is its identity in the saved layout |
| Main menu | `usdcc.app.add_menu_action("Menu/Action", callback, shortcut, tip)`, one undo step per run |
| Outliner's prim menu | `usdcc.app.add_prim_menu_action(label, callback, applies=None, id=None)`, one undo step per run |
| Viewport tools | `usdcc.tools.register_tool(id, label, cls, icon="")` |
| App events | `usdcc.app.connect(event, callback)`: `selection_changed`, `current_stage_changed`, `stage_opened`, `stage_closed` |
| Create menu entries | `usdcc.usd.set_create_entry(...)` |
| Relationship target rules | `usdcc.usd.set_relationship_targets(...)` |
| Schemas, render delegates | A folder named in the manifest's `"usd"` |
| Settings | `plugin.settings.value(key, default)`, `set_value(key, value)`, JSON values, one group per plugin in `usdcc.ini` |
| Logging | `plugin.log`, a logger with the source `plugin:<id>` in the Log panel |

Attribute editors can't be added yet (§4, Later). **View > Plugins** lists the plugins, what each added, and why any isn't running, and turns them on or
off for the next start.

Startup scripts are unchanged. A plugin differs in having an identity, a manifest readable without running it, and files other than Python.

## 2. How it fits together

- **`PluginManager`** ([plugin_manager.h](../src/cpp/scripting/include/usdcc/scripting/plugin_manager.h)): `scan()`, `activate()`, `deactivate()`, and the
  enabled state in `usdcc.ini`.
- **`usdcc._plugins`** ([_plugins.py](../src/python/usdcc/_plugins.py)): the `Plugin` object, loading packages, and the third-party module check.
- **`ScriptedTool`**: a Python tool behind `tools::Tool`. **`PluginManagerSidePanel`**: the panel.
- **The existing calls are the API.** `activate()` runs with its plugin as the current one, so `register_view_panel()` and the rest are attributed to it
  without a second set of functions.

## 3. The issues

### 3.1 USD files have to register before the first stage

**Checked, with a schema-only plugin and `PlugRegistry::RegisterPlugins()`:**

| Registered | `TfType` | Prim definition | `foo` attribute from the schema |
|---|---|---|---|
| Before any stage | Known | Yes | Reads 3 |
| After a stage has used the schema registry | Known | **No** | **Missing** |

The late case reports success and half works: the type is listed, but prims of it have no schema.

**So:** startup has two steps. **Scanning** reads manifests and registers `usd/` folders near the top of `main()`, running no plugin code. **Activating**
comes later (§3.3). **Checked in the app:** a stage on the command line with a plugin's prim type reads the schema's value.

*Render delegates and file formats haven't been tried. I expect the same or stricter, so the same early step covers them.*

A plugin's USD files stay registered even if its Python is then refused or fails, as they can't be taken back (§3.4).

### 3.2 C++ plugins can't link against the app

**This is the largest piece of work, and isn't built.** Every usdcc library but `usdcc_core` is static, and nothing is exported. A plugin linking those
statically compiles and links, then fails quietly:

- **It gets its own copy of every singleton.** `PrimCreationCatalog::instance()` in a plugin is a different catalog, so its entries never
  appear, with no error. `usdcc_core` is a DLL for exactly this reason (its `CMakeLists.txt` says so).
- **It gets its own `ViewPanel`,** with its own Qt metaobject, so `qobject_cast<ViewPanel*>` in the app, which `savePanels()` relies on,
  returns null for its panels.

`_ui.pyd` has this problem already: it links its own `usdcc_ui`. `UiScaleController::instance()` looks itself up through `qApp` to get
round that.

| Option | For | Against |
|---|---|---|
| **The libraries become DLLs, with exports** (recommended) | Plugins use the same classes the app does, and it fixes `_ui.pyd` | About 90 public classes to mark (a rough count); every test and module needs the DLLs next to it |
| **The exe exports its symbols** (`ENABLE_EXPORTS`) | Less build work | `_usd.pyd` in a plain Python has no exe to link to, so `usdcc.usd` keeps its own copy |
| **A narrow C API** | The classic way to a stable ABI | Panels couldn't derive from `ViewPanel`. Qt and USD types cross it anyway, so the ABI still isn't stable |

The dependencies have no cycles (`core` ← `usd` ← `tools`, `ui` ← `usd_ui` ← `scripting_*`), so DLLs are possible. `usdcc_usd` and
`usdcc_tools` have to stay free of Qt, so that `usdcc.usd` still imports in a plain Python.

**A C++ plugin is tied to the exact build,** whichever option is used: `std::` and Qt types cross the boundary, and USD's symbols carry its
version (`pxrInternal_v0_26_11`). The compiler, CRT, Qt and USD all have to match. Debug builds are a trap here, since the app links the
release CRT in Debug (PLAN.md §6).

**So, in phase 6:** the SDK's `usdcc_add_plugin()` CMake function writes those versions into the manifest's `built_for`, and the app compares them
**before loading the DLL**. For now, a manifest with `"native"` is refused, saying that it can't be loaded yet.

### 3.3 Activating comes before the layout is restored

`restorePanels()` and `restoreLayout()` only place panels that exist, so a plugin's panel can only behave like a built-in one if the plugin has
run by then. And `savePanels()` used to skip a type it couldn't find, so one start with a plugin off would have forgotten its panels for good.

**So:** the order in `main()` is:

1. Scan manifests, register USD folders (§3.1), before any stage.
2. Create the window and registries, and open the command-line stage.
3. **Activate plugins,** dependencies first, otherwise by id.
4. `restorePanels()`, `restoreLayout()`, show the window.
5. On `layoutSettled`: startup scripts, then `--script`.
6. At quit: `savePanels()`, then `deactivate()` in reverse order.

`savePanels()` keeps entries whose type is unknown, so they come back with the plugin.

**Checked in the app, over five starts:** a plugin's side panel moved to the right stays there, its view panel comes back, neither appears with
`--no-plugins`, and both come back after that. **The cost on a start:** activating a plugin importing PySide and numpy took 1667 ms the first time, with cold
files, and 122 to 126 ms after that. It's only paid with a Python plugin enabled.

### 3.4 Nothing can really be unloaded

- **USD can't:** `PlugRegistry` has `RegisterPlugins` and nothing to undo it (checked, on its bindings), and `TfType`s stay.
- **Python can't fully:** other code keeps references to its classes and modules, and `sys.modules` keeps what it imported.
- **C++ couldn't safely:** types, metaobjects and callbacks would point into a freed DLL.

**So:** enabling and disabling take effect **at the next start**, and the panel says so. A plugin that fails in `activate()` keeps whatever it had
registered until then, for the same reason. For developing one, re-running a registration replaces the type. A proper Reload could come later.

### 3.5 One Python for everyone

All plugins share one interpreter, so one `sys.modules`. Each plugin's package loads by its file location rather than through `sys.path`, so nothing else
in its `python/` folder can shadow another's. A second plugin naming a module already taken, or one Python already has, is refused, naming it.
Third-party modules are the larger problem, in §3.10.

### 3.6 A plugin can break the app

Plugins run with the user's full rights, and PLAN.md only asks that errors don't take the app down.

- **Python errors:** `activate()` raising marks that plugin failed, logs the traceback, and the others go on. Callbacks log exceptions too.
- **Crashes in native code can't be caught.** One crashing at every start would lock the user out. **So:** a marker file names the plugin being
  activated and is removed afterwards. Finding it at the next start means that plugin crashed, so it's turned off and the app says so, in the status bar
  and the panel, until the user turns it on again. **Checked in the app, with `os.abort()`.** This only covers `activate()`, not a crash later on.
- **Failures in `requires`** skip the plugins that depend on them, saying why. A cycle refuses all of those in it.
- **No sandbox and no signatures.** Stages can't bring plugins with them: code only comes from these folders, as with startup scripts. A plugin found there
  just loads.

### 3.7 Undo

Scripts run from the panel get an undo step automatically, but startup scripts don't (SCRIPTING.md §4.2). A menu action from a plugin should
behave like a built-in one, and authors will forget `usdcc.usd.edit()`.

**So:** actions from `add_menu_action()` and `add_prim_menu_action()` run in one global edit, as `ScriptRunner` does, so each is one undo step, named after
it. A failing one's step is marked as failed. Panels make their own edits with `usdcc.usd.edit()`, like the built-in ones. A tool's drag is one step
if its `mouse_press()` says `starts_edit`. `activate()` gets none: it shouldn't be editing stages.

### 3.8 Tools

`ToolKind` was an enum and `ToolSet` an array of four. **So:** tools have string ids in a list, with the built-in four added the same way, `ToolKind`
is gone, and the rail builds itself from the list, following `toolsChanged`. Built-ins can't be replaced or removed.

**A Python tool** subclasses `usdcc.tools.Tool` (`options`, `set_option`, `build_gizmo`, `mouse_press`, `mouse_move`, `mouse_release`). `ScriptedTool`
passes plain numbers and strings, as Pixar's and pybind11's bindings can't share Gf and Sdf types, and `tools.py` builds them. The gizmo is asked for on every
frame, so an exception is logged once and the tool then does nothing until it's registered again. A tool without an icon gets its label's first letter.

**Measured: 0.017 ms per gizmo** in this build, with a line and a triangle, which is what the plan feared and hasn't turned out to be a problem. A
tool's own work on each event is its own cost.

### 3.9 Things with names

- **Ids:** `id` is letters, digits, `.`, `_` and `-`. Panel and tool ids starting with it is a convention, not enforced.
- **A side panel's identity is its title** unless it's given an `id`, so two plugins' "Notes" would clash in the layout, with a warning logged.
- **Prim menu entries and event connections** end with the app context, while Python is still there.

### 3.10 Third-party Python modules

**What limits the choices:**

- **One interpreter:** a virtual environment per plugin can't work in-process, and one process can only load one numpy.
- **Renaming doesn't work:** nesting a package inside the plugin's own breaks packages that import themselves by absolute name, and it can't
  give compiled packages a second copy.
- **The deployed app has no pip:** `tools/deploy.ps1` copies no `python.exe`, leaves out `ensurepip`, and skips the source Python's `site-packages`.
- **Compiled packages must match USD's Python,** currently 3.12 on Windows x64.
- **Some packages can take the app down:** `PySide6` from pip brings its own Qt (PLAN.md §6 item 11), and `usd-core` competes with `pxr`.
  Others may carry their own Qt or copies of libraries USD uses. `opencv-python` is one I'd expect to, *not checked.*

**So:**

1. **A plugin declares what it needs** in `python.requires`. Declaring installs nothing.
2. **A plugin ships it in `deps/`,** made with `pip install --target deps`, which keeps the `dist-info` folders the check reads. `deps/` isn't on `sys.path`:
   a finder at the end of `sys.meta_path` serves it, after the app's folders, so it can't replace what the app uses.
3. **The app checks before running any plugin code.** Reading the package metadata of every enabled plugin, including what each bundled package itself
   needs, and importing nothing, it refuses:
   - **a conflict:** when plugins can't agree on a version, the later one, naming the other plugin and the package;
   - **a missing package,** saying which;
   - **a protected name:** a plugin bundling `PySide6`, `shiboken6`, `pxr`, `usdcc`, `PySide2`, `PyQt5` or `PyQt6`. Requiring what the app provides is fine.
4. **One copy is used where several would do.** If two plugins bundle numpy and one version satisfies both, the finder serves the newest that does, to both.
   Only one could load anyway, and this makes the choice explicit rather than down to load order.

**The version check needs a parser** for requirement strings, which the standard library doesn't have. `packaging` 24.2 is vendored at
`usdcc._vendor.packaging`, rather than in `site-packages`, so that a plugin's own copy can't clash with it.

**A shared folder for large packages.** Shipping numpy or scipy in every plugin is wasteful. So `USDCC_PYTHON_PATH` and
`%LOCALAPPDATA%/usdcc/python-packages` go on `sys.path`, and a studio or user can fill them with `pip install --target`, using a Python of the same version.
**A copy found there is fixed:** a bundled one can't override it, and a plugin it doesn't suit is told which version is installed. An **Install** button in the
Plugin Manager can come later. It needs `python.exe` and pip in the deploy.

**No pip at startup.** It would need the network at launch, can run package setup code, and could change what other plugins depend on. An install
is something the user asks for.

**Versions that truly can't coexist** have one real fix: the plugin runs that code in its own separate process and talks to it. That's left to its
author, and no helper is planned.

**Checked in the app:** numpy 2.5.4 in a plugin's `deps/`, next to USD and PySide, imports and computes. Only that one compiled package has been tried.

## 4. Phases

| Phase | Builds | Status |
|---|---|---|
| **1. Python plugins** | `PluginManager`, manifests, order, USD folders, new startup order, `savePanels()` keeping unknown types, `--no-plugins`, `--plugin-path`, log source, third-party modules, example plugin | **Done** |
| **2. Extension points** | `add_menu_action()` with undo, prim menu entries, `plugin.settings`, events, side panel ids | **Done** |
| **3. Tools** | Tool ids and list, a rail built from it, Python `Tool`, example tool | **Done** |
| **4. Plugin Manager** | View > Plugins, enable and disable, Open Plugins Folder, crash marker | **Done** |
| **5. Libraries become DLLs** | `usdcc_usd`, `usdcc_tools`, `usdcc_ui`, `usdcc_usd_ui` shared, with export headers. `_ui.pyd`, `_usd.pyd`, tests and deploy updated. **No plugin feature:** the app must behave the same. Test: all suites, a singleton being one instance across DLLs, `usdcc.usd` in a plain Python, the deploy folder on its own | Not started |
| **6. C++ plugins** | Loading, the `built_for` check, entry point, `usdcc_add_plugin()`, SDK in the deploy, example with a panel and tool, `docs/PLUGIN_SDK.md`. Test: PLAN.md's exit criterion, a mismatched build is refused, a plugin's own DLLs are found, a registration reaches the app's own catalog | Not started |

**Phase 5 is the riskiest:** it touches every target, and this project has already had trouble with DLLs and CRTs (PLAN.md §6), so it stands alone with no
features in it. Whether to do 5 and 6 at all is decision 3 below.

**Later:** plugin-provided attribute editors (`createPropertyEditor()` needs a registry first), plugin icons for prim types, Reload, an Install button
for Python modules (§3.10).

## 5. Tested, and not

**Unit tests:** [plugin_manager_test.cpp](../tests/plugin_manager_test.cpp) and [plugin_extensions_test.cpp](../tests/plugin_extensions_test.cpp), including the
example plugin's panel, menu action, prim menu entry and tool.

**In the real app, with scratch plugins, and looked at in a screenshot:** the items marked checked above, and the Plugins panel, the tool rail's letter icons and the
options bar. A deployed folder ran the example plugin with its own Python.

**Not checked:**

- **A real mouse drag with a Python tool in the viewport,** and its gizmo actually drawn. The tests call the `Tool` methods as the viewport does.
- **Clicking through the panel by hand,** such as unticking a plugin with the mouse and restarting. The tests set the checkbox directly.
- **The environment variables** `USDCC_PLUGIN_PATH` and `USDCC_PYTHON_PATH`. What they feed was tested through options and `--plugin-path`.
- **Render delegates and file formats from a plugin folder:** only a schema was tried (§3.1).
- **Compiled packages other than numpy,** in particular ones bringing their own Qt or USD libraries (§3.10).
- **How much phase 5 changes in practice:** no library has been converted. The count of classes is rough, and I haven't looked for function-local
  statics or `inline` state that would change meaning across DLLs.
- **Linux and macOS:** only Windows has been run.

## 6. Decisions

1. **Python first, C++ after:** taken. Phases 1 to 4 are Python and USD.
2. **How C++ plugins link:** recommended, for phase 5: **the libraries become DLLs** (§3.2). It's the most work, and the only option that gives a plugin the
   same classes and fixes `_ui.pyd`.
3. **Do C++ plugins come at all?** **Still open.** The spec asks for them, but stopping here is reasonable if Python is enough, except for code that needs to be
   fast. USD schemas and render delegates work without them.
4. **Turning a plugin on or off:** at the next start (§3.4).
5. **A newly found plugin:** it just loads, as startup scripts do. Someone had to put it in that folder.
6. **Installing:** copying a folder, with Open Plugins Folder in the panel. No zip installer or store for now.
7. **Manifest format:** JSON. Qt reads it with no new dependency, and USD's `plugInfo.json` is the same kind of file.
8. **Compatibility:** `api` is a whole number, raised only when something breaks, and a plugin whose `api` doesn't match is refused with the reason. C++ will
   additionally need the exact build (§3.2).
9. **Third-party Python modules:** declared in the manifest, shipped in `deps/` or a shared folder, and checked before any plugin code runs (§3.10). No pip at startup.
