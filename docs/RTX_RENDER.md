# RTX Render: native C++ replacement plan

Status: M1-M4 implemented in the optional native development plugin, 2026-11-07.
Broader acceptance and cutover remain in progress. The optional native plugin,
GPU presenter, shared controls, render worker, and scene bridge build. The GPU
presenter passes an independent CUDA/Vulkan diagnostic with validation enabled.
The isolated native SDK probe renders and presents a real RTX image, saves it,
and drains SDK/GPU resources with zero Vulkan validation errors. The app also
displays and saves both test scenes in all three modes alongside Storm. The
apparent attachment blocker was cold shader compilation exceeding the old probe timeout;
compilation progress now reaches the UI. M1-M3 acceptance is not complete.
The shipped plugin remains Python-based.
M4 navigation/highlighting and real-GPU app checks pass. M5-M6 have not started.

## 1. Objective and completion criteria

Replace the Python RTX Render plugin with a native C++ plugin using NVIDIA
[ovrtx](https://nvidia-omniverse.github.io/ovrtx/) and ovstage. Adapt the
[Vulkan interop example](https://github.com/NVIDIA-Omniverse/ovrtx/tree/main/examples/c/vulkan-interop)
for GPU presentation and camera navigation inside usdcc's render view.

The finished replacement must:

- Render without a Python RTX module, numpy, a helper process, or image pipes.
- Keep normal image presentation on the GPU through CUDA/Vulkan interop.
  CPU readback is allowed for Save Image and verification.
- Preserve existing render controls and add orbit, pan, zoom, and scene framing
  directly in the rendered image.
- Highlight prims selected elsewhere in usdcc, following the selection for the
  stage shown by the RTX panel.
- Render unsaved and session-layer changes without adding renderer setup to the
  user's stage.
- Remain optional: Storm and usdcc must work without RTX runtime files, an NVIDIA
  GPU, or CUDA/Vulkan development tools.
- Remove the Python implementation, deployment entries, package requirements, and
  obsolete tests after the native acceptance checks pass.

The old implementation may remain during development for comparison. It is not a
permanent fallback or a second RTX implementation in the completed release.

The view displays the rendered image, highlights the app's selected prims, and lets
the user navigate the scene with its viewing camera. Selection synchronization is
one-way: app selection -> RTX highlight. Picking, changing selection from the RTX
image, gizmos, and scene-editing tools are outside this replacement's scope. Edits
made elsewhere in usdcc still update the image through Update on Edits.

## 2. Baseline and integration work

The current [Python plugin](../src/plugins/rtx_render) exports a flattened temporary
USD stage, renders in a helper, maps pixels to CPU memory, and sends RGBA bytes to
a `QImage` through a pipe. Camera, resolution, and transform changes use in-place
ovstage writes; other edits load another snapshot.

Preserve Real-Time, Path Tracing, Minimal, Render/Stop, Refresh, camera choices,
resolution choices, Update on Edits, status/errors, settings, and Save Image.
Preserve progressive Path Tracing: a one-sample preview followed by increasing
budgets to the requested total. Coalesce obsolete camera, resize, and edit work.

Reuse the [C++ plugin SDK](PLUGIN_SDK.md), `ViewPanel`, `StageManager`, camera
calculations, stage notifications, and per-stage selection notifications. The SDK
already exposes panel registration and the app's shared services; the render view
does not need `ToolSet` or direct undo-manager integration.

Two core integration tasks are required:

1. [RenderViewPanel](RENDER_VIEW_PANEL.md) currently owns a CPU image surface.
   Extract reusable controls/controller behavior or introduce a small presentation
   interface for a Vulkan surface. Preserve its existing image implementation and
   Python hooks used by V-Ray and other plugins.
2. Reuse orbit-camera calculations and usdcc's navigation conventions in a small
   render-view camera controller. Share only camera/input helpers if necessary;
   no Hydra picking, gizmo, or editing-tool refactor is required. Keep
   ovrtx/CUDA/Vulkan dependencies out of the app core.

## 3. Proposed architecture

Develop in `src/plugins/rtx_render_native`, using `usdcc_add_plugin()`.

| Component | Responsibility |
|---|---|
| Plugin entry point | Register panel/settings, locate runtime files, connect host services, and route logs |
| `RtxRenderPanel` | Docking, stage association, toolbar/status, navigation input, duplication, and visibility |
| `RtxEngine` | Own ovrtx/ovstage handles, asynchronous work, sample/frame budgets, and result lifetimes |
| `RtxStageBridge` | Snapshot the app stage, translate edits, manage ordinals, and replace runtime scenes |
| `RtxVulkanWindow` / presenter | Qt surface, device matching, shared image ring, synchronization, and presentation |
| Camera controller | Free viewing camera, orbit/pan/zoom, scene framing, and camera-source switching |
| Selection highlight adapter | Observe the panel stage's selected prim paths and update renderer-only outline groups |

The normal image path becomes:

```text
ovrtx CUDA output -> shared Vulkan buffer -> Vulkan image/blit -> Qt window
```

Replace GLFW with Qt. First prove `QVulkanWindow`, embedded through
`QWidget::createWindowContainer()`, can expose the required device extensions and
external-memory/semaphore facilities. Use a custom Vulkan `QWindow` if Qt's device
ownership is too restrictive. Resolve this in M1 before implementing the full UI.

Adapt the example's asynchronous polling and synchronization; do not import its
entire application, remote default scene, fixed camera/product paths, or unit
assumptions. The Qt material-editor example is useful for UI wiring, but its CPU
image display and deprecated scene-management APIs are not the target architecture.
Reuse its native selection-outline setup for externally supplied prim paths.
Do not port its picking or marquee-selection input/overlay code.

### Process, threading, and lifetime

Run ovrtx in process through its C API. The previous claim that it cannot coexist
with another OpenUSD is obsolete: NVIDIA namespaced its bundled OpenUSD in
[ovrtx 0.3](https://github.com/NVIDIA-Omniverse/ovrtx/blob/main/CHANGELOG.md#030---2026-05-15).
Test coexistence with usdcc's exact OpenUSD, Storm, Qt, and dependencies. Keep both
USD implementations behind their APIs; do not pass app `UsdStage*` pointers into
ovrtx or import ovrtx's Python bindings.

- The UI thread owns widgets, navigation input, and access to the app stage.
- A render worker owns the ovrtx/ovstage lifecycle and serialized command stream.
  Give it immutable snapshots/edit values, not live app-stage pointers.
- Specify Vulkan device/queue/surface/image ownership and synchronized queue use.
- Poll pending render/fetch/map operations without blocking the Qt event loop.
  Keep the last completed image visible while work is pending.
- Bound command queues and in-flight frames. Tag commands/results with scene
  and camera generations; discard results that refer to an obsolete view. Coalesce
  selection updates and reject highlight commands for an obsolete stage generation.
- Stop prevents new submissions and safely drains outstanding work. It does not
  promise cancellation inside a driver operation.
- Hiding stops new rendering. Deletion disconnects callbacks and retires resources
  only after their GPU users finish; do not wait indefinitely on the GUI thread.
- Destroy mappings/results, shared GPU resources, renderer, and stage in dependency
  order. Coordinate process-wide initialization/shutdown across multiple panels.
- Log native errors, runtime build identities, initialization, and slow operations.

The helper's crash isolation is deliberately lost: a native renderer/driver crash
can affect usdcc. Exceptions do not restore process isolation. Do not use process
termination or free in-flight resources to imitate the old helper's shutdown.

### Scene synchronization

usdcc's stage remains authoritative. Edits and undo/redo performed elsewhere in
the app reach ovstage through stage notifications. The render view does not author
scene changes or create undo transactions. Its free viewing camera is panel-local
state, written only to the renderer's private camera.

- Start with a flattened snapshot including unsaved/session edits. Snapshot safely
  on the stage-owning thread, then give the worker an owned result. Temporary USD
  files remain acceptable for full scene transfer; GPU image presentation does not
  require eliminating scene files.
- Preserve asset anchoring/resolver context for relative textures, MDL, references,
  and packaged assets after export to a different directory.
- Create collision-free private camera/product prims in the snapshot only. Render
  the plugin's chosen product and leave source render settings/products untouched.
- Preserve units, up axis, projection, apertures, clipping, and matrix conventions.
- Apply camera, size, and supported transform changes in place with increasing
  ordinals, committed before rendering. Coalesce repeated updates.
- Unsupported edits or failed writes trigger a full snapshot. Reset/replace the
  previous runtime scene and invalidate cached queries: population open alone can
  merge content and leave deleted prims behind.
- Cover stage switches, add/delete/rename, visibility/material changes, undo/redo,
  and projection changes. Never apply old queued edits to a replacement stage.
- Keep USD default-time rendering for parity. Timeline/simulation, sensor
  scheduling, and DLSS frame generation are outside this replacement.

### Presentation and camera navigation

- Match CUDA/Vulkan devices by UUID and validate the device of mapped outputs.
  Explain unsupported combinations instead of showing a blank panel. Do not impose
  the example's picking-related GPU-0 restriction on this image-only render view.
- Use a bounded shared-image ring with explicit synchronization. Keep mappings
  and step results alive until GPU copies and consumers have completed.
- Verify sRGB, alpha, orientation, high DPI, letterboxing, and image formats.
  Scale navigation input consistently across display sizes and device-pixel ratios.
- Support orbit/pan/zoom, Fit to View, and Reset Camera using usdcc conventions.
  Frame the scene bounds without depending on selected prims.
  Preserve following another viewport or choosing a stage camera. Free navigation
  switches explicitly to a panel-local camera initialized from the current view;
  it must not edit an authored camera or move the followed viewport.
- Navigation resets accumulation and schedules a fresh preview followed by
  refinement. Coalesce rapid movements and preserve the last completed image
  while rendering catches up. Do not add scene-selection or edit gestures.
- Save Image explicitly reads back a completed real frame, without making CPU
  readback part of every displayed frame. Save the render without selection
  highlights by default. Retain a clean GPU companion before the outline pass,
  then apply selection for presentation. Save reads that retained image without
  submitting new render work when rendering is stopped.

### One-way selection highlighting

- Read `StageManager::selectedPaths(currentStage())` on panel creation, stage
  changes, and after a scene reload. Subscribe to `StageManager::selectionChanged`
  and filter notifications by the panel's stage, not the globally current stage.
- Send copied prim paths to the worker. Use ovrtx's native selection outline groups
  and styles; do not add picking, hit buffers, or a second Vulkan selection overlay.
  Verify the outline APIs and mode support against the pinned SDK in M1.
- Clear the old outline group when selection changes or becomes empty. Support
  multiple selected prims and highlighting renderable descendants of selected
  container prims. Skip deleted, non-renderable, or unresolved paths safely.
- Keep highlighting renderer-local. Do not author selection attributes into the
  user's stage, call `setSelectedPaths()` from the RTX view, or create undo entries.
- Request a display refresh when selection changes, even after refinement is Done
  or Update on Edits is off. Stop and hiding still suppress new submissions; retain
  the newest selection and apply it when rendering resumes or the panel is shown.
- Reapply highlights after runtime scene replacement and refresh them on show.
  Disconnect subscriptions on deletion; selections in another stage must not affect
  this panel. Navigation must preserve the app's selection.

## 4. Identity, settings, and distribution

Keep plugin ID `org.usdcc.rtx-render`, panel ID `org.usdcc.rtx-render.view`, and
display name **RTX Render** in the finished replacement. Preserve plugin enablement
and saved layouts. During development load one implementation per app run; do not
scan two manifests with the same ID.

Keep `RtxRender/mode` and `RtxRender/editDelay`. Migrate `maxFrames` into explicit
Real-Time frame and Path Tracing sample budgets, initializing both from the old
value when new keys are absent. Remove `RtxRender/python`. Retire `imageInterval`
unless the presenter needs an equivalent display-update setting. Make migration
idempotent and test old settings/layouts.

Build and packaging work:

- Add opt-in `USDCC_BUILD_RTX_PLUGIN`, initially OFF. A default app configure/build
  must not fetch RTX dependencies or require CUDA/Vulkan development tools.
- Pin exact compatible ovrtx/ovstage builds, origins, and checksums. Start from the
  current 0.6/0.3 pair; verify required APIs against installed binaries, not only
  upstream main. Support both in-tree and deployed `UsdccPlugin` SDK builds.
- Honor native `built_for` checks and matching compiler, Qt, OpenUSD, and CRT.
- Locate the runtime relative to the plugin using the vendor loader and explicit
  package root. Avoid global PATH/Python/USD plugin-path changes.
- Activation registers UI without creating a GPU renderer. A missing runtime must
  not prevent app startup; defer loading vendor DLLs accordingly.
- Produce a versioned native plugin artifact with required notices and verified
  runtime/example redistribution terms. If native runtime redistribution is not
  available, support a user-installed runtime root. No Python package-install step
  is required for rendering. Distinguish build tools from end-user runtime files.
- Change `tools/deploy.ps1` to install the built artifact into
  `deploy/plugins/rtx_render`, rather than copying Python source. Update required
  file checks and example-copy exclusions. Validate distributions without RTX too.
- Clean stale legacy files from the known RTX deploy folder. Detect old user-folder
  copies, which currently override shipped plugins, and provide an explicit
  migration action that backs up the recognized legacy copy. Do not silently load
  the old implementation or delete unrelated plugins.
- Remove RTX package declarations without deleting shared Python environments or
  packages that other plugins may still require.

## 5. Implementation milestones

Milestone exits describe required evidence. Complete M1-M5 before the final
removal in M6. The implementation record below distinguishes working setup
from the unvalidated rendering path.

### M1. Native plugin and coexistence proof

- [x] Add optional native target/manifest through the existing SDK.
- [x] Create a docked Qt Vulkan surface and resolve the window implementation.
- [x] Initialize the pinned SDK pair; display a local scene while Storm is open.
- [x] Verify deferred initialization, panel creation/duplication/destruction, stage
  close/reassignment, and an actionable missing-runtime error without a GPU.
- [ ] Verify panel close/reopen during actual rendering and unsupported-GPU
  behavior; the app still starts.

Exit: a real RTX image inside usdcc with no Python RTX module imported, working
Storm coexistence, and recorded SDK/app builds and surface design.

#### M1 implementation and validation record

`USDCC_BUILD_RTX_PLUGIN` defaults to `OFF`. Enabling it builds
`src/plugins/rtx_render_native` with the existing plugin SDK. The plugin preserves
`org.usdcc.rtx-render` and `org.usdcc.rtx-render.view`; use an explicit plugin path
for development. Do not install it over the shipped Python plugin yet.

The current Qt build has `QT_FEATURE_vulkan=-1`. M1 therefore uses a custom Win32
Vulkan `QWindow` embedded through `QWidget::createWindowContainer()`. Vulkan and
Volk headers are confined to the optional plugin target and fetched with SHA256
pins. Vulkan instance, surface, NVIDIA device, swapchain, and external-memory /
external-semaphore / timeline capability checks initialized successfully while
Storm was open. CUDA/Vulkan resource sharing is M2 work and is not proven by
these capability checks.

The native engine uses delayed DLL loading, explicit runtime roots, a flattened
temporary stage, and asynchronous status polling. The user's stage is not passed
to the vendor SDK. M2 replaces the original M1 CPU-transfer prototype with GPU
presentation; M3 uses the shared render panel's camera and resolution choices.
Snapshots and GPU work outlive closed panels safely through owned worker state.
An RTX-produced fixture frame now reaches the presenter and saves correctly in
the isolated native probe. Its synthetic GPU diagnostic remains separate evidence.

Native SDK identities are pinned in `deps/rtx-native.json`: ovrtx
`0.6.0+release.1791947691.c6d71501.gl` and ovstage
`0.3.0+mr.0.36fe3c44.local`, Windows x64. CMake checks build strings and forwarding
DLL SHA256 values, then records SDK metadata in the plugin's
`sdk-build-info.txt`. Supply installed native package roots containing `bin`,
`include`, and `lib/cmake`; CMake does not install Python packages or download
the vendor SDKs. The optional native build also requires CUDA Toolkit headers
(tested with 12.8); it loads the CUDA driver dynamically and does not link the
CUDA runtime or require CUDA compilation. The default app build remains free of
CUDA/Vulkan development dependencies.

```powershell
cmake -S . -B build -DUSDCC_BUILD_RTX_PLUGIN=ON `
  -DUSDCC_RTX_OVRTX_ROOT=C:/path/to/ovrtx `
  -DUSDCC_RTX_OVSTAGE_ROOT=C:/path/to/ovstage
cmake --build build --config RelWithDebInfo --target usdcc rtx_render_native usdcc_python_tests
build/src/cpp/app/RelWithDebInfo/usdcc.exe --no-startup-scripts --plugin-path build/plugins test_data/rtx_native_m1.usda
```

Use the current app build: the shared render-panel presentation API changed in
M3, so an older deployed app is not a compatible test host. Set the OpenUSD
install's `bin` and `lib` on `PATH` and its `plugin/usd` as
`PXR_PLUGINPATH_NAME`, as `tools/run.ps1 -UsdInstall ...` does.
The app and plugin must use matching app SDK/Qt/USD builds. The generated
`build/plugins/rtx_render_native/runtime.json` locates the development runtimes;
`USDCC_RTX_OVRTX_ROOT` and `USDCC_RTX_OVSTAGE_ROOT` override its values at runtime.
Without either locator, the plugin searches `runtime/ovrtx` and
`runtime/ovstage` under its own folder. Missing runtime DLLs produce an error
when Render is pressed, without loading a renderer at app startup.

Observed environment: OpenUSD 26.08, Qt 6.11.1, MSVC 19.44.35229.0,
RTX 4090, NVIDIA driver 621.14, Vulkan 1.4.357. The native target builds. Current
test results are recorded under M2 and M3 below. Configuring with
`USDCC_BUILD_RTX_PLUGIN=OFF` succeeds with empty native SDK roots. The full
`tools/build.ps1 -UsdInstall C:\Users\olive\DEV\git\USD\build\usd_typhoon`
build also succeeds with the native target enabled.

**Attachment blocker resolved (2026-11-07):** longer isolated runs show that
`ovrtx_attach_ovstage()` waits for initial ray-tracing pipeline compilation.
Native stacks show active work in NVIDIA's shader compiler, and SDK INFO logs
report `Waiting for RtPso async group async compilation`. A minimal C++
bootstrap returned successfully after roughly 100 seconds. The same binary
renamed `python.exe` attached in about five seconds; the observed cache behavior
explains why successful Python rendering did not establish warm native startup.
No executable renaming, Python initialization, driver-profile changes, or Vulkan
layer disabling is used in the fix.

The engine receives SDK INFO messages for `omni.rtx`, forwards compilation waits
to the active request's status and app log, and preserves warning/error delivery.
Routing is synchronized, ignores stopped/hidden/closed or superseded requests,
and holds no panel pointer. Other INFO messages are filtered out. The callback
is disabled before SDK shutdown. Production rendering has no artificial attach
timeout; Stop suppresses new work while an accepted SDK call drains.

On the validation RTX 4090, the native probe displayed and saved the local
fixture at 960×540 after about 55 seconds of compilation. It then completed
SDK/GPU shutdown with zero Vulkan validation errors. A repeat of the unchanged
executable completed the entire render-and-drain probe in 6.65 seconds. The
saved image was inspected and shows the fixture's cube and blue sphere. This
probe uses C++ only, without an interpreter or Python RTX imports.

App validation with the matching current build also displays and saves
1280×720 images for `test_data/rtx_native_m1.usda` and the reported
`cube_stacks_scene.usda` in Real-Time, Path Tracing, and Minimal. Storm remains
open beside RTX. The fixture uses its scene camera; the reported scene also
passes with a viewport camera on that same stage. For the cube-stack image,
frame `/World/StackGroup0` in Storm and choose Viewport Camera in RTX; fitting
the entire scene includes its oversized ground plane. Source-stage flattenings are
unchanged after rendering. Mode changes, Stop/Render between modes, separate
panels for the two stages, Save Image, and normal app shutdown pass. No Python
RTX/ovrtx/ovstage modules are imported, and no Vulkan validation errors are
reported. Automated non-GPU native tests still pass all 46 checks. This closes
the attachment blocker; broader edit, lifecycle, and fidelity acceptance below
remains open.

Build and run the isolated diagnostic explicitly (it is excluded from normal
builds and CTest; its default timeout is ten minutes to accommodate cold caches):

```powershell
cmake --build build --config RelWithDebInfo --target rtx_native_sdk_probe
build/src/plugins/rtx_render_native/RelWithDebInfo/rtx_native_sdk_probe.exe `
  C:/path/to/ovrtx C:/path/to/ovstage test_data/rtx_native_m1.usda /Render/Product `
  build/rtx-native-fixture.png
```

Run with the matching Qt runtime and `build/src/cpp/core/RelWithDebInfo` on
`PATH`, and set `USDCC_RTX_VULKAN_VALIDATION=1` to enable validation. The optional
last argument saves the displayed RTX frame through the engine's explicit Save
Image path. `USDCC_RTX_PROBE_TIMEOUT_SECONDS` overrides the timeout (1–3600).
The diagnostic reports named phases and compilation progress, then waits for
SDK and GPU retirement. A `GPU image` size message, completed drain, and exit
code zero are required for success. The fixture and reported scene have now
displayed alongside Storm; finish the remaining lifecycle checks before marking
M1 complete.

### M2. GPU presentation and asynchronous lifecycle

- [x] Implement UUID matching, CUDA output mapping/copy, a bounded shared GPU
  buffer/image ring, and external semaphores (six slots for M4 display/clean pairs).
- [x] Implement worker ownership, timed operation polling, generation/revision
  tags, and coalesced desired state with one pending display/save result per panel.
- [x] Implement resize, hide/show, duplicate panels, stage close, and asynchronous
  shutdown/draining; preserve the last displayed image during slow work.
- [ ] Validate these lifecycle paths with an attached RTX renderer, including
  multiple panels and normal app exit after real rendering.

Exit: no CPU readback or image IPC in normal display; repeated lifecycle operations
have no validation errors, leaked handles, or callbacks into deleted UI.

#### M2 implementation and validation record

The normal display path maps `LdrColor` to CUDA, copies it into one of six
exported Win32 Vulkan buffers, and blits a Vulkan image into the swapchain. There
is no CPU pixel mapping or image IPC in this path. Buffers are reused at a fixed
resolution; discarded ready signals are consumed before reuse. Device selection
matches CUDA/Vulkan UUIDs, configures RTX's active CUDA device, and checks every
mapped tensor's device, RGBA8 layout, dimensions, and strides.

External binary semaphores order the CUDA copy before Vulkan reads; Vulkan queue
ownership barriers and fences retire consumers before a slot is reused. UI fence
polling continues while hidden. Device waits, resizing retirement, and destruction
run on a retirement worker. Native windows survive until their surfaces retire.

One persistent render service serializes process initialization, per-panel
sessions, ordinal writes, and frames. Stop/hide suppress subsequent submissions;
work already accepted by the SDK drains before mappings/results are released.
Closing a panel clears its queued work and disconnects UI callbacks without
joining a GPU job on the GUI thread. Normal main-window closing polls SDK and
GPU retirement before closing; its progress dialog can cancel the close while
the app remains responsive. A vendor call that never returns cannot be cancelled
safely. The isolated native probe now proves a full drain after rendering and
saving a real RTX frame; broader app lifecycle acceptance remains below.
The close-progress dialog is constructed only after an actual close request,
preventing Qt's automatic progress timer from displaying it during rendering.

The explicit `--interop` diagnostic requires an NVIDIA GPU but no initialized
RTX renderer. It fills the ring, abandons frames, presents a known gradient,
resizes, hides/shows, verifies an exact RGBA GPU round trip, and retires resources
before destroying the native window. On the recorded RTX 4090 it passed with
22 presentations and zero Vulkan validation errors; its captured window also
showed the expected gradient and letterboxing. This does **not** count as an RTX
scene-rendering test.

```powershell
cmake --build build --config RelWithDebInfo --target rtx_native_sdk_probe
$env:PATH = "$PWD/build/src/cpp/core/RelWithDebInfo;$PWD/build/vcpkg_installed/x64-windows/bin;$env:PATH"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$PWD/build/vcpkg_installed/x64-windows/Qt6/plugins/platforms"
$env:USDCC_RTX_VULKAN_VALIDATION = "1" # requires VK_LAYER_KHRONOS_validation
build/src/plugins/rtx_render_native/RelWithDebInfo/rtx_native_sdk_probe.exe --interop
```

An optional filename after `--interop` captures the diagnostic window. Validation
is opt-in for development; end-user machines do not need validation layers.

### M3. Render parity and scene synchronization

- [x] Reuse render controls through an optional presentation widget and Save Image
  hook; preserve the existing CPU image implementation and Python hooks.
- [x] Implement flattened snapshots, full renderer/stage replacement, and
  ordinal-based camera, size, and transform updates with snapshot fallback.
- [x] Wire all three modes, camera/resolution choices, settings migration, and
  asynchronous Save Image readback of the retained displayed GPU frame.
- [x] Schedule progressive Path Tracing previews with an exact final sample budget.
- [x] Test unsaved/session snapshots, relative assets, collision-free setup,
  projection/units/up axis, deleted prims, source-layer isolation, and undo/redo.
- [ ] Verify mode fidelity, real incremental writes, stage switching during live
  rendering, and the reported scene end-to-end after attachment is fixed.

Exit: existing RTX render behavior works in C++ and renderer setup leaves the
user's stage unchanged.

#### M3 implementation and validation record

The C++ panel derives from `RenderViewPanel`, sharing Render/Stop, Refresh,
viewport/prim cameras, resolution, Update on Edits, Save Image, and Settings.
V-Ray and Python render panels continue using `QImage` presentation. Native Save
Image retains the displayed GPU frame and reads it back only on request; an I/O
failure reports an error without stopping rendering.

`stage_bridge.cpp` exports under the source resolver context with unsaved/session
opinions, resolved asset paths, preserved metadata, and collision-free private
camera/product prims. Supported edits use local transforms and increasing
ordinals; projection changes, structural/unsupported edits, and failed writes
replace the runtime scene completely. No app USD pointer reaches the worker.

`RtxRender/mode` and `RtxRender/editDelay` remain. Existing `maxFrames` seeds
`frameBudget` and `sampleBudget` only when those new keys are absent. Native
rendering does not consume `python` or `imageInterval`. Path Tracing uses budgets
1, 2, 4, ... capped at the exact requested sample count; Real-Time and Minimal
use frame counts. Camera/size/edit updates invalidate older view generations.
Modes remain independent between panels; the mode setting supplies the default
for newly opened panels.

Three native tests pass 46 checks, covering deferred setup, settings migration,
generation rejection, sample budgets, snapshots, edits, and undo/redo. Existing
Python render-panel (76), Python RTX (47), V-Ray (31), and C++ render-panel (30)
checks pass. Qt's isolated test-settings file needs filesystem write access;
GPU tests remain separate from these tests. Actual RTX output fidelity and
incremental-update success still require the broader acceptance checks below.
The scheduling and stage-bridge tests build without the optional native target
or vendor SDK; configuring with RTX off and empty SDK roots, building, and running
those two tests passes 30 checks. The plugin activation test is enabled with the
native target. An app smoke test also passed native control creation, an
actionable unsupported-driver error, no Python RTX import, and clean close.

### M4. Camera navigation and selection highlighting

- [x] Connect orbit/pan/zoom, scene framing, and camera reset to the rendered image.
- [x] Reuse camera calculations and navigation conventions without editing-tool dependencies.
- [x] Preserve scene-camera and follow-viewport choices; switch to a local free
  camera explicitly when navigating directly in the render view.
- [x] Coalesce navigation updates, reset accumulation, and render new previews.
- [x] Mirror per-stage app selection into native RTX outlines, including initial
  selection, multiple prims, selected containers, deselection, and reloads.
- [x] Refresh highlights independently of Update on Edits and preserve Stop/hide
  behavior; save clean images without selection highlights.
- [ ] Verify high-DPI input, independent cameras in duplicate panels, and that
  navigation changes neither the source stage nor its selection or undo history.

Exit: users can navigate and frame the rendered scene directly, with responsive
image updates and highlights matching selection elsewhere in the app. The RTX
view does not pick, change selection, or edit the scene.

#### M4 implementation and validation record

In the native RTX image, drag the left button to orbit, the middle button to pan,
and use the wheel to zoom. Alt+left drag also orbits. Fit to View (`F` over the
image) frames all visible scene geometry, independently of selection. Reset
Camera (`Home`) uses the viewport's default orbit pose. Both actions are also
on the toolbar and image context menu. Letterbox bars ignore navigation input.

`navigation.cpp` reuses `orbitCameraAxes`, `frameOrbitCamera`, `orbitClipping`,
pole handling, and cursor wrapping. Input uses logical image coordinates, so
device pixel ratios are not applied twice. Navigation initializes from the
currently viewed camera, preserves its roll/projection, and explicitly selects
Free Camera. The shared panel coalesces changes at 30 ms. Viewport and authored
camera choices remain available; duplicated native panels copy their viewing
state independently. Source cameras, selection, and undo history are untouched.

Selection subscriptions are per stage and independent of Update on Edits.
Selected containers expand to unique renderable descendants, including instance
proxies. Deleted/non-renderable paths are skipped. Snapshots record geometry
membership so new source prims absent from the rendered snapshot are not sent
to the renderer. A separate selection revision rejects stale highlight frames
without cancelling scene loading. Selection changes after Done request a fresh
display; Stop/hide retain updates until rendering resumes. Reload reapplies them.
The SDK's renderer-only outline groups use an orange, two-pixel edge with no fill.

With selection active, a render cycle copies a clean GPU image, applies outline
groups, and produces its highlighted companion at the same camera/sample budget.
The display frame retains the clean frame for Save Image. Empty selection needs
one pass. The ring grows from three slots to six, bounding three display/clean
pairs. This increases selected-view GPU work and potential VRAM use; no CPU pixel
readback occurs until Save or an explicit diagnostic. Saving while stopped reads
the retained clean frame and does not submit another render. Fast completed
renders also restore Done correctly after Stop/Render or reload.

Validation on the recorded RTX 4090/Windows environment:

- Six non-GPU native tests passed 61 checks; the shared C++ render-panel
  test passed 34, including local-camera coalescing and source isolation.
- Native GPU probes passed in Real-Time, Path Tracing, and Minimal: 540 orange
  outline pixels in the selected-sphere display and zero in the clean save;
  complete SDK/GPU drain and zero Vulkan validation errors in each mode.
- The six-slot interop diagnostic passed 21 presentations, resize/hide/show,
  discarded-frame reuse, and exact RGBA readback.
- An app run passed 18 scenarios: initial/container/multiple selection,
  deselection after Done, Update on Edits off, Stop/resume, hide/show, wheel,
  orbit, pan, frame/reset, camera-source switching, per-stage selection isolation,
  reload, and clean Save while stopped. Source flattening, selection, undo state,
  and the followed viewport camera remained unchanged by navigation; no Python
  RTX import or Vulkan validation error occurred. Normal app exit returned zero.
- Additional snapshot-membership and plugin-duplication/undo checks build. The
  final regression rerun was declined, so those added assertions have not been
  executed. The final M4 acceptance checkbox remains open for that verification.

For an explicit outline/clean-save GPU check, use the SDK probe above with an
output filename and `USDCC_RTX_PROBE_SELECTION=/World/Sphere`. It also writes
`<filename>.with-selection.png` for comparison. `USDCC_RTX_PROBE_MODE` can be
`Path Tracing` or `Minimal` when the supplied scene's RenderProduct uses that mode.
These variables affect the diagnostic only.

The separately reported `omniverse_preview.usda` MDL scene remains an open
first-frame investigation: the shipped Python RTX helper loads it, but a 512×512
one-sample diagnostic produced no frame within ten minutes. Its stacks show MDL
translation work. M4 does not claim to fix that scene's material/rendering issue.

### M5. Regression coverage and deploy validation

- [ ] Add SDK-independent scheduler/stage-bridge tests and Qt tests with a fake engine.
- [ ] Add explicitly labeled GPU tests requiring an RTX device and native runtime.
- [ ] Test a clean packaged plugin without development SDK paths or Python RTX packages.
- [ ] Test settings/layout migration, a shadowing legacy user copy, and RTX omission.
- [ ] Complete the acceptance matrix below and record results before cutover.

Exit: non-GPU CI works without the SDK; actual GPU and clean-deploy results prove
the replacement. Skipped GPU tests do not count as successful GPU validation.

### M6. Cutover and remove Python RTX

- [ ] Ship only the native artifact under the preserved plugin/panel IDs.
- [ ] Remove `src/plugins/rtx_render`, including Python module, client, helper,
  protocol, fake-renderer environment variables, and RTX package declarations.
- [ ] Replace/remove `tests/rtx_render_plugin_test.cpp`, `tests/rtx_helper_test.py`,
  their CMake entries, and RTX-specific Python module cleanup.
- [ ] Retain generic package/index tests, Python render-panel support, and helper
  functionality still needed by V-Ray or other plugins.
- [ ] Complete deployment/release checks, example-copy changes, and stale-file cleanup.
- [ ] Update `RENDER_VIEW_PANEL.md`, `PLUGINS.md`, `PLUGIN_SDK.md`,
  `PYTHON_PACKAGES.md`, `CI.md`, `VRAY_RENDER.md`, and `MATERIAL_EDITOR.md`
  wherever they describe the old RTX implementation.
- [ ] Rewrite this document as native setup/usage documentation with verified limits.
- [ ] Audit source/tests/scripts/docs for old RTX imports, helpers, install steps,
  settings, and process assumptions; resolve every remaining reference.

Exit: clean source/deploy contain one C++ RTX implementation, no legacy Python
RTX plugin, and no RTX Python package-install prompt. Upgrades are verified.

## 6. Acceptance matrix

| Area | Required checks |
|---|---|
| Reported scene | Open `C:\Users\olive\DEV\git\ovrtx\tests\data\cube_stacks_scene.usda`; display/refine inside the app in all three modes, with free and scene cameras |
| Portable fixtures | Add repository-owned small scenes; automated coverage must not depend on the external ovrtx checkout or network assets |
| Difficult content | Recheck the previously problematic MDL/dome-light scene; maintain UI responsiveness and honest progress during slow GPU work; C++ does not remove shader/material cost |
| Fidelity | Unsaved/session edits, relative textures/MDL, UsdPreviewSurface/MaterialX, units/up axis, projection/clipping, alpha, and color |
| Changes | Transforms, visibility/material edits, add/delete/rename, undo/redo, full reload, rapid camera/size changes, and stage switches with work pending |
| Navigation | Orbit/pan/zoom/frame/reset, high-DPI input, camera-source switching, independent panel cameras, accumulation restart, and no changes to the stage, selection, or undo history |
| Selection highlights | Initial and changed app selection, multiple prims, container descendants, deselection/deletion, stage isolation, reload/show/resume, changes after Done or with Update on Edits off, and clean saved images; no picking or selection writes from RTX |
| Lifecycle | Render/Stop/restart, slow initialization/frame, hiding, deletion/duplication, multiple RTX panels, Storm coexistence, and app exit |
| Deployment | Matching build identity, missing runtime/driver/GPU, optional RTX omission, old layouts/settings, and stale deploy/user copies |
| Presentation | GPU-only steady display, correct memory/semaphore lifetimes, no stale frames, explicit Save Image readback, and stable resource use across repeated cycles |
| Existing app | V-Ray/generic Python render panels, viewport tools, docking/layout restoration, stage editing, and plugin enablement still work |

Windows with the current RTX 4090 is the initial validation platform. Additional
GPU/driver or Linux support requires separate tests and documentation; upstream
example support alone does not establish support in usdcc.
