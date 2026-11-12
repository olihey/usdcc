# RTX Render

RTX Render is an optional C++ plugin using NVIDIA ovrtx and ovstage. It displays
images through CUDA/Vulkan interop, supports camera navigation, and highlights
selection made elsewhere in usdcc. It has no picking or gizmos, and no ground grid;
it doesn't use the viewport background color ([VIEWPORT_GRID.md](VIEWPORT_GRID.md) §2). The Python RTX
plugin and helper have been removed; V-Ray and generic Python panels remain.

## Setup

The initial platform is Windows x64 with an RTX GPU. Validation used Windows 11,
RTX 4090, driver 621.14, OpenUSD 26.08, Qt 6.11.1, MSVC 19.44 and CUDA toolkit
12.8. Other GPU/driver combinations require validation; Linux is not implemented.
Missing runtimes, unsupported drivers and CUDA/Vulkan device mismatches produce
an error in the panel and Log.

Exact SDK builds and loader SHA-256 hashes are pinned in
[deps/rtx-native.json](../deps/rtx-native.json). Both CMake and runtime loading
check identity; the public version alone is insufficient. Use complete matching
native SDK packages, containing bin, headers/import libraries for building, and
`<name>-build-info.json`. Loading is deferred until Render. RTX requests no
Python packages.

```powershell
.\tools\build.ps1 -UsdInstall C:\usd\install -WithRtx `
  -OvrtxRoot C:\sdk\ovrtx -OvstageRoot C:\sdk\ovstage
.\tools\run.ps1 -UsdInstall C:\usd\install
```

Options remain in the CMake cache. run.ps1 adds build/plugins to the plugin search
path when native RTX is enabled. Direct executable launches need
`--plugin-path <repo>\build\plugins`. `-WithoutRtx` disables native RTX and GPU
tests. Fresh default builds omit RTX and need no vendor SDK or CUDA toolkit.

Runtime roots resolve in this order: `USDCC_RTX_OVRTX_ROOT` and
`USDCC_RTX_OVSTAGE_ROOT`; then runtime.json beside usdcc-plugin.json; then
runtime/ovrtx and runtime/ovstage beneath the plugin folder. Roots point at the
package directory, not bin. Example runtime.json:

```json
{"ovrtx": "C:/sdk/ovrtx", "ovstage": "C:/sdk/ovstage"}
```

## Using the view

Open RTX Render from the stage view menu and press Render. Real-Time, Path Tracing
and Minimal are available. Path Tracing starts with one sample and refines toward
the sample budget in Settings > RTX; the other modes use a frame budget. Cold
shader compilation can take minutes. Status and Log report progress while Qt
continues processing events.

With its own camera exposure, ovrtx renders lights about 1/1134 as bright as Storm.
Settings > RTX > Lighting > **Match Storm light intensities** (on by default) gives the RTX
camera unit exposure instead, with **Intensity multiplier** on top. Turn it off for scenes
lit for Omniverse. Lights themselves aren't changed, so distant and normalized sphere
lights still differ ([RTX_LIGHT_INTENSITY.md](RTX_LIGHT_INTENSITY.md)).

The toolbar has the app's icons (▶/■, ⟳, 💾, ⚙); everything else is in the options
menu (☰), as [RENDER_VIEW_PANEL.md](RENDER_VIEW_PANEL.md) §1 describes.

| Control | Where | Behavior |
|---|---|---|
| Render / Stop | ▶ / ■ | Start or prevent new submissions; retain the latest image and queued changes |
| Refresh | ⟳ | Replace the snapshot with the current app stage |
| Save Image | 💾 | Read back the completed clean GPU frame without selection outlines |
| Settings | ⚙ | Settings > RTX |
| Camera | Options | Follow the viewport, choose a scene camera, or use a panel-local Free Camera |
| Resolution | Options | Fit the panel or use a fixed output size |
| Update on Edits | Options | Follow edits elsewhere; unsupported incremental edits reload the snapshot |
| Mode | Options | Real-Time, Path Tracing or Minimal; a change reloads |
| Fit to View | Options, or F over the image | Frame the whole scene |
| Reset Camera | Options, or Home over the image | Restore the default orbit pose |
| Orbit | Left / Alt+left drag | |
| Pan | Middle drag | |
| Zoom | Wheel | |

Navigation switches to Free Camera. It never edits authored cameras, moves the
followed viewport, changes selection or creates undo entries. Duplicate panels
own independent viewing cameras. Letterbox bars ignore input; logical coordinates
avoid applying display scale twice.

Selection synchronization is one-way: app selection becomes orange renderer-only
outlines. Selected containers expand to renderable descendants, including instance
proxies. Highlights refresh after Done and with Update on Edits off; Stop/hiding
retain the newest selection until rendering resumes. Paths absent from the
snapshot are excluded until reload. Selected views retain a clean GPU companion
before the outline pass, increasing GPU work and possible VRAM use. Save while
stopped reads that companion without submitting another render.

## Architecture and limits

[rtx_render_native](../src/plugins/rtx_render_native) derives from the shared
[RenderViewPanel](RENDER_VIEW_PANEL.md). rtx_panel.cpp owns the controller; engine
and presentation interfaces allow SDK-independent tests. RtxEngine serializes
vendor API work on a persistent worker. The UI supplies owned snapshots and
copied values, never app-stage pointers. Scene/view/selection revisions reject
obsolete frames.

The bridge flattens unsaved/session opinions under the resolver context, anchors
assets, and creates collision-free private camera/product prims only in the
snapshot. Camera, size and supported transforms update in place; other edits or
failed writes replace the snapshot. Rendering uses USD default time.

```text
ovrtx CUDA output -> shared Vulkan buffer -> Vulkan image/blit -> Qt QWindow
```

CUDA/Vulkan devices are matched by UUID. A bounded ring retains frames until GPU
consumers finish; only Save and diagnostics read pixels to the CPU. The custom
Vulkan window keeps vendor/CUDA/Vulkan dependencies out of the app core.

The renderer runs inside usdcc, so a native SDK/driver crash can affect the app.
Stop cannot cancel a call already executing inside the SDK. Deletion retires
resources after their users finish. App close drains SDK/GPU work with a
responsive progress dialog; cancelling close keeps the app available. C++ does
not remove material translation cost.

**Unresolved content:** the reported omniverse_preview.usda scene, with roughly
1,550 MDL shaders, produced no frame in a bounded ten-minute Python diagnostic at
512 x 512 and one sample. Stacks were in MDL translation. The native cutover does
not establish that this scene renders successfully. Broader MDL/MaterialX fidelity,
relative assets, alpha/color and long-running stability still need acceptance.
Portable fixtures do not prove every production scene.

## Deployment and upgrades

```powershell
# Native plugin using external runtime roots on the target:
.\tools\deploy.ps1
# Explicit local portable bundle with native runtime bins and licenses:
.\tools\deploy.ps1 -Destination build\rtx-package -BundleRtxRuntime
# Omit RTX even when it was built:
.\tools\deploy.ps1 -Destination build\without-rtx -OmitRtx
```

Deploy copies only the built native artifact into plugins/rtx_render, excluding
the development runtime.json, Python RTX bindings and duplicate RTX examples.
SDK files remain external by default. Explicit bundling copies native bin trees,
identity metadata, LICENSE and THIRD-PARTY-NOTICES. Bundling is a packaging
capability, not a statement that redistribution rights were reviewed; consult
the supplied SDK terms for the intended distribution. Target machines need no
CUDA/Vulkan development tools. Disabled builds ignore stale RTX build artifacts.

Plugin ID `org.usdcc.rtx-render` and panel ID `org.usdcc.rtx-render.view` are
preserved for layout restoration. RtxRender/maxFrames seeds the new frame/sample
budgets once; existing new values win. Mode/edit delay retain their keys. Retired
Python copies with this plugin ID are ignored before duplicate resolution and
package planning, including higher-priority user copies. Remove obsolete copies;
they cannot serve as a fallback. Deploy replacement removes stale deploy files.
Previously installed shared Python packages remain because user scripts may use
them.

### Licensing: ship the plugin, not the runtime

Checked on 2026-11-08 against what NVIDIA publishes online. This is not legal advice, and the LICENSE
in the pinned ovrtx 0.6.0 package wasn't read, as no copy was on the machine; GitHub's current one was.

- **The plugin** is usdcc's own code under its BSD-3-Clause licence, and ships with every deploy.
- **ovrtx and ovstage** are NVIDIA's. PyPI marks ovrtx `LicenseRef-NvidiaProprietary` and Alpha; its
  LICENSE points to the NVIDIA Software License Agreement and the Product Specific Terms for AI Products.
  ovstage states no licence of its own. Those terms allow distribution only "as part of a Customer
  Product", and with conditions:
  - the grant is "subject to the Part Number or Order Form and payment of fees", and it isn't clear that
    covers a free download with no order;
  - end users need terms "at least as protective as" NVIDIA's, which usdcc's BSD licence isn't;
  - Omniverse software is "licensed solely to run on systems with NVIDIA Platforms";
  - no modifying the runtime or removing its notices, and NVIDIA may ask for usage reports, up to monthly;
  - pre-release software is "not intended for use in business-critical systems".
- **The Omniverse MDL files** that come with ovrtx haven't been reviewed (MATERIAL_EDITOR.md §6, 7).

So a public deploy omits the runtime, as the default deploy does, and each user installs ovrtx and accepts
NVIDIA's terms themselves. Don't publish a `-BundleRtxRuntime` deploy without NVIDIA's written agreement
that free ovrtx can be redistributed inside a BSD-licensed app, and on which end-user terms.

Sources: [ovrtx on PyPI](https://pypi.org/project/ovrtx/),
[its LICENSE](https://github.com/NVIDIA-Omniverse/ovrtx/blob/main/LICENSE),
[NVIDIA Software License Agreement](https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-software-license-agreement/),
[Product Specific Terms for AI Products](https://www.nvidia.com/en-us/agreements/enterprise-software/product-specific-terms-for-ai-products/).

## Validation and milestone record

M1-M4 supplied the native plugin, presentation, stage bridge, navigation and
highlighting. M5 adds SDK-independent scheduler/stage/runtime/controller tests,
explicit GPU tests and packaged-app validation. M6 removes Python RTX source,
tests and package declarations, switches deployment, handles legacy user copies,
and updates documentation. Generic Python package/index tests and V-Ray remain.

M5's automated coverage and deploy checks and M6's implementation cutover are
complete. Broader production-content acceptance remains open as recorded below.

Normal CI uses -WithoutRtx. The real native controller, stage bridge and navigation
tests still build without vendor headers, DLLs, CUDA, Vulkan or a GPU. The native
activation/layout/migration test additionally runs when the plugin is built and
checks missing roots without initializing a GPU.

```powershell
.\tools\build.ps1 -WithRtx -RtxGpuTests
cmake --build build --config RelWithDebInfo --target rtx_gpu_test_bins
ctest --test-dir build -C RelWithDebInfo -L gpu --output-on-failure
ctest --test-dir build -C RelWithDebInfo -LE gpu --output-on-failure --timeout 900
.\tools\ci\rtx-smoke-test.ps1 -Deploy build\rtx-package
```

GPU tests are opt-in, labeled gpu/rtx, serialized and bounded. They use the
repository-owned test_data/rtx_native_m1.usda in all three modes, compare outlined
and clean output, and verify exact CUDA/Vulkan transfer, resize/hide/show and
retirement with Vulkan validation. The host must supply the validation layer.
The manual rtx-gpu.yml workflow requires a prepared self-hosted Windows RTX
runner; hosted CI skipping GPU work does not count as GPU validation.

The packaged test uses a fresh profile, Windows-only PATH, no development locator
or RTX Python packages. It verifies DLLs load inside the package, a frame and
clean save complete, the source stays unchanged, no Python RTX imports occur,
and app exit drains normally. Use -Mode MissingRuntime for an external-root
deploy with no roots, or -Mode Omitted for an -OmitRtx deploy. Artifacts remain
under build/rtx-packaged-smoke by default.

Validation record, 2026-11-07, on the environment above:

| Area | Result / remaining acceptance |
|---|---|
| GPU | Four labeled tests passed: three modes and interop; zero Vulkan validation errors |
| Earlier app coverage | 18 navigation/selection/lifecycle scenarios passed with Storm; source, selection, undo and followed viewport unchanged |
| Regression | Full non-GPU suite passed: 328 test cases across core, UI and Python/plugin executables; final targeted native run passed 91 checks |
| Build without RTX | Separate build with native target off and both SDK roots empty passed 7 native tests / 68 checks, including the real Qt controller |
| Packaged native render | Fresh pre-startup test profile, Windows-only PATH and bundled native SDKs: 1280 x 720 frame, clean Save while stopped, unchanged source, no Python RTX imports and normal exit |
| Missing runtime / omission | Both isolated packaged-app checks passed; replacing a deploy with -OmitRtx removed stale native files |
| Upgrades | Tests passed settings migration, valid/malformed legacy-copy exclusion, old saved-layout restoration, independent duplicate cameras and deferred GPU initialization |
| Difficult MDL content | Open; ten-minute first-frame failure above |
| Content/driver matrix | Broader materials, alpha/color, multi-GPU, stability and additional drivers unverified |

The implementation cutover does not close remaining content acceptance items.
Record additional evidence here as runs complete.
