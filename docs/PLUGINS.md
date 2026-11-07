# Plugin System

All six phases are built. This is milestone M9 in [PLAN.md](PLAN.md), and answers its open question 4.

A plugin is a folder the app finds at startup, holding any mix of Python, USD plugins (schemas, render delegates, file formats) and a C++ library. It adds to the
app without forking it. This covers plugins in general and Python ones; [PLUGIN_SDK.md](PLUGIN_SDK.md) is the guide to C++ ones.

Marked "checked" below means run against this build (USD 26.11). §5 says what was tested and how.

## 1. Writing a plugin

```
notes/
  usdcc-plugin.json
  python/notes/__init__.py     imported, then activate(plugin) is called
  deps/                        optional: third-party Python modules it carries (§3.10)
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

Everything but `id`, `name`, `version` and `api` is optional, so a schema-only plugin has no code at all. `python.requires` are packages the app installs when asked, and `python.indexes` extra https package indexes they come from, shown before installing ([PYTHON_PACKAGES.md](PYTHON_PACKAGES.md) §4).

```python
# python/notes/__init__.py
import usdcc.app, usdcc.tools

def activate(plugin):
    usdcc.app.register_view_panel(plugin.id + ".panel", "Notes", NotesPanel)
    usdcc.app.add_menu_action("Tools/Add Note", add_note)
    usdcc.tools.register_tool(plugin.id + ".measure", "Measure", MeasureTool)

def deactivate(plugin):  # optional: at quit and before a reload, to stop what you started
    pass
```

**To write one inside the app,** see [PLUGIN_DEV.md](PLUGIN_DEV.md): **New Plugin...** in View > Plugins, a plugin's tab in the Script Editor, and one-click Reload.

[src/plugins/measure_example](../src/plugins/measure_example) is a working example of all of it. Try it with `usdcc --plugin-path src/plugins`. The C++ example builds to
`build/plugins`, so `usdcc --plugin-path build/plugins` loads that. [src/plugins/vray_render](../src/plugins/vray_render) is a bigger one: a view panel that renders
with V-Ray in a helper process, following the viewport camera and edits ([VRAY_RENDER.md](VRAY_RENDER.md)). The optional [native RTX plugin](../src/plugins/rtx_render_native) uses NVIDIA's C API and GPU presentation ([RTX_RENDER.md](RTX_RENDER.md)); it requests no Python packages. Old Python copies with its reserved ID are ignored so they cannot shadow the native plugin.

**Found in, the first with an id winning (logged):**
1. `--plugin-path` folders;
2. `USDCC_PLUGIN_PATH` (a studio's);
3. `%LOCALAPPDATA%/usdcc/plugins` (the user's);
4. the folders added in **Edit > Settings > Plugins > Plugin folders**, in their order;
5. `plugins/` next to `bin/`, the plugins shipped with usdcc: V-Ray Render and RTX Render.

The setting lists the user's folder and the shipped one as fixed rows, greyed, around the ones you add, remove and reorder. It takes effect at the next start, as plugins are found before the window opens. **Plugin Folders...** in View > Plugins opens it.

A plugin in your own folder wins over a shipped one with the same id; the other is ignored, and the log says so. `--no-plugins` starts clean when one breaks the app. A deploy puts the examples in `examples/plugins`, where they don't load unless asked for (`--plugin-path`, or added in the setting), and the SDK for C++ plugins in `sdk`.

**What a plugin can add:**

| Extension point | Call |
|---|---|
| View panel types | `usdcc.app.register_view_panel(id, name, cls)` |
| Render view panels | `usdcc.app.register_view_panel(id, name, cls)` with a `usdcc.ui.RenderViewPanel` subclass: the app's panel, with the plugin's renderer behind it ([RENDER_VIEW_PANEL.md](RENDER_VIEW_PANEL.md)) |
| Side panels | `usdcc.app.add_side_panel(panel, area, id=...)`. Give an `id`: otherwise the title is its identity in the saved layout |
| Main menu | `usdcc.app.add_menu_action("Menu/Action", callback, shortcut, tip)`, one undo step per run |
| Outliner's prim menu | `usdcc.app.add_prim_menu_action(label, callback, applies=None, id=None)`, one undo step per run |
| Viewport tools | `usdcc.tools.register_tool(id, label, cls, icon="")` |
| App events | `usdcc.app.connect(event, callback)`: `selection_changed`, `current_stage_changed`, `stage_opened`, `stage_closed`, `stage_edited`, `viewport_camera_changed` |
| The viewport's camera | `usdcc.app.active_viewport_camera()`: camera-to-world, field of view, aspect ratio, clipping |
| Create menu entries | `usdcc.usd.set_create_entry(...)` |
| Relationship target rules | `usdcc.usd.set_relationship_targets(...)` |
| Schemas, render delegates | A folder named in the manifest's `"usd"` |
| Settings | `plugin.settings.value(key, default)`, `set_value(key, value)`, JSON values, one group per plugin in `usdcc.ini` |
| Logging | `plugin.log`, a logger with the source `plugin:<id>` in the Log panel |

Attribute editors can't be added yet (§4, Later). **View > Plugins** lists the plugins, a chip for each of their parts (`C++`, `Py`, `USD`), what each added, and why any isn't running, and turns them on or
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

### 3.2 C++ plugins share the app's libraries

Every usdcc library but `usdcc_core` used to be static. A plugin linking those would compile and link, then fail quietly:

- **It would get its own copy of every singleton.** `PrimCreationCatalog::instance()` in a plugin would be a different catalog, so its entries would never
  appear, with no error.
- **It would get its own `ViewPanel`,** with its own Qt metaobject, so `qobject_cast<ViewPanel*>` in the app, which `savePanels()` relies on, would return null for
  its panels.

`_ui.pyd` had this problem already: it linked its own `usdcc_ui`.

**So:** `usdcc_usd`, `usdcc_tools`, `usdcc_ui` and `usdcc_usd_ui` are DLLs. The alternatives were the exe exporting its symbols, which `_usd.pyd` in a plain Python couldn't
link to, and a narrow C API, where panels couldn't derive from `ViewPanel`, and which wouldn't make the ABI stable as Qt and USD types cross it anyway. `usdcc_usd` and
`usdcc_tools` stay free of Qt, and `usdcc.usd` still imports in a plain Python. The scripting libraries stay static, in the exe.

**How they export, which differs from the plan.** The plan was to mark about 90 classes by hand, which misses the hundreds of free functions, such as `prim_actions::`. Instead:

- **CMake exports the functions** (`WINDOWS_EXPORT_ALL_SYMBOLS`). That's about 9,600 symbols, well within Windows' limit of 65,535 per DLL.
- **Data still needs marking,** and the only data is each `Q_OBJECT` class's `staticMetaObject`, so those 50 classes have `USDCC_UI_API` or `USDCC_USD_UI_API`.
- **Everything is exported, not a chosen API.** A plugin can use anything in the headers, so nothing marks what's meant to stay stable. That's the cost of this.

**Two traps found on the way:**

- **`WINDOWS_EXPORT_ALL_SYMBOLS` fails with AUTORCC** in a target under the Visual Studio generator, looking for `qrc_*.obj` in the wrong folder. So Qt resources are in small static
  libraries of their own (`usdcc_add_resources()`), which `Q_INIT_RESOURCE` pulls in.
- **A failed export step leaves an empty `exports.def`** that later builds don't replace, and the link then reports a few symbols missing. Delete it and rebuild if that happens
  after a failed build.
- **Copying DLLs after a link isn't enough:** changing a DLL often doesn't relink what uses it, so a stale copy can stay beside it. So `usdcc_deploy_core_library()` copies on every
  build. With the copy after linking, the tests loading a C++ plugin once crashed, and stopped when this changed, with the plugin and test program themselves untouched. That
  fits a stale DLL, but which one wasn't established.
- **Two Pythons in a Debug build:** USD's targets link `Python3::Python`, which in Debug was vcpkg's `python312_d.lib`. A symbol used only by a library later on the link line, such as
  pybind11's list conversion in `usdcc_usd_bindings`, resolved to it and ran in a Python that never started, so any binding taking a list stopped at an assert dialog, invisible under the offscreen
  tests, which looked like a hang. Python is now mapped to the release one in every configuration, like Qt and ADS, and in the SDK too. The link order suggests this predates the DLLs, but
  no Debug build from before them was run to confirm it.
- **`build/python/usdcc` is shared by every configuration,** so after a Debug build, `_ui.pyd` there is the Debug one until a Release build relinks it.

**A C++ plugin is tied to the exact build:** `std::` and Qt types cross the boundary, and USD's symbols carry its version (`pxrInternal_v0_26_11`). So `usdcc_add_plugin()`
writes the platform, compiler, Qt, USD and usdcc versions into the manifest's `built_for`, and the app compares them **before loading the DLL**, refusing with what differs.
Debug and Release needn't match, as every configuration uses the release CRT (PLAN.md §6 item 28).

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

**So:** enabling and disabling take effect **at the next start**, and the panel says so.

**A plugin's Python can be reloaded,** though, as nearly everything it registers can be taken back, and that's what developing one needs ([PLUGIN_DEV.md](PLUGIN_DEV.md)). It's for working on a plugin rather than for turning one off: what it did directly, such as connecting a Qt signal, only its `deactivate()` can end.

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
- **Compiled packages must match USD's Python,** currently 3.12 on Windows x64.
- **Some packages can take the app down:** `PySide6` from pip brings its own Qt (PLAN.md §6 item 11), and `usd-core` competes with `pxr`.
  Others may carry their own Qt or copies of libraries USD uses. `opencv-python`'s Windows wheel doesn't carry Qt (*checked,* and it runs in the app).

**So:**

1. **A plugin declares what it needs** in `python.requires`, and **the app installs it** into a folder shared by all Python code, when the user asks. [PYTHON_PACKAGES.md](PYTHON_PACKAGES.md)
   is that whole story. This is what a plugin should normally do.
2. **A plugin can instead carry it in `deps/`,** made with `pip install --target deps`, which keeps the `dist-info` folders the check reads. `deps/` isn't on `sys.path`:
   a finder at the end of `sys.meta_path` serves it, after the app's folders, so it can't replace what the app uses. This suits a plugin that must work with nothing installed.
3. **The app checks before running any plugin code.** Reading the package metadata of every enabled plugin, including what each bundled package itself
   needs, and importing nothing, it refuses:
   - **a conflict:** when plugins can't agree on a version, the later one, naming the other plugin and the package; or, when an installed copy is fixed, the plugins it doesn't suit;
   - **a missing package,** saying which, as **Needs packages**, since installing can fix it;
   - **a URL as a requirement:** only names, versions and markers are allowed;
   - **a protected name:** a plugin bundling `PySide6`, `shiboken6`, `pxr`, `usdcc`, `PySide2`, `PyQt5` or `PyQt6`. Requiring what the app provides is fine.
4. **One copy is used where several would do.** If two plugins bundle numpy and one version satisfies both, the finder serves the newest that does, to both.
   Only one could load anyway, and this makes the choice explicit rather than down to load order.

**The version check needs a parser** for requirement strings, which the standard library doesn't have. `packaging` 24.2 is vendored at
`usdcc._vendor.packaging`, rather than in `site-packages`, so that a plugin's own copy can't clash with it.

**Folders of packages on `sys.path`.** The shared folder the app installs into, the **Additional package folders** setting, and `USDCC_PYTHON_PATH`, which a studio can fill
with `pip install --target` or `uv pip install --target`, using a Python of the same version. **A copy found there is fixed:** a bundled one can't override it, and a plugin it doesn't
suit is told which version is installed.

**No installing at startup.** It would need the network at launch, and could change what other plugins depend on. An install is something the user asks for.

**Versions that truly can't coexist** have one real fix: the plugin runs that code in its own separate process and talks to it. That's left to its
author, and no helper is planned.

**Checked in the app:** numpy 2.5.4 in a plugin's `deps/`, next to USD and PySide, imports and computes. So do numpy, scipy 1.18 and opencv-python 5.0 from a folder uv filled.

## 4. Phases

| Phase | Builds | Status |
|---|---|---|
| **1. Python plugins** | `PluginManager`, manifests, order, USD folders, new startup order, `savePanels()` keeping unknown types, `--no-plugins`, `--plugin-path`, log source, third-party modules, example plugin | **Done** |
| **2. Extension points** | `add_menu_action()` with undo, prim menu entries, `plugin.settings`, events, side panel ids | **Done** |
| **3. Tools** | Tool ids and list, a rail built from it, Python `Tool`, example tool | **Done** |
| **4. Plugin Manager** | View > Plugins, enable and disable, Open Plugins Folder, crash marker | **Done** |
| **5. Libraries become DLLs** | `usdcc_usd`, `usdcc_tools`, `usdcc_ui`, `usdcc_usd_ui` shared, exported as in §3.2; `_ui.pyd`, `_usd.pyd`, tests and deploy updated | **Done** |
| **6. C++ plugins** | Loading, the `built_for` check, `usdcc::plugin::Host`, `usdcc_add_plugin()`, SDK in the deploy, example with a panel and tool, [PLUGIN_SDK.md](PLUGIN_SDK.md) | **Done** |

**Later:** plugin-provided attribute editors (`createPropertyEditor()` needs a registry first), plugin icons for prim types, Reload, an Install button
for Python modules (§3.10).

## 5. Tested, and not

**Unit tests:** [plugin_manager_test.cpp](../tests/plugin_manager_test.cpp), [plugin_extensions_test.cpp](../tests/plugin_extensions_test.cpp), including the
example plugin's panel, menu action, prim menu entry and tool, and [plugin_native_test.cpp](../tests/plugin_native_test.cpp), with a C++ plugin that has a `Q_OBJECT` panel and a DLL of its
own: it reaches the app's own catalog, the app's `qobject_cast` recognizes its panel, and each kind of mismatch or broken library says why.

**In the real app, with scratch plugins, and looked at in a screenshot:** the items marked checked above, and the Plugins panel, the tool rail's letter icons and the
options bar. A deployed folder ran the example plugin with its own Python. **The C++ example in the app:** its panel counts prims and follows an edit and its undo, and comes back
after restarts, and its menu action is one undo step.

**Outside this repository:** a plugin built from a copy of the example, against a deployed folder's `sdk`, runs in that deployed app with no development folders on `PATH`, next to the
C++ and Python examples. **This caught a real bug:** at first it failed to load, as CMake picked vcpkg's debug Qt without the app's mapping to the release one. The SDK now maps it.

**Not checked:**

- **A real mouse drag with a Python tool in the viewport,** and its gizmo actually drawn. The tests call the `Tool` methods as the viewport does.
- **Clicking through the panel by hand,** such as unticking a plugin with the mouse and restarting. The tests set the checkbox directly.
- **Render delegates and file formats from a plugin folder:** only a schema was tried (§3.1).
- **Compiled packages bringing their own USD libraries or Qt,** which the block list only stops by name (§3.10).
- **The Python tests in Debug: the hang seen here no longer reproduces.** When this was written, two of the 42 Python tests hung in Debug (`python_exit_criterion`, `python_app_api`):
  a binding taking a list, such as `usdcc.usd.select()`, hung in pybind11 before reaching the binding's code. Retried on 2026-11-05 against the `usd_typhoon` OpenUSD build,
  after the merge with the settings and toast work: all three suites pass in Debug (the Python one 45 tests), also with Debug tests loading Release-built `_ui.pyd`/`_usd.pyd`
  from the shared `build/python/usdcc`. That clone's OpenUSD wasn't available to compare, so the cause wasn't found. Two Debug build problems were fixed on the way: `/bigobj`
  for `value_type_info.cpp` (C1128 unoptimised), and TBB's debug mode against the release TBB (FindUSD.cmake). If it comes back, `tools/native_stacks.py <pid>` (dbghelp
  `StackWalk64` on each suspended thread) gives the native stacks of the hung process without a debugger.
- **Debug and Release share a plugin's output folder,** so the last one built is what's there. They're meant to be compatible, as both use the release CRT, but only the matching pairs were run.
- **A real mouse drag with a C++ tool,** as for Python's above. The example's Drop Locator tool has been loaded and selected but never driven. The test plugin's tool, much like it, was driven through the `Tool` methods.
- **A C++ plugin with Python or USD parts too.** The code activates the library, then the Python, but no test has both.
- **`inline` state across DLLs:** a search of the headers found no function-local statics or mutable globals, but it was a text search.
- **Linux and macOS:** only Windows has been run.

## 6. Decisions

1. **Python first, C++ after:** done in that order.
2. **How C++ plugins link:** the libraries are DLLs (§3.2), the most work of the options, and the only one giving a plugin the same classes and fixing `_ui.pyd`.
3. **C++ plugins at all:** built, as the spec asks. Most plugins should still be Python (PLUGIN_SDK.md).
4. **Turning a plugin on or off:** at the next start (§3.4).
5. **A newly found plugin:** it just loads, as startup scripts do. Someone had to put it in that folder.
6. **Installing:** copying a folder, with Open Plugins Folder in the panel. No zip installer or store for now.
7. **Manifest format:** JSON. Qt reads it with no new dependency, and USD's `plugInfo.json` is the same kind of file.
8. **Compatibility:** `api` is a whole number, raised only when something breaks, and a plugin whose `api` doesn't match is refused with the reason. C++ also needs the exact build (§3.2).
9. **Third-party Python modules:** declared in the manifest, installed by the app when asked into a shared folder ([PYTHON_PACKAGES.md](PYTHON_PACKAGES.md)) or carried in `deps/`, and checked before any plugin code runs (§3.10). Nothing is installed at startup.
