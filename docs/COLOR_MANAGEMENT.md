# Color Management (OpenColorIO) — Plan

usdcc shows colors in three places that disagreed with each other: the Hydra viewport,
the render view panels (RTX, V-Ray) and the color pickers. This plan puts one OpenColorIO
setup behind all three: a config, a working color space, and a display and view that
every image goes through on its way to the screen.

Status: phase 1 (the service and the Hydra viewport) is built; phases 2–5 are plans.
Checked on 2026-11-16 against OpenUSD 0.26.08 (`usd_typhoon`, built with `--opencolorio`),
ovrtx 0.6.0 and the V-Ray App SDK 7. §1 describes usdcc before phase 1.

## 1. What happens today

| Where | What reaches the screen | Code |
|---|---|---|
| **Hydra viewport** (Storm and any other delegate) | Linear values as they are. `UsdImagingGLRenderParams::colorCorrectionMode` is left empty, which Hydra treats as `disabled`: the float16 color AOV is written to the 8-bit framebuffer without an sRGB encode. Linear 0.2 shows as 51/255, not 124 | `viewportRenderParams()` in [viewport_render_params.cpp](../src/cpp/usd/src/viewport_render_params.cpp) |
| **Material preview** (shader ball) | The same: it is a `HydraViewportWindow` in preview mode | [shading_preview.cpp](../src/cpp/usd/src/shading_preview.cpp) |
| **RTX Render** | ovrtx's `LdrColor`: its own filmic tonemap and sRGB encode, RGBA8, copied through CUDA into a Vulkan image and blitted to the swapchain | `makeSnapshot()` in [stage_bridge.cpp](../src/plugins/rtx_render_native/stage_bridge.cpp), [rtx_engine.cpp](../src/plugins/rtx_render_native/rtx_engine.cpp), [vulkan_window.cpp](../src/plugins/rtx_render_native/vulkan_window.cpp) |
| **V-Ray Render** | `VRayImage.toBitmapData()`: V-Ray's own 8-bit conversion (sRGB), RGBA8 over the helper's stdout | [vray_helper.py](../src/plugins/vray_render/python/usdcc_vray_render/helper/vray_helper.py), [client.py](../src/plugins/vray_render/python/usdcc_vray_render/client.py) |
| **Color pickers** (attribute editor) | A hard-coded sRGB transfer both ways, clamped to 0–1, and an 8-bit `QColorDialog` | `ColorSwatch::linearToDisplay()` / `displayToLinear()` in [color_swatch.cpp](../src/cpp/usd/src/properties/color_swatch.cpp), `NumericEditor` in [property_editors.cpp](../src/cpp/usd/src/properties/property_editors.cpp) |

So the three disagree:
- **The picker assumes an sRGB display, the viewport shows raw linear.** A mid grey picked
  as 128 is stored as linear 0.216 and drawn by Storm as 55.
- **RTX and the viewport differ in the midtones** even at matched radiance
  ([RTX_LIGHT_INTENSITY.md](RTX_LIGHT_INTENSITY.md) §2.4, open question 1).
- **V-Ray and RTX each apply their own display transform,** neither of them usdcc's.

What is already there:
- **OCIO 2.2.1 is in the USD build.** `usd_hdx.dll` links `OpenColorIO_2_2.dll`, and the
  headers and `OpenColorIO.lib` are in the install, so usdcc can link the same library.
- **OCIO 2.2 has built-in configs** (`ocio://default`, the CG and Studio configs), so usdcc
  needn't ship a `.ocio` file.
- **Hydra has an OCIO step.** `HdxColorCorrectionTask` runs on the color AOV after the render
  and before the present, for Storm and for every other delegate (`HdxTaskController` adds it
  in both of its render graphs). `UsdImagingGLEngine::SetColorCorrectionSettings()` and the
  render params' `colorCorrectionMode`, `ocioDisplay`, `ocioView`, `ocioColorSpace` and
  `ocioLook` drive it.

Three catches in Hydra's OCIO step (`colorCorrectionTask.cpp`):
- **It only uses OCIO when the `OCIO` environment variable is non-empty.** Otherwise mode
  `openColorIO` silently does nothing.
- **It takes the config from `OCIO::GetCurrentConfig()`,** the process-wide config, which
  OCIO reads from `$OCIO` the first time it is asked. There is no per-engine config.
- **Looks are broken:** given a look, it calls `setDisplay()` with the look's name instead
  of `setLooks()`, the transform throws, and Hydra quietly shows the image uncorrected. So
  usdcc passes no look; a view's own looks still apply.

And two in the built-in CG config (`ocio://default`):
- **Its `scene_linear` role is ACEScg,** not linear Rec.709. The working space has to be
  named (`lin_rec709`), not taken from the role.
- **Each display's first view is Raw,** so `getDefaultView()` gives Raw. usdcc picks
  Un-tone-mapped by name.

## 2. Terms

| Term | Meaning here |
|---|---|
| **Config** | The `.ocio` file or built-in config. One per process |
| **Working space** | The scene-linear space renderers compute in, and USD color values are in. Linear Rec.709 (sRGB primaries), what Storm, RTX and V-Ray assume by default: the config's `lin_rec709` (or a name it goes by), else its `scene_linear` role |
| **Display** | The monitor, e.g. `sRGB - Display` |
| **View** | The transform for that display, e.g. `ACES 1.0 - SDR Video` (filmic), `Un-tone-mapped` (plain sRGB encode), `Raw` (none) |
| **Look** | An optional grade on top of the view. Not offered: Hydra mishandles it (§1) |
| **Color picking space** | The config's `color_picking` role if it has one, else the display with an un-tone-mapped view. What swatches and the picker show |

## 3. The design

### 3.1 `ColorManagement`, one service

[color_management.h](../src/cpp/usd/include/usdcc/usd/color_management.h), in `usdcc_usd`,
Qt-free (it needs the USD install, which brings OCIO):
- Loads the config and makes it OCIO's current one, which Hydra uses.
- Lists displays and views per display, for the Settings page and the viewport's menu.
- `workingSpace()`, `defaultDisplay()`, `defaultView()` (Un-tone-mapped first), and
  `displayView(viewOverride)`: the settings' display and view, checked against the config,
  with a viewport's own view winning when the display has it.
- `toDisplay()`: one color through a display and view, on the CPU (tests now, swatches in
  phase 2).
- OCIO's messages go to the log under `usdcc.color`.
- Links `OpenColorIO.lib` found next to OpenUSD (`USDCC_OCIO_LIBRARY`): the same DLL Hydra
  uses. Without it, `USDCC_HAS_OCIO` is off, nothing loads, and viewports show sRGB.
- Later phases add CPU processors for the render panels and pickers, and the LUT bake for
  RTX (§5). Changes come through Settings' own `changed` signal; no signal of its own yet.

**The config, at startup,** before anything has used OCIO, in the same early step as
[usd_environment.cpp](../src/cpp/usd/src/usd_environment.cpp):
1. `$OCIO` set outside usdcc: it wins, and the Settings page says so (like the other
   environment variables, docs/USD_RUNTIME.md).
2. Otherwise the Settings value: a file, or a built-in config.
3. Default: `ocio://default` (the CG config).

usdcc then sets `OCIO` itself, so Hydra's check passes and the V-Ray helper inherits it.
The `OCIO` entry under "Left out on purpose" in USD_RUNTIME.md goes, and `OCIO` joins the
variables Settings > USD manages.

**Changing the config applies at the next start** (decision 4): Hydra only rebuilds its
OCIO shader when its params change, and the V-Ray helper reads `OCIO` when it starts. The
display and view apply at once.

### 3.2 Settings

A **Color Management** page (docs/SETTINGS.md §4):

| Key | Type | Default | What it does |
|---|---|---|---|
| `Color/config` | String, next start: a path or `ocio://` URI | `ocio://default` | The config, unless `$OCIO` is set. Its description says which config is in use and the working space, or why none is |
| `Color/display` | Choice from the config | the config's default display | Defined once a config has loaded |
| `Color/view` | Choice for that display | `Un-tone-mapped` if the display has it, else its default view | Decision 1. Its choices change with the display; a view the new display lacks gives way to its default |
| `Color/pickerSpace` | Color picking / Through the view | Color picking | Phase 2 (§7) |

No look setting (§1), and no working space setting: it stays linear Rec.709 until §8's
other working spaces.

A missing or broken config logs a warning, the viewport falls back to Hydra's sRGB encode,
and the config's row says why.

### 3.3 Per panel

Each viewport can override the view from its options menu, **View Transform**: *Default
(the settings' view)*, then the display's views. **Raw** shows linear values as before, for
checking numbers; without a config the menu offers only Default (sRGB) and Raw. The override
is `ViewportDisplayOverrides::colorView`, saved in sessions as `"colorView"` and copied by
Duplicate; *Use Settings Defaults* drops it. Not in the Display... dialog yet. Render view
panels get the same menu in phases 3 and 4.

## 4. The Hydra viewport

**Storm and the other delegates in the viewport go through the same step.**
`applyColorView()`, called by `viewportRenderParams()`
([viewport_render_params.cpp](../src/cpp/usd/src/viewport_render_params.cpp)):
- `colorCorrectionMode = openColorIO`, with `ocioDisplay`, `ocioView`, and
  `ocioColorSpace` = the working space. No `ocioLook` (§1).
- Without a working config, `colorCorrectionMode = sRGB`: Hydra's own sRGB encode.
- `disabled` only for the Raw view, rather than an identity OCIO pass.
- `HydraViewportWindow` redraws when a Color setting changes, so the material preview
  follows too.
- **Storm has to be asked for the color AOV** (`requestColorAov()`: `SetRendererAov(color)`
  after every renderer change, as usdview does). Without it Storm draws straight to the
  framebuffer, and Hydra's color correction, which only works on the color AOV, silently
  does nothing. Other delegates render to AOVs anyway.

Things that change with it:

| Thing | What happens | What to do |
|---|---|---|
| **Background** | `params.clearColor` fills the color AOV, so the view transform would change the user's grey | Built, two ways. **Storm:** the AOV clears to alpha 0, and Hydra's composite (premultiplied, `HgiInteropOpenGL`) lays the render over usdcc's own `glClear` background, which is already a display color (decision 5). **Other delegates:** this build's Embree fills its misses with the clear color at alpha 1, so the composite can't help; they get `opaqueClearColor()`, the background taken back through the view's inverse, which the view then shows as the background again (exact for Un-tone-mapped, within 1 code value for ACES) |
| **Selection highlight** | Hydra's selection task runs before color correction, so the Solid style's tint is transformed (lighter) | Accepted for now: it stays visible. The Outline and Wireframe styles are overlays, untouched |
| **Grid, outlines, gizmos, HUD** | Drawn after `Render()` into the framebuffer, in display space | Nothing: they stay UI colors |
| **Hidden Line** (two passes) | Both passes are corrected | Nothing |
| **A delegate whose color AOV is already display-referred** (8-bit) | It would be transformed twice | Built: after a renderer's first frame, `checkColorBuffer()` reads its color AOV's format. 8-bit: no correction, logged once. Storm (float16) and Embree (float32) get the transform. A delegate that tonemaps into a float AOV isn't detected |
| **Viewport snapshots, previews** | They read the framebuffer, already transformed | Nothing |
| **Brightness** | Everything looks brighter than today (the sRGB encode lifts the midtones) | Expected. Scenes lit to look right in today's viewport will look washed out; Raw keeps the old look. RTX_LIGHT_INTENSITY.md §2.4's Storm row changes |

**Not covered by Hydra:** texture color spaces. Storm reads `sourceColorSpace` (`sRGB`,
`raw`, `auto`) with its own code, not OCIO, and MaterialX's color management is its own
fixed set. Both assume a Rec.709 working space, which is why the working space stays
linear Rec.709 in phase 1 (§8).

## 5. RTX Render

RTX isn't a Hydra delegate, so §4 doesn't reach it. The plugin has to apply the transform.

1. **Ask ovrtx for `HdrColor`** (linear, float16) instead of `LdrColor` in `makeSnapshot()`.
   ovrtx's filmic tonemap is then out of the picture; RTX_LIGHT_INTENSITY.md measured that
   its tonemap settings do nothing in 0.6 anyway.
2. **The shared CUDA–Vulkan buffer** becomes RGBA16F (8 bytes a pixel), and the CUDA copy
   copies twice the bytes.
3. **Present with a shader instead of `vkCmdBlitImage`.** A small fullscreen pass (fragment
   or compute) that samples the frame, goes through a 3D LUT with a log2 shaper, and writes
   the swapchain image. The shader is fixed and precompiled to SPIR-V at build time (glslc
   from the Vulkan SDK); only the LUT changes. `ColorManagement` bakes the LUT (65³) from
   the working → display/view/look processor, and the window uploads it on `changed()`.
4. **Save Image** reads `HdrColor` back and applies the CPU processor for PNG / JPEG / TIFF.

| Alternative | Why not first |
|---|---|
| OCIO's exact GPU shader | OCIO writes GLSL text; Vulkan needs SPIR-V, so it means compiling at runtime (shaderc / glslang). Exact, but a big dependency. A LUT is within a fraction of a code value for SDR views |
| A CUDA kernel in the copy | The plugin uses the CUDA driver API without a compiler; a kernel needs PTX or NVRTC |
| Keep `LdrColor` | RTX would keep its own look and never match the viewport |

A per-panel option, **RTX Tonemap**, keeps `LdrColor` for anyone matching Omniverse output.
This closes RTX_LIGHT_INTENSITY.md open question 1: with the same transform on both sides,
matched radiance looks the same.

## 6. V-Ray Render

Two ways. A is recommended: it puts the transform in the shared panel, where
RENDER_VIEW_PANEL.md says things two renderers need belong.

| | A. usdcc transforms (recommended) | B. V-Ray transforms |
|---|---|---|
| How | The helper sends linear float16 RGBA. The `image` event gets `format: "rgba16f"`. `RenderViewPanel` applies the `ColorManagement` CPU processor on a worker thread and shows the 8-bit result | Pass V-Ray the config, display and view so `toBitmapData()` is already in display space |
| Matches the viewport | Exactly: same processor | Only if V-Ray's OCIO reads the config the same way. V-Ray's OCIO may not know OCIO 2.2's `ocio://` built-ins |
| Cost | Twice the bytes over the pipe (16 MB a 1080p frame). The helper already coalesces images, so a slow reader only skips frames. A CPU transform of 1080p, a few ms | None in usdcc |
| Save Image | Can write linear EXR too | 8-bit only |
| Open | How the App SDK gives float pixels (render element data, or `VRayImage` float access): to check | The App SDK's OCIO parameters: to check |

**V-Ray's render color space** (V-Ray 6+ can render in ACEScg) must be the working space:
Rec.709 primaries in phase 1, which is V-Ray's default. The helper sets it explicitly so a
user's V-Ray defaults don't change it.

## 7. Color pickers

**Yes, they are affected.** Today they assume sRGB; with OCIO they follow the config.

- **The numbers stay what USD stores:** working-space linear values. Nothing about authored
  data changes.
- **The swatch and the picker show the color picking space,** not the full view. A filmic
  view (ACES) compresses highlights and isn't a clean round trip: white picked through it
  doesn't come back as 1.0. That is why OCIO configs have a `color_picking` role, and why
  Blender and Maya pick that way. `Color/pickerSpace` = *Through the view* shows swatches
  as the viewport would, for matching looks; picking still goes through the picking space.
- **`ColorSwatch::linearToDisplay()` / `displayToLinear()`** become calls into
  `ColorManagement` (picking processor and its inverse). Without a config they stay the
  sRGB formulas they are now.
- **Values above 1** (emission, light colors × intensity): the swatch shows them clamped
  with a small marker, and the picker keeps the number fields as the way to type them.
  `QColorDialog` is 8-bit; an HDR picker (hue / saturation / value plus intensity) is later.
- **UI colors aren't scene colors.** The viewport background, selection and grid colors,
  theme colors and the Settings dialog's color pickers stay plain sRGB, with no OCIO.
- **Per-prim color spaces:** OpenUSD 0.26 has `UsdColorSpaceAPI` (`colorSpace:name`,
  inherited down the namespace) and `GfColorSpace`. Phase 1 assumes the working space for
  every color. Phase 4 reads the prim's color space and converts, so an ACEScg-authored
  color shows correctly.

## 8. Not now

- **A working space other than linear Rec.709** (ACEScg rendering). Every renderer would
  have to agree, and Storm's and MaterialX's texture handling don't follow OCIO.
- **Exposure and gamma controls in the viewport.** Hydra's task has neither; they would be a
  scale before the transform. Useful for RTX-lit scenes (RTX_LIGHT_INTENSITY.md §8.6).
- **HDR monitors** (scRGB or HDR10 swapchains).
- **Linear EXR export** from the viewport.

## 9. Decisions to make

| # | Question | Proposal |
|---|---|---|
| 1 | Default view | **Decided: `Un-tone-mapped`.** The plain sRGB encode, what the pickers already assume. ACES SDR is a click away in the view menu |
| 2 | Global setting plus per-panel override, or global only | Both (§3.3) |
| 3 | V-Ray: A or B (§6) | A |
| 4 | Config change while running, or on restart | **Decided: next start** (§3.1). Display and view apply at once |
| 5 | Background behind the render: composite (alpha 0) or inverse transform | **Decided: composite** |

## 10. Phases

| Phase | Content | Files | Verification |
|---|---|---|---|
| **1. Service and viewport** *(built)* | `ColorManagement` (config, lists, one-color transform), the `OCIO` variable at startup, the Settings page, render params, background composite, per-viewport View Transform menu, the 8-bit-delegate check | `color_management.h/.cpp`, `usd_environment.h/.cpp` (`setUsdEnvironmentVariable()`), `viewport_render_params.h/.cpp`, `hydra_viewport_window.h/.cpp`, `viewport_display.h/.cpp`, `viewport_view_panel.cpp`, `main.cpp`, `settings_dialog.cpp` (page order), `src/cpp/usd/CMakeLists.txt`, docs/SETTINGS.md, docs/USD_RUNTIME.md, docs/VIEWPORT_DISPLAY.md | `color_management_test.cpp` (`usdcc_tests`): the built-in config, working space, Un-tone-mapped first, a 0.2 grey to 0.4845 (124) and Raw to 0.2, a missing config, the settings and view choices per display, the render params with and without a config, the opaque clear color's round trip, the override's JSON. In the app: below |
| **2. Pickers** | Swatch through the picking processor, inverse on edit, HDR marker | `color_swatch.h/.cpp`, `property_editors.cpp`, docs/ATTRIBUTE_EDITOR.md | Unit: pick → store → show returns the same 8-bit color; existing picker tests updated |
| **3. Render view panel and V-Ray** | Float image path in `RenderViewPanel` (C++ and Python), the transform on a worker thread, per-panel view override; the helper sends float16 | `render_view_panel.h/.cpp`, `scripted_render_view_panel`, `ui.py`, V-Ray `protocol`, `vray_helper.py`, `client.py`, docs/RENDER_VIEW_PANEL.md, docs/VRAY_RENDER.md | V-Ray and the viewport side by side on a grey-card scene: same 8-bit values within 2 |
| **4. RTX** | `HdrColor`, RGBA16F buffer, LUT pass in Vulkan, Save Image, RTX Tonemap option | `stage_bridge.cpp`, `rtx_engine.cpp`, `vulkan_window.cpp`, `gpu_frame.h`, `rtx_panel.cpp`, new shader + CMake glslc step, docs/RTX_RENDER.md, docs/RTX_LIGHT_INTENSITY.md §2.4 / §8 | `rtx_gpu_*` probe: centre value of a known `HdrColor` through the LUT vs the CPU processor, within 1 code value |
| **5. Per-prim color spaces** | `UsdColorSpaceAPI` in the pickers | `property_editors.cpp`, `color_management.cpp` | Unit: an ACEScg-authored color shows as its Rec.709 equivalent |

Phase 1 comes first; 2, 3 and 4 each need only 1 and can go in any order. Phase 5 needs 2.

**Phase 1 in the app** (2026-11-16, a test profile, a card emitting linear 0.2 with no
reflection, read from session previews, which come from the viewport's framebuffer):

| Renderer | Un-tone-mapped | Raw | ACES 1.0 - SDR Video | Background (`#1c1c1e`) |
|---|---|---|---|---|
| Storm | 124 | 51 | 98 | 28, 28, 30 in all three |
| Embree | 124 | 51 | 98 | 28, 28, 30 in all three |

The grid behind the card stays hidden (depth still composites with the color AOV on). Not
checked in the app: the View Transform menu's clicks (the tests cover the override), a
config from `$OCIO` or a file, an 8-bit delegate, the material preview.
