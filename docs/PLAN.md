# usdcc — Project Plan

*Living document — keep this updated as decisions are made and milestones complete. Last updated: 2026-09-05.*

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
test_data/
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
| 2 | Minimum/pinned versions: OpenUSD, Qt6, Python, C++ compiler | Qt 6.11.1 (vcpkg baseline), Python 3.12, MSVC 2022 pinned via M0; OpenUSD 26.8 now in use (a from-source build, not vcpkg's Python-less `usd` port — see §6 item 11) |
| 3 | Project license, and how third-party license compliance is tracked for the Info dialog | TBD — needed before first public distribution |
| 4 | Plugin manifest/discovery format, versioning & compatibility policy, sandboxing for Python plugins | Design during M9 |
| 5 | Threading model: does stage loading / Hydra render run off the UI thread? | Assume async loading + render thread; confirm during M2/M3 |
| 6 | Undo granularity: is a single `QUndoStack` shared across all stages, or one per stage/session? | Assume one stack per open stage/session; confirm during M6 |
| 7 | Scope of "full undo support" — does it cover script-driven edits from the Scripting panel and plugin actions, or only UI-driven edits? | Assume it must cover all mutation paths (UI, scripting, plugins) |
| 8 | Multi-stage model: are stages fully independent documents, or can they reference/compose each other in one session? | Assume independent documents, each with its own `pxr.Usd.Stage` and undo stack |
| 9 | Import/export of non-USD formats (FBX, Alembic, glTF, etc.) — in scope at all? | Out of scope until explicitly requested |
| 10 | Where does the app's PySide6/Shiboken6 come from — a from-source build against vcpkg's own Qt (required today, see §6 item 11) or something else once M11 packaging needs a repeatable, CI-friendly answer? | Assume the from-source build (`tools/build-pyside.ps1`) stays the approach; revisit if it proves too slow/fragile for CI |
| 11 | **Resolved (M2).** `StageManager` (bound via pybind11) needs to hand out/accept the same `UsdStage` objects OpenUSD's own `pxr_boost::python` bindings produce. A custom pybind11 `type_caster<UsdStageRefPtr>` bridging via each framework's `PyObject*`/`.ptr()`/`extract<>()` escape hatch was built and round-tripped a stage's *identity* correctly, but calling unrelated `pxr_boost::python` APIs afterward (e.g. `stage.GetPrimAtPath` with a `Sdf.Path` argument) started failing argument-matching — confirmed via a minimal repro, not a fluke. USD's boost::python fork lives under `pxrInternal_v..._pxrReserved__::pxr_boost`: explicitly internal/reserved, not designed for cross-DLL use by third-party extensions. | Resolved differently: every stage opened through `StageManager` is registered in `UsdUtilsStageCache::Get()` (USD's own shared, process-wide cache, designed for exactly this cross-context handoff); only the plain-integer cache id crosses the pybind11 boundary, and Python retrieves the real stage via `UsdUtils.StageCache.Get().Find(Usd.StageCache.Id.FromLongInt(id))` — USD's own, already-correct bindings. No custom caster needed or used in the shipped code. |

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
11. **PySide6/Shiboken6 must be built from source against vcpkg's Qt** — discovered while
    wiring up M1's Python bindings: a pip-installed PySide6 ships its own, independently
    compiled Qt binaries, and loading those alongside vcpkg's Qt in one process causes
    intermittent `DLL load failed` crashes (Windows resolves a same-named DLL to whichever
    copy loaded first, not necessarily the one usdcc's C++ was compiled against). There's
    no reliable way to bind vcpkg-Qt-derived classes for pip-PySide6 as a result.
    `tools/build-pyside.ps1` builds PySide6/Shiboken6 from source against the same vcpkg
    Qt; this needs to become a real dependency step (M0/CI, and M11 packaging) rather than
    a one-off local workaround. See §5 item 10.
12. **Resolved: Hydra-in-Qt viewport embedding had two separate bugs — geometric pixel
    corruption (fixed by switching embedding approach) and all-black shading (fixed by an
    explicit `glClear()`).** Discovered while building M3's `HydraViewportWidget` (a
    `QOpenGLWidget`): camera/projection math, render-viewport sizing, and render-delegate
    enumeration/switching were all confirmed correct (pixel-perfect, camera-tracking
    silhouettes; both `HdStormRendererPlugin` and `HdEmbreeRendererPlugin` enumerate and
    switch cleanly) — but the shaded pixels inside those silhouettes were corrupted: a fine
    colored-static pattern, identical between Storm and Embree, unaffected by
    `SetRenderBufferSize`, `GL_POLYGON_STIPPLE`/`GL_STENCIL_TEST` state, or waiting for
    convergence.
    **Fix #1, per the user's direction to try an alternative embedding**: `QOpenGLWidget`
    renders into an offscreen FBO that Qt then composites into the widget backing store —
    that indirection was corrupting Hydra's output. Rebuilding the viewport as
    `HydraViewportWindow : public QWindow` (its own `QOpenGLContext`, calling
    `swapBuffers()` directly, embedded into the widget tree via
    `QWidget::createWindowContainer()`) made the corruption disappear completely. One
    necessary fix found along the way: `QSurfaceFormat` must request `CompatibilityProfile`,
    not `CoreProfile` — Storm's `HgiGL_ScopedStateHolder` queries/pushes legacy GL state
    that doesn't exist in Core and raises `GL error: invalid enum` there.
    **Fix #2**: after the `QWindow` switch, shaded geometry rendered solid black — even with
    lighting disabled and even with a hard `overrideColor`, ruling out lighting/material
    specifically. Root cause found by reading `HgiInteropOpenGL::CompositeToInterop`'s
    actual source (`pxr/imaging/hgiInterop/opengl.cpp`): it composites Hydra's rendered
    color+depth "over the application's framebuffer contents" using premultiplied-alpha
    blending plus a `GL_LEQUAL` depth test against *our* depth buffer — i.e. it assumes the
    caller already cleared color and depth before calling `Render()`. `HydraViewportWindow`
    never did (a raw `QWindow`+`QOpenGLContext`, unlike `QOpenGLWidget`, has no framework
    doing this automatically). An explicit `glClear(GL_COLOR_BUFFER_BIT |
    GL_DEPTH_BUFFER_BIT)` before `Render()` on every frame fixed it completely — confirmed
    both by the user directly (live screenshots of both Storm and Embree showing the correct
    red cube/blue sphere with proper shading) and independently on this end.
    **What this also explains**: earlier in this investigation, automated screenshots of
    the same code/scene sometimes showed correct colors and sometimes solid black, which
    was chalked up to "unreliable capture tooling" at the time. In hindsight this was very
    likely the real, uncleared-depth-buffer bug manifesting inconsistently (its outcome
    depends on whatever undefined memory the depth buffer happened to contain from a
    previous frame/process) rather than a capture artifact — worth remembering: intermittent
    results are more often a real race/uninitialized-state bug than a broken test harness.
    **Loose end**: `HgiInteropOpenGL`/`HgiGLTexture` GL errors ("invalid operation",
    `glGetError()` verification failures) still appear in the log at every frame/teardown
    even though rendering is now visibly correct — cosmetic-only so far (no visible
    artifacts, no crashes across extended runs), but worth investigating if it ever proves
    otherwise. See §9 status.
13. **`MainWindow`'s saved dock layout must only be restored after every panel for the
    session exists.** Surfaced when M3 added `ViewportViewPanel` in `main.cpp` *after*
    `MainWindow`'s constructor had already run — and that constructor called
    `restoreLayout()` on itself, before Viewport existed. Applying a saved ADS state
    (captured in a *previous* run, once Viewport existed) against a dock manager that
    currently only knows about the M1 stand-ins left ADS's internal layout state
    inconsistent, and the Viewport panel added moments later simply didn't appear at all
    (not corrupted-looking — entirely absent from the UI) until the registry key
    (`HKCU\Software\usdcc\usdcc`) was cleared by hand. Fixed by moving `restoreLayout()` out
    of the constructor into a public method the composition root calls once, after every
    panel (stand-ins and USD-dependent ones alike) has been added — confirmed fixed across
    repeated runs with no manual reset needed. Relevant again for M4 (`OutlinerViewPanel`,
    `AttributesViewPanel`): any future panel added after `MainWindow` is constructed needs
    to exist before `restoreLayout()` is called, same as `ViewportViewPanel`.
    Also switched saved-layout storage from `QSettings`' native format (the Windows
    registry) to a plain INI file at `%LOCALAPPDATA%\usdcc\usdcc.ini`
    (`QStandardPaths::AppConfigLocation`), so a stale/incompatible saved layout — a routine
    occurrence during active development, as above — can be found and deleted by hand
    without hunting through regedit.
14. **M4 design decisions.** Selection is shared per-*stage* rather than globally, since a
    stage can be open in several panels/viewports at once (the established M3 pattern):
    `StageManager` keys a `std::map<long /*stage cache id*/, std::vector<SdfPath>>` and
    emits `selectionChanged(stage, paths)`; each panel's tree/table selection syncs
    bidirectionally against it, guarded by a `bool m_updatingSelection` flag to prevent
    signal feedback loops. `OutlinerViewPanel` is a custom `UsdPrimTreeModel`
    (`QAbstractItemModel`) built with `UsdPrim::GetFilteredChildren(UsdPrimAllPrimsPredicate)`
    so inactive prims are shown (grayed out, italic) and toggleable rather than hidden;
    renaming goes through `pxr::UsdNamespaceEditor` (`RenamePrim` + `ApplyEdits`), which
    handles relationship/connection path fixups automatically; reordering authors a
    "reorder nameChildren" opinion via `SdfPrimSpec::SetNameChildrenOrder` on the parent's
    edit-target spec (creating one via `UsdStage::OverridePrim` first if the parent has none
    there yet) — this is the composition-aware way to reorder siblings, as opposed to moving
    specs between layers. Both the rename and the checkbox-driven active/inactive toggle
    call the model's full `beginResetModel()/endResetModel()` rather than incremental
    updates (simpler given USD's own composition machinery does the real work) — the
    trade-off is that this invalidates every previously-held `QModelIndex`, which must be
    re-fetched afterward. `AttributesViewPanel` dispatches attribute get/set by the
    attribute's underlying C++ type via `pxr::TfType::Find<T>()` compared against
    `attr.GetTypeName().GetType()` (so role variants like `Color3f`/`Vector3f`/`Point3f`,
    which all share the `GfVec3f` C++ representation, are handled uniformly); `Gf` vector
    types support `operator<<` but not `operator>>`, so parsing typed-in text uses a small
    manual `parseNumberList()` helper instead of stream extraction. Known gap: array,
    matrix, and asset-path attributes aren't covered by the dispatcher yet and display
    read-only. All three new panels live in the `usdcc_usd_ui` CMake target (renamed from
    `usdcc_usd_viewport` now that it hosts more than just the viewport panel).
15. **M5 design decisions.** `usdcc::tools::Tool` (base for `SelectTool`/`MoveTool`/
    `RotateTool`/`ScaleTool`) lives in a new `usdcc_tools` static library kept deliberately
    free of Qt/GL: a `GizmoGeometry` struct (world-space line/triangle lists) is the only
    output a Tool produces for drawing, and a `ToolContext` (stage, selection, camera
    matrices, a `pickPrim` callback) is the only input it needs — `HydraViewportWindow`
    owns one instance of each tool, is the only place that touches GL or USD's picking API,
    and turns `GizmoGeometry` into actual draw calls. Click-to-select uses Hydra's own GPU
    picking (`UsdImagingGLEngine::TestIntersection` with a `GfFrustum::ComputeNarrowedFrustum`
    around the click point, resolve mode `HdxPickResolveModeTokens->resolveNearestToCamera`)
    rather than a manual ray-cast — the same mechanism `usdview` itself uses. Gizmo geometry
    is drawn as legacy (fixed-function) immediate-mode GL (`QOpenGLFunctions_1_1`, obtained
    via Qt6's `QOpenGLVersionFunctionsFactory::get<T>()` — Qt5's `QOpenGLContext::
    versionFunctions<T>()` template method no longer exists in Qt6) directly into the same
    already-Storm-composited framebuffer, with depth testing disabled so handles stay
    grabbable from behind geometry; this piggybacks on the `CompatibilityProfile` context M3
    already required for Storm, so it adds no new platform constraint. Handle hit-testing is
    screen-space (project each handle's world-space geometry to NDC via the same
    `viewMatrix * projMatrix` used for rendering, then measure 2D distance to the click point)
    rather than full 3D ray-vs-primitive math — simpler and robust, at the cost of the
    picked handle being whichever screen-space geometry is nearest even if another handle is
    genuinely closer in 3D (not visually distinguishable to the user in practice). Axis- and
    plane-constrained dragging use `GfFindClosestPoints(ray, GfLine)` and `GfRay::Intersect
    (GfPlane)` respectively — both are ready-made USD `Gf` utilities, no custom geometry math
    needed. `TransformGizmoTool::getOrCreateOp()` re-sorts a prim's xformOps into canonical
    translate/rotate/scale order whenever it has to add a new one, but only when every
    existing op is one of those three known types — a stack already containing a matrix or
    orient op is left alone rather than risking corrupting an ordering this code doesn't
    understand. Known gap: `RotateTool` drives the prim's own `rotateXYZ` Euler components
    directly from its object-space rings (see §6 item 17), which is exact for a single-axis
    rotation (the common case) and only an approximation once more than one Euler component
    is non-zero, since doing this exactly would mean composing/decomposing a `GfRotation`
    through the op's existing value on every drag step. Multi-prim gizmo editing is also out
    of scope —
    with more than one prim selected, the gizmo tools draw nothing and don't handle mouse
    events. Gizmo-driven edits mutate the stage directly rather than going through an undo
    command, same as the rename/reorder/attribute-edit paths M4 already shipped without one —
    M6 (Undo/Redo) is the milestone that has to retrofit all of these uniformly.
16. **Fixed: the viewport's "Stage:" combo didn't actually switch stages once more than one
    was open.** `UsdImagingGLEngine` only populates its scene index/delegate from a stage
    once per engine instance (an internal `_isPopulated` flag in
    `usdImaging/usdImagingGL/engine.cpp`, set on the first `Render()`/`PrepareBatch()` call
    and never cleared again except by `SetRendererPlugin()` switching to a genuinely
    different plugin, which tears down and rebuilds the whole engine as a side effect) — so
    every later `Render()` call kept showing the first stage's content regardless of which
    stage's root prim `HydraViewportWindow` actually passed in. There's no public API to
    force a re-population directly. Fixed by having `HydraViewportWindow::setStage()`
    recreate the engine (mirroring what `SetRendererPlugin()` does internally) whenever the
    stage actually changes, restoring whichever renderer plugin was already selected
    afterward so switching stages doesn't reset that choice back to the default. Verified by
    opening two visually distinct stages and confirming the combo switch changes the
    rendered content (screenshots before/after).
17. **Fixed: the Move/Rotate/Scale gizmos were always drawn/dragged along pure world axes,
    ignoring the selected prim's own rotation.** `TransformGizmoTool` previously used the
    literal world unit vectors `(1,0,0)`/`(0,1,0)`/`(0,0,1)` for every axis, plane normal, and
    ring — correct for an unrotated prim, but visibly wrong once the prim (or an ancestor)
    carried a rotation, since the handles no longer lined up with the object's actual edges.
    Fixed by adding `TransformGizmoTool::GizmoFrame` (origin + X/Y/Z axes, all in world space):
    the axes come from `ComputeLocalToWorldTransform()` via `TransformDir()` on each unit
    basis vector — that picks out the corresponding row of the matrix, i.e. the world-space
    direction of the prim's own local axis, with scale normalized back out so a scaled prim
    doesn't skew the gizmo or its drag math. `MoveTool`/`RotateTool`/`ScaleTool` all switched
    from the old `gizmoOrigin()` + hardcoded axes to `gizmoFrame()` throughout (drawing, hit
    testing, and the axis/plane drag math alike); `gizmoOrigin()` itself was deleted as dead
    code once nothing called it anymore. The `RotateTool` header comment was reworded to
    match: rings are now drawn along the prim's own current axes rather than literal world
    axes, so the existing single-axis-only limitation (documented there) applies to whichever
    axis is currently non-zero, not specifically "world-aligned" rotation. Verified with a
    prim under a 45°-rotated parent: the gizmo's arrows now visibly track the rotated cube's
    edges instead of staying screen-axis-aligned (screenshot).
18. **Fixed: Alt+drag camera orbit didn't work while the Select tool was active (but worked
    fine with Move/Rotate/Scale).** `HydraViewportWindow::mousePressEvent()` had no explicit
    Alt-modifier check at all — it always gave the active tool first refusal on a left-button
    press and only fell back to orbiting when the tool reported `handled = false`. That
    fallback happened to coincide with Alt-drag for Move/Rotate/Scale, since their
    `mousePress()` only reports `handled` when a gizmo handle is actually under the cursor
    (which an arbitrary Alt-drag rarely is). `SelectTool::mousePress()`, however, always
    reports `handled = true` — a click always resolves to "select this" or "select nothing" —
    so it permanently latched `m_dragging` and orbiting could never engage, Alt or not. Fixed
    by checking `Qt::AltModifier` explicitly in `mousePressEvent()`: Alt+left-drag now always
    orbits and skips tool dispatch entirely, regardless of which tool is active — which also
    hardens Move/Rotate/Scale against the latent case of Alt-dragging directly over a gizmo
    handle (previously would have wrongly grabbed the handle instead of orbiting). Verified via
    synthetic mouse events sent to the real `HydraViewportWindow` with the Select tool active:
    an Alt-drag changed the camera's yaw/pitch as expected, while a plain (non-Alt) drag left
    the camera untouched (still consumed as a selection action, confirming no regression).
19. **Fixed: a Debug configuration build failed to link, with two separate
    `LNK1104: cannot open file '...'` errors.** Both are the same well-known MSVC/CPython
    issue (pyconfig.h auto-embeds `#pragma comment(lib,"pythonXY(_d).lib")` into any object
    file that includes `Python.h` while `_DEBUG` is defined) surfacing in two different places,
    root-caused by reading the actual embedded linker directives with
    `dumpbin /directives` (run via PowerShell — Git Bash mangles `/`-prefixed flags into bogus
    paths, which silently broke the first several attempts) rather than guessing from headers:
    - `usdcc_ui_python` (the Shiboken6/PySide6 bindings module) failed needing
      `python312_d.lib`. Its generated wrapper `.cpp` files (regenerated by shiboken6 at build
      time, not something usdcc's own code controls) include shiboken6's `sbkpython.h`, which
      `#include`s `Python.h` with no protection at all — unlike pybind11's own equivalent
      wrapper, which explicitly undefs `_DEBUG` around its include for exactly this reason (see
      `pybind11/conduit/wrap_include_python_h.h`). A pip-installed PySide6/shiboken6 only ships
      release-mode Python Stable-ABI stub libraries (`pyside6.abi3.lib`/`shiboken6.abi3.lib`,
      already linked and sufficient on their own — they resolve dynamically against whatever
      `python3*.dll` is loaded at runtime), so the debug-suffixed import library plain
      `pip install python`/python.org installs don't ship was never going to exist. Fixed with
      `/NODEFAULTLIB:python<version>_d.lib` on the `usdcc_ui_python` target (Debug config only),
      derived dynamically from the same interpreter probe already used to locate
      shiboken6/PySide6 (see `src/cpp/ui/python/CMakeLists.txt`) — this only suppresses that one
      specific embedded request, leaving every explicitly-linked library untouched.
    - `usdcc` (the app executable) then failed needing plain `python312.lib`, i.e. the same
      pragma but for the *release*-named variant, and unconditionally — `dumpbin /directives`
      showed it embedded in every USD-header-including object file across
      `usdcc_tools`/`usdcc_usd`/`usdcc_usd_ui`, in both Debug *and* Release builds alike (USD's
      own headers apply this same "never require a debug-suffixed Python" wrapper, but do it
      regardless of the *consumer's* own build config — always requesting the release name).
      Release/RelWithDebInfo builds never noticed only because `usdcc_usd_python`'s own
      CMakeLists.txt (see milestone M2) already explicitly links the exact same file by full
      path for unrelated reasons, which happens to also satisfy every other target's bare
      request for it; `usdcc` itself never linked it at all, and USD's own lib dirs (the only
      ones on its search path) don't contain a copy. Fixed by explicitly linking
      `${Python3_LIBRARY}` — the same system-Python path `find_package(USD)` already resolved
      via `pxrConfig.cmake` — onto the `usdcc` target too, mirroring the pattern already
      working in `usdcc_usd_python`. Verified with clean Debug and RelWithDebInfo rebuilds from
      a reconfigure, plus launching the resulting Debug `usdcc.exe` and confirming it stays
      running rather than exiting immediately.
20. **Fixed: `StageManager` (in the non-UI `usdcc_usd` target) violated the "Qt only in UI
    classes" architecture rule.** An audit across `usdcc_core`, `usdcc_tools`, and the non-UI
    part of `usdcc_usd` found those clean except for `StageManager` itself: it inherited
    `QObject`, used `Q_OBJECT`/Qt signals/`emit`, and took `QString` parameters — forcing
    `usdcc_usd` to publicly link `Qt6::Core` (and, transitively, its Python bindings module,
    `usdcc_usd_python`, to depend on Qt at all despite never using it — the very thing that
    target's own CMakeLists.txt comment already flagged as worth avoiding for
    `usdcc_usd_ui`/Qt Widgets, just not carried through to Qt6::Core/QObject itself). Fixed by:
    - Adding `usdcc::core::Signal<Args...>` (`src/cpp/core/include/usdcc/core/signal.h`): a
      small, header-only, Qt-free multicast delegate. `connect()` returns a move-only
      `Connection` that disconnects on destruction (or explicitly) — the same safety
      `QObject`'s automatic disconnect-on-destroy provides, without requiring either side to
      derive from `QObject`. Internally, callback storage lives behind a `shared_ptr`/`weak_ptr`
      pair specifically so destruction order between the `Signal` and an outstanding
      `Connection` can't dangle either way. (Its internal callback-list field is deliberately
      *not* named `slots`: Qt's `<QObject>` `#define`s that as a bare macro project-wide, and a
      Qt-including translation unit reaching this header afterward silently mangled the
      member — hit and fixed during this change.)
    - Rewriting `StageManager` to drop `QObject`/`Q_OBJECT` entirely, changing
      `openStage(const QString&)` to `openStage(const std::string&)`, and replacing its four
      Qt signals with `usdcc::core::Signal` members of the same name (`stageOpened`,
      `stageClosed`, `currentStageChanged`, `selectionChanged`) fired via `operator()` instead
      of `emit`.
    - Updating every consumer (`ViewportViewPanel`, `OutlinerViewPanel`,
      `AttributesViewPanel`, `main.cpp`, `bindings.cpp`) from `connect(&stageManager,
      &StageManager::stageOpened, this, &Panel::slot)` to `stageManager->stageOpened.connect(...)`,
      storing the returned `Connection` as a panel member so it disconnects automatically
      when the panel is destroyed. `main.cpp`'s two `QString`-passing call sites now do
      `.toStdString()` first.
    - Dropping the now-unnecessary "pybind11 must come first" comment/ordering constraint in
      `bindings.cpp`: that workaround existed solely because `stage_manager.h` used to pull in
      `<QObject>`/`<QString>`, colliding with CPython's own "slots" struct member — moot now
      that `StageManager` doesn't touch Qt at all.
    - Removing `Qt6::Core` from `usdcc_usd`'s `target_link_libraries` in
      `src/cpp/usd/CMakeLists.txt`; `usdcc_usd_ui` (the actual UI target) already links
      `Qt6::Widgets`/`Qt6::OpenGL`, which pull in `Qt6::Core` transitively on their own.

    Verified: clean Debug and RelWithDebInfo rebuilds; `dumpbin /dependents` on the rebuilt
    `usd.pyd` confirms it no longer links any Qt DLL at all (previously transitive via
    `usdcc_usd`); a Python smoke test (`import usdcc.usd`, `open_stage`/`close_stage`/
    `stage_cache_ids`) still works correctly. Signal/Connection correctness was verified with a
    temporary test harness in `main.cpp` (since removed): direct connect/fire/disconnect
    semantics, all three real panels' combo boxes updating correctly when a second stage opens
    (proving the new mechanism actually reaches real UI code), and — the specific safety
    property being replaced — that destroying a panel while `StageManager` stays alive doesn't
    crash on the next signal it fires.

    That same harness also surfaced an unrelated, pre-existing bug: rapidly closing a stage
    while a `HydraViewportWindow` is showing it (i.e. calling `StageManager::closeStage()`
    synchronously, back-to-back with other work, rather than paced by real UI clicks) can
    crash with `STATUS_HEAP_CORRUPTION` inside `HydraViewportWindow`'s stage-change/engine-
    recreation path (see item 16). Bisection confirmed this reproduces independent of every
    piece of today's Qt-removal change (isolated `Signal`/`Connection` use, `StageManager`
    open/close, and panel construction/destruction each individually check out clean); it's
    specifically triggered by the compound sequence exercising the viewport's engine
    recreation without the pacing normal UI interaction provides. Not investigated further
    here — flagged for a separate pass.
21. **Added `usdcc::usd::Stage`, wrapping each `UsdStageRefPtr` StageManager tracks.** Until
    now, `StageManager` stored stages as a plain `std::vector<UsdStageRefPtr>` with a *second*,
    parallel `std::map<long, std::vector<SdfPath>>` (keyed by cache id) tracking each one's
    selection — the only piece of usdcc-specific per-stage state that existed. Adding a second
    piece of per-stage state (e.g. a future per-stage render setting) would have meant another
    parallel cache-id-keyed map. `Stage` (`src/cpp/usd/include/usdcc/usd/stage.h`) fixes that by
    giving each stage's usdcc-specific state (currently just `selectedPaths()`, but the point of
    the class is to have one place to grow that) a home directly alongside the `UsdStageRefPtr`
    it belongs to, computing and caching `cacheId()` once at construction (previously
    recomputed via a `UsdUtilsStageCache::Get()` round-trip on every call).

    Chose the full-migration option over keeping `Stage` an internal-only detail: `StageManager`'s
    entire public API (`stages()`, `currentStage()`, `openStage()`/`closeStage()`,
    `findByCacheId()`, `selectedPaths()`/`setSelectedPaths()`, and all four signals) now returns/
    accepts `StageRefPtr` (a `std::shared_ptr<Stage>`, aliased in stage.h) instead of a bare
    `UsdStageRefPtr`. `shared_ptr` rather than a raw `Stage*` deliberately: it preserves the
    exact survival-past-close semantics `UsdStageRefPtr` itself already had (documented on
    `StageManager::closeStage()`) — a ViewPanel holding a `StageRefPtr` keeps the `Stage` (and
    its selection data) alive even after `StageManager::closeStage()` removes it from the
    manager's own list, rather than being left with a dangling pointer.

    `ViewportViewPanel`/`OutlinerViewPanel`/`AttributesViewPanel` (the only other consumers of
    this API) were updated accordingly — their `m_stage` members are now `StageRefPtr`, and
    every direct USD API call on them (`GetRootLayer()`, `GetPrimAtPath()`, `GetEditTarget()`,
    etc.) goes through the new `.usdStage()` accessor. Deliberately *not* touched:
    `HydraViewportWindow::setStage()` and `UsdPrimTreeModel::setStage()` — both are lower-level,
    StageManager-agnostic classes that only ever needed a raw `UsdStageRefPtr` in the first
    place, so the panels translate at the boundary (`m_viewport->setStage(m_stage ?
    m_stage->usdStage() : nullptr)`) rather than teaching those classes about `Stage` too.

    `StageManager::stageCacheId(stage)` (the single-stage overload) was removed entirely — now
    that `cacheId()` lives on `Stage` itself, `stage->cacheId()` replaces it directly at every
    call site (bindings.cpp included); `stageCacheIds()` (the all-open-stages plural) stays, now
    implemented as a one-line loop over `stage->cacheId()`.

    Verified: clean Debug and RelWithDebInfo rebuilds, and a temporary test harness in
    `main.cpp` (since removed) confirming two independently-opened stages get distinct `Stage`
    objects and cache ids, `setSelectedPaths`/`selectedPaths` round-trip correctly through
    `Stage`, and — the specific behavior this design was chosen to preserve — that a
    `StageRefPtr` held past `StageManager::closeStage()` keeps its `Stage` (selection data
    included) alive and readable even though `findByCacheId()` no longer finds it. That test
    deliberately never touched the stage the viewport was actively displaying, to avoid
    retriggering the unrelated crash noted in item 20.

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
  archive); CI artifact publishing; the from-source PySide6/Shiboken6 build
  (§6 item 11) folded into this rather than a manual local step.
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
| M0 Repository & Build Bootstrap | In progress — vcpkg submodule, root CMakeLists.txt, `FindUSD.cmake`, and an empty-`QMainWindow` app skeleton build and run cleanly on Windows (verified); Linux/macOS untested, CI skeleton still pending |
| M1 Core Application Shell | In progress — Qt Advanced Docking System integrated into `MainWindow`, `SidePanel`/`ViewPanel` C++ base classes exist with stand-in subclasses, dock layout persists across restart (verified). `SidePanel`/`ViewPanel` are also exposed to Python via Shiboken6 and subclassable from Python (verified: import, instantiate, subclass, and content-widget ownership all confirmed working) — required building PySide6/Shiboken6 from source against usdcc's own vcpkg Qt (`tools/build-pyside.ps1`), since a pip-installed PySide6's independently-built Qt binaries clash with vcpkg's at runtime; see §5 item 10 and §6 item 11. `ViewPanel`'s ADS base isn't itself bound to Python (out of scope — see typesystem.xml); layout-persistence and panel-registration are not yet exposed to Python. Panel registration/layout persistence beyond the stand-ins is otherwise done |
| M2 OpenUSD Integration & Stage Management | Exit criteria met and verified — `usdcc::usd::StageManager` (open/close/enumerate/current-stage tracking) works from both C++ (Qt signals; standalone smoke test) and Python (`usdcc.usd` pybind11 module; standalone smoke test). Python side deliberately never passes a `UsdStageRefPtr`/`Usd.Stage` across the pybind11⇄pxr_boost::python boundary — that was tried via a custom pybind11 type_caster and confirmed to corrupt unrelated boost::python state — instead every opened stage is registered in `UsdUtilsStageCache::Get()` and only its plain-integer cache id crosses into Python, which retrieves the real stage via USD's own bindings (see §5 item 11). Remaining for a later pass: `ViewPanel`'s actual stage-dropdown UI (needs a real `ViewPanel` subclass to hang it on, M3/M4) and exposing `StageManager`'s Qt signals to Python |
| M3 Hydra Viewport | Exit criteria met and verified — `ViewportViewPanel`/`HydraViewportWindow` (`usdcc_usd_ui` target, renamed from `usdcc_usd_viewport` in M4 — see §6 item 14) render a stage correctly (confirmed by the user directly: red cube and blue sphere, correctly shaded/lit) in both `HdStormRendererPlugin` and `HdEmbreeRendererPlugin`, with working orbit/pan/zoom camera navigation, per-panel stage selection, render-delegate switching, `MainWindow` File > Open Stage, and a `usdcc.exe <stage-path>` / `tools/run.ps1 -Scene <path>` CLI path. Two real bugs were found and fixed along the way (see §6 item 12): geometric pixel corruption (fixed by moving from `QOpenGLWidget` to a `QWindow`-based `HydraViewportWindow`) and all-black shading (fixed by explicitly clearing color+depth before each `Render()` call, since `HgiInteropOpenGL`'s compositing step assumes the caller already did). A cosmetic `HgiInteropOpenGL`/`HgiGLTexture` GL-error log spam remains unexplained but doesn't affect the visible output. Remaining for a later pass: `OutlinerViewPanel`/`AttributesViewPanel` (M4) |
| M4 Scene Introspection Panels | Exit criteria met and verified — `OutlinerViewPanel` (rename/disable/select/reorder via a custom `UsdPrimTreeModel`) and `AttributesViewPanel` (view/edit via `TfType`-based dispatch) are docked alongside the viewport; selection is shared per-stage through `StageManager` (see §6 item 14 for the full design). Verified end to end through the real, wired-up UI code paths (tree/table widgets, model roles, the outliner's context menu): select, rename, selection-follows-rename, disable, attribute edit, and reorder all confirmed correct against direct stage introspection; the disable case was additionally confirmed *visually* — a screenshot taken after deactivating a prim via the outliner's checkbox showed it correctly absent from the live Hydra render, satisfying the "reflected live in the viewport" exit criterion. Known gap: array/matrix/asset-path attributes remain read-only in the Attributes panel (see §6 item 14) |
| M5 Editing Tools & Gizmos | Exit criteria met and verified — `SelectTool` (GPU-picking click-to-select via `UsdImagingGLEngine::TestIntersection`), `MoveTool` (axis/plane/free-constrained translate), `RotateTool` (axis-ring rotate), and `ScaleTool` (axis/uniform scale) all live in the new `usdcc_tools` target and are wired into `HydraViewportWindow`/`ViewportViewPanel`'s new "Tool:" combo (see §6 item 15 for the design). Verified through the real, wired-up code paths — synthetic mouse-drag events sent to the actual `HydraViewportWindow` for Move/Rotate, a hand-built `ToolContext` calling `ScaleTool` directly for Scale — confirming the resulting `xformOp:translate`/`rotateXYZ`/`scale` values, plus a screenshot showing the gizmo rendering correctly (no black-viewport or corruption regression) on the moved/rotated/scaled prim. One real bug was found and fixed along the way: the verification harness itself crashed inside ADS (`qtadvanceddocking-qt6.dll`, access violation) from calling `setCurrentDockWidget()` on a `CDockAreaWidget*` captured *before* `MainWindow::restoreLayout()`, which can rebuild dock areas from a saved layout and leave that earlier pointer dangling — fixed by fetching the dock area live via `CDockWidget::dockAreaWidget()` at the point of use instead of caching it. Known gaps: multi-selection gizmo editing is unsupported (no gizmo drawn), and `RotateTool`'s object-space rings (see §6 item 17) are only exact for a single-axis rotation |
| M6 Undo/Redo Framework | Not started |
| M7 Scripting & Python Extensibility | Not started |
| M8 Layer Editing & Logging | Not started |
| M9 Plugin System | Not started |
| M10 Dialogs & Settings | Not started |
| M11 Packaging & Deployment | Not started |
| M12 Hardening & Release Readiness | Not started |
