# Writing a C++ Plugin

A C++ plugin is a library the app loads at startup, sharing the app's own classes and singletons. [PLUGINS.md](PLUGINS.md) covers plugins in general, and
most plugins are better off in Python: it has no build, and doesn't have to match the app's exactly. Use C++ for code that needs to be fast, or that has to derive
from a C++ class such as `ViewPanel`.

[src/plugins/cpp_example](../src/plugins/cpp_example) is a working example.

## 1. The code

```cpp
#include "usdcc/plugin/plugin.h"

USDCC_PLUGIN_EXPORT void usdcc_plugin_activate(usdcc::plugin::Host& host) {
    host.addViewPanel("com.example.notes.panel", "Notes", [&host](const auto& stage, const QString& title, QWidget* parent)
                          -> usdcc::ui::ViewPanel* { return new NotesPanel(host.stageManager, stage, title, parent); });
    host.addTool(host.id + ".measure", "Measure", "", std::make_unique<MeasureTool>());
    host.addMenuAction("Tools/Add Note", [] { /* one undo step */ });
}

// Optional, at quit.
USDCC_PLUGIN_EXPORT void usdcc_plugin_deactivate(usdcc::plugin::Host& host) {}
```

| `Host` | |
|---|---|
| `id`, `name`, `version`, `folder` | From the manifest |
| `stageManager`, `undoManager`, `toolSet`, `mainWindow`, `viewPanels` | The app's own objects |
| `addViewPanel(id, name, factory)` | A view panel type, in the Stages menu, which comes back after a restart |
| `addSidePanel(panel, area)` | Set a stable object name on it: that's its identity in the saved layout |
| `addTool(id, label, icon, tool)` | `icon` is an image's path; empty, the rail shows the label's first letter |
| `addMenuAction(path, run, shortcut, tip)` | "Menu/Action". `run` is one undo step, and an exception is logged |
| `addPrimMenuAction(label, run, applies)` | At the end of the Outliner's prim menu, one undo step |
| `settingKey(key)` | A key of the plugin's own, for `usdcc::ui::appSettings()` |
| `log(level, message)` | A record with the source `plugin:<id>` in the Log panel |

These also tell the Plugin Manager what the plugin added. Anything else in the app's headers can be used directly, such as
`usdcc::usd::PrimCreationCatalog::instance()` or `stageManager->selectionChanged`: there's one copy of each, shared with the app.

An exception from `usdcc_plugin_activate()` marks the plugin failed. What it registered by then stays until the next start, and its library is never unloaded, so
keep `Host` references and callbacks as long as you like.

## 2. Building it

Within this repository, add a folder and `add_subdirectory()` it. Elsewhere, build against a deployed app's `sdk` folder:

```cmake
cmake_minimum_required(VERSION 3.21)
project(notes LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(UsdccPlugin REQUIRED)

usdcc_add_plugin(notes
    ID com.example.notes
    NAME "Notes"
    VERSION 1.0.0
    DESCRIPTION "Notes on prims"
    SOURCES notes.cpp)
```

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DUsdccPlugin_DIR=<deploy>/sdk -DUSD_INSTALL=<path> -DCMAKE_PREFIX_PATH=<the app's build>/vcpkg_installed/x64-windows
```

```bash
cmake --build build --config RelWithDebInfo
```

The SDK holds usdcc's headers and import libraries. **Qt, Qt Advanced Docking System and OpenUSD aren't in it,** so you need the ones the app was built with, which
`find_package(UsdccPlugin)` checks, along with the compiler, stopping with a message if they differ.

**OpenUSD** comes prebuilt from the repository's releases: the `deps/usd-…` prereleases (not usdcc releases), one per OpenUSD build that
[deps/usd.json](../deps/usd.json) has pinned, made by CI ([CI.md](CI.md)). Take the one whose name `tools/ci/usd-asset.ps1` prints for the usdcc
version you build against, unzip it anywhere, and pass that folder as `-DUSD_INSTALL`. It has OpenUSD's headers, import libraries, DLLs, plugins and
CMake files, plus those of its dependencies (TBB, MaterialX, OpenImageIO, OpenColorIO, Embree, ...). It's a Release build, so a plugin uses the release
CRT (`/MD`) with `_ITERATOR_DEBUG_LEVEL=0` in every configuration, as usdcc does; `find_package(UsdccPlugin)` sets that up. It also needs Python 3.12 x64
installed, which CMake finds by itself. A plain OpenUSD plugin, without usdcc, builds against it the same way: see `tools/ci/usd-sdk-check`, which CI
builds against every new OpenUSD asset.

The plugin lands in `build/plugins/notes`, with its manifest. Copy that folder to `%LOCALAPPDATA%/usdcc/plugins`, or try it with `usdcc --plugin-path build/plugins`.

`usdcc_add_plugin()` also takes `OUTPUT_DIR`, `PYTHON_MODULE`, `USD_FOLDERS` and `REQUIRES`. A plugin can have Python and USD parts too: the library is activated first,
and you copy `python/` and `usd/` into the plugin's folder yourself.

**A DLL of the plugin's own** goes next to the plugin's library, in `bin/windows-x64`.

## 3. It has to match the app exactly

`std::` and Qt types cross the boundary, and USD's symbols carry its version, so a plugin only works with the build it was made for. The manifest's `built_for`, written
by `usdcc_add_plugin()`, says what that is:

```json
"native": {
  "library": "notes",
  "built_for": { "platform": "windows-x64", "compiler": "MSVC 19.44.35229.0", "qt": "6.11.1", "usd": "2611", "usdcc": "0.1.0" }
}
```

The app compares these before loading the library, and a mismatch is a refusal naming what differs, rather than a crash. So **a new compiler, Qt, OpenUSD or usdcc
version means rebuilding your plugin.**

**This check isn't everything.** usdcc's own version has to be raised when its headers change for it to catch that, and it doesn't check your compile flags. Build with
`usdcc_add_plugin()`, which sets those as the app does: the release CRT and `_ITERATOR_DEBUG_LEVEL=0` in every configuration, and `QT_NO_KEYWORDS`, so `Q_EMIT` rather than
`emit`. On MSVC it also makes sure `extern "C"` functions may throw (`/EHc-`, or usdcc's own `/EHs`): the entry points are `extern "C"`, and under the default `/EHsc` an
exception from `usdcc_plugin_activate()` is undefined behaviour (warning C4297) rather than a failed plugin. Replacing `/EHsc` with `/EHs` in your `CMAKE_CXX_FLAGS` avoids
cl's "overriding /EHc" note.
