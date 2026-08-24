# usdcc — Project Plan

*Living document — keep this updated as decisions are made and milestones complete. Last updated: 2026-08-24.*

## 1. Overview

Cross-platform C++20 digital content creation (DCC) tool that manages multiple scenes
via OpenUSD stages and displays them through Hydra viewports.

## 2. Technical Requirements

- C++20
- Scripting via Python, both embedded in the app and for plugins
- UI: Qt6
- Python bindings for UI (Qt) use Shiboken6
- Python bindings for OpenUSD data use `pxr_boost::python` (bundled with OpenUSD)
- Python bindings for other C++ classes use pybind11
- vcpkg as package manager, added as a git submodule at `external/vcpkg`
- CMake build system
- Plugin system supporting both C++ and Python extensions
- Full undo/redo support

## 3. Repository Layout

```
docs/
  PLAN.md
  changelog.md
CMakeLists.txt
external/vcpkg/
src/
  cpp/
    core/
    ui/
    usd/
    tools/
  python/
    (mirrors src/cpp structure: usdcc.core, usdcc.ui, usdcc.usd, usdcc.tools)
  plugins/
readme.md
```

## 4. Architecture Guidelines

- Python modules mirror the C++ namespace/folder structure: `usdcc.core`, `usdcc.ui`, `usdcc.usd`, `usdcc.tools`.
- The app is a `QMainWindow` using Qt Advanced Docking System (ADS) for its central widget.
- **SidePanel** — base class for panels docked at the edges of the `QMainWindow`, *outside* the ADS docking area. Exposed to Python.
  - `LogSidePanel` — application logs with filter/search.
  - `ScriptingSidePanel` — Python script editor with syntax highlighting and load/save.
  - `USDASidePanel` — shows/edits a selected layer as USDA text; edits are applied explicitly (not live) via a button.
- **ViewPanel** — base class for panels living inside the ADS docking area. Exposed to Python, subclassable from both Python and C++. Every `ViewPanel` can switch between currently loaded USD stages via a dropdown.
  - `ViewportViewPanel` — Hydra viewport; 3D navigation; switches between available Hydra render delegates.
  - `OutlinerViewPanel` — tree view of the USD stage; rename, disable, select, reorder prims.
  - `AttributesViewPanel` — shows/edits USD attributes of the selected prim, supporting all USD attribute types.
- **Tools** — operate on gizmos in the viewport and modify the selected prims.
  - `SelectTool`, `MoveTool` (translate gizmo: axis/plane/free), `RotateTool`, `ScaleTool`.
- **Dialogs**
  - Settings dialog for persistent application settings.
  - Info dialog listing third-party libraries (name, version, license) and all registered USD extensions.
- Build: `USD_INSTALL` (or similar) must be a CMake parameter so the build can target a custom OpenUSD build.
- A `deploy` CMake target must produce a self-contained, launchable package folder, including the required files from the OpenUSD build tree.

## 5. Open Questions & Assumptions

These aren't blocking, but each has a real cost if decided implicitly. Flagging them
so a deliberate choice gets made instead of an accidental one.

| # | Question | Assumption used for planning below |
|---|----------|-------------------------------------|
| 1 | Target OS/platforms (Windows only? + Linux? + macOS?) | Windows + Linux, macOS best-effort |
| 2 | Minimum/pinned versions: OpenUSD, Qt6, Python, C++ compiler | Pin exact versions at M0 once vcpkg baseline is chosen |
| 3 | Project license, and how third-party license compliance is tracked for the Info dialog | TBD — needed before first public distribution |
| 4 | Plugin manifest/discovery format, versioning & compatibility policy, sandboxing for Python plugins | Design during M9 |
| 5 | Threading model: does stage loading / Hydra render run off the UI thread? | Assume async loading + render thread; confirm during M2/M3 |
| 6 | Undo granularity: is a single `QUndoStack` shared across all stages, or one per stage/session? | Assume one stack per open stage/session; confirm during M6 |
| 7 | Scope of "full undo support" — does it cover script-driven edits from the Scripting panel and plugin actions, or only UI-driven edits? | Assume it must cover all mutation paths (UI, scripting, plugins) |
| 8 | Multi-stage model: are stages fully independent documents, or can they reference/compose each other in one session? | Assume independent documents, each with its own `pxr.Usd.Stage` and undo stack |
| 9 | Import/export of non-USD formats (FBX, Alembic, glTF, etc.) — in scope at all? | Out of scope until explicitly requested |

## 6. Suggested Improvements

Beyond fixing the typos in the original draft (done above), the spec is missing several
things that will bite later if not addressed early:

1. **Testing strategy** — no mention of unit/integration tests. Recommend GoogleTest for
   C++ and `pytest` for Python bindings, plus a smoke test that opens a stage headlessly
   in CI. Add this as its own workstream (see §8) rather than bolting it on per-milestone.
2. **CI/CD** — cross-platform build matrix (at minimum Windows + Linux), vcpkg cache,
   and artifact upload of the `deploy` target output. Needed as soon as M0 lands so
   later milestones don't regress silently.
3. **Coding standards** — a `.clang-format`/`.clang-tidy` config and a short C++/Python
   style guide (naming conventions for the `usdcc.*` mirror, header layout, error handling
   convention — exceptions vs. status codes at the C++/Python boundary).
4. **Plugin API contract** — the spec says "plugin system for C++ and Python extensions"
   but not the discovery mechanism, manifest schema, lifecycle hooks, or ABI/version
   compatibility policy. This is a project in itself and should be designed explicitly
   (M9) rather than emerging ad hoc from whatever the first plugin needs.
5. **Undo architecture** — recommend basing it on USD's own change-notification system
   (`Tf` notices / `SdfLayer` change blocks) feeding into Qt's `QUndoStack`/`QUndoCommand`,
   so edits from any source (UI, scripting, plugins) produce commands uniformly instead
   of each panel inventing its own undo path.
6. **Stage/session management** — introduce an explicit `StageManager` (or `Session`)
   concept now, since almost every panel depends on "the set of currently loaded stages."
   Otherwise each `ViewPanel` subclass reinvents stage tracking.
7. **Logging framework** — `LogSidePanel` implies a structured logging backend (levels,
   categories, filtering) shared by C++ and Python. Worth picking one early
   (e.g. `spdlog` + a Python logging bridge) since every subsystem will emit into it.
8. **Performance targets** — no stated scale (prim count, stage file size) the viewport
   and outliner need to handle smoothly. Worth setting a rough target before M3/M4 so
   render-delegate and outliner-model choices aren't revisited later.
9. **Crash/error handling policy** — should a plugin crash or a bad script take down the
   whole app? Recommend isolating Python plugin execution enough that scripting errors
   surface in `LogSidePanel` without corrupting undo state or crashing the process.
10. **Docs beyond the plan** — the layout lists `docs/changelog.md` but nothing for
    architecture notes or a plugin-author guide. Recommend adding
    `docs/ARCHITECTURE.md` and `docs/PLUGIN_SDK.md` once M1/M9 stabilize their shapes.

## 7. Milestones

Each milestone lists its goal, key deliverables, and exit criteria. Milestones are
mostly sequential but M6 (undo) and the cross-cutting workstreams in §8 run alongside
several of them rather than as a discrete phase.

### M0 — Repository & Build Bootstrap
- **Goal:** a clean checkout can configure, build, and produce an empty app window.
- Deliverables: repo layout per §3, `external/vcpkg` submodule, root `CMakeLists.txt`
  with `USD_INSTALL` parameter, vcpkg manifest with pinned Qt6/pybind11/OpenUSD deps,
  `readme.md` with build instructions, CI skeleton (configure+build on Windows & Linux).
- Exit criteria: fresh clone builds on both target OSes and launches an empty `QMainWindow`.

### M1 — Core Application Shell
- **Goal:** the docking framework and panel base classes exist and are exposed to Python.
- Deliverables: `QMainWindow` + Qt Advanced Docking System integration; `SidePanel` and
  `ViewPanel` base classes in C++ with Shiboken6 bindings; panel registration/layout
  persistence (save/restore docking state).
- Exit criteria: a trivial `ViewPanel` and `SidePanel` can each be created from both
  C++ and Python and docked/undocked interactively.

### M2 — OpenUSD Integration & Stage Management
- **Goal:** the app can load, hold, and enumerate multiple USD stages.
- Deliverables: `StageManager`/session concept (see §6.6); `pxr_boost::python` bindings
  wired into the embedded interpreter; stage open/close/reload; the "current stage"
  dropdown contract that all `ViewPanel`s will consume.
- Exit criteria: multiple stages can be opened simultaneously and enumerated from both
  C++ and Python.

### M3 — Hydra Viewport
- **Goal:** `ViewportViewPanel` renders a stage and lets the user navigate it.
- Deliverables: Hydra render index wired to the panel, render-delegate enumeration and
  switching, camera navigation (orbit/pan/zoom), per-panel stage selection.
- Exit criteria: any loaded stage can be viewed and navigated in at least two different
  render delegates.

### M4 — Scene Introspection Panels
- **Goal:** users can see and edit stage structure and prim attributes.
- Deliverables: `OutlinerViewPanel` (tree view: rename/disable/select/reorder prims);
  `AttributesViewPanel` (view/edit all USD attribute types for the selected prim);
  selection state shared across panels for the active stage.
- Exit criteria: a prim renamed/reordered in the outliner and an attribute edited in
  the attributes panel are both reflected live in the viewport.

### M5 — Editing Tools & Gizmos
- **Goal:** viewport-driven transform editing.
- Deliverables: `SelectTool` (click-to-select), `MoveTool`, `RotateTool`, `ScaleTool`
  with standard 3D gizmos; tool switching UI.
- Exit criteria: selected prims can be translated/rotated/scaled via gizmo manipulation
  and the resulting USD transform ops are correct.

### M6 — Undo/Redo Framework *(cross-cutting; starts alongside M2, hardens through M5)*
- **Goal:** every mutation path (UI, scripting, plugins) is undoable.
- Deliverables: command layer bridging USD change notices to `QUndoStack`; per-
  stage/session undo stacks (pending §5 item 6); undo coverage for stage edits from
  the outliner, attributes panel, gizmo tools, and the USDA panel's "apply" action.
- Exit criteria: every edit surface introduced by M2–M5 is undoable/redoable without
  corrupting stage state.

### M7 — Scripting & Python Extensibility
- **Goal:** users can script the running application.
- Deliverables: `ScriptingSidePanel` (editor, syntax highlighting, load/save, run);
  embedded interpreter exposing `usdcc.core`/`usdcc.ui`/`usdcc.usd`/`usdcc.tools`;
  pybind11 bindings for the non-UI, non-USD C++ classes; error surfacing into logging
  (§6.7) instead of crashing the app.
- Exit criteria: a script run from the panel can open a stage, edit a prim, and drive
  the undo stack identically to a UI-driven edit.

### M8 — Layer Editing & Logging
- **Goal:** low-level layer inspection/editing and centralized diagnostics.
- Deliverables: `USDASidePanel` (USDA text view/edit with explicit apply);
  `LogSidePanel` (filter/search) backed by the shared logging framework (§6.7).
- Exit criteria: a manual USDA edit applied through the panel round-trips correctly
  and is undoable; log messages from C++, Python, and plugins all appear in one place.

### M9 — Plugin System
- **Goal:** third parties can extend the app in C++ or Python without forking it.
- Deliverables: plugin manifest schema, discovery/loading mechanism, lifecycle hooks
  (load/unload, register panels/tools), versioning/compatibility policy, a minimal
  example plugin in each language, `docs/PLUGIN_SDK.md`.
- Exit criteria: the example C++ plugin and example Python plugin each register a
  panel and a tool that behave identically to built-in ones.

### M10 — Dialogs & Settings
- **Goal:** persistent app configuration and license/extension transparency.
- Deliverables: Settings dialog (persisted app settings); Info dialog listing
  third-party libraries with version/license and all registered USD extensions
  (feeds off the plugin registry from M9).
- Exit criteria: settings persist across restarts; Info dialog accurately reflects
  installed third-party components and currently registered extensions.

### M11 — Packaging & Deployment
- **Goal:** a one-command deployable build.
- Deliverables: `deploy` CMake target producing a launchable package folder,
  including required OpenUSD build-tree files; per-OS packaging (installer or
  archive); CI artifact publishing.
- Exit criteria: the deploy output runs standalone on a clean machine (no dev
  environment) on each target OS.

### M12 — Hardening & Release Readiness
- **Goal:** production-quality baseline.
- Deliverables: test coverage across the workstreams in §8, performance pass against
  the target scale (§6.8), crash/error isolation for plugins and scripts (§6.9),
  `docs/ARCHITECTURE.md`, changelog populated, license/compliance finalized (§5 item 3).
- Exit criteria: defined performance targets met; no known crash-on-plugin-error paths;
  documentation set complete.

## 8. Cross-Cutting Workstreams

These run throughout, not as discrete milestones:

- **Testing:** GoogleTest (C++), `pytest` (Python bindings), headless stage-load smoke
  test in CI, gizmo/undo regression tests as M5/M6 land.
- **CI/CD:** build matrix per §6.2, vcpkg dependency caching, deploy-artifact publishing
  once M11 exists.
- **Documentation:** `docs/changelog.md` kept current per change; `docs/ARCHITECTURE.md`
  and `docs/PLUGIN_SDK.md` added once M1/M9 stabilize.
- **Coding standards:** `.clang-format`/`.clang-tidy`, Python style (PEP 8 + the
  `usdcc.*` mirroring convention), error-handling convention at the C++/Python boundary.

## 9. Status

| Milestone | Status |
|-----------|--------|
| M0 Repository & Build Bootstrap | Not started |
| M1 Core Application Shell | Not started |
| M2 OpenUSD Integration & Stage Management | Not started |
| M3 Hydra Viewport | Not started |
| M4 Scene Introspection Panels | Not started |
| M5 Editing Tools & Gizmos | Not started |
| M6 Undo/Redo Framework | Not started |
| M7 Scripting & Python Extensibility | Not started |
| M8 Layer Editing & Logging | Not started |
| M9 Plugin System | Not started |
| M10 Dialogs & Settings | Not started |
| M11 Packaging & Deployment | Not started |
| M12 Hardening & Release Readiness | Not started |
