# RTX Light Intensity — Plan

A stage lit for the Storm viewport renders almost black in RTX Render, and a stage lit
for RTX (Omniverse-style intensities in the thousands) is blown out in Storm. This plan
measures the difference and adds an option that makes RTX Render show a stage at Storm's
brightness, with a multiplier on top.

Status: phase 1 (the camera) is built. Phase 2 (scaling lights) waits until phase 1's
effect has been tried on real scenes. Measured on 2026-11-12 with OpenUSD 0.26.08
(`usd_typhoon`), ovrtx 0.6.0 / ovstage 0.3.0 as pinned in
[deps/rtx-native.json](../deps/rtx-native.json), an RTX 4090.

## 1. What RTX does today

- **RTX isn't a Hydra render delegate.** [RTX_RENDER.md](RTX_RENDER.md): the plugin
  flattens the stage into a snapshot, adds a private camera and RenderProduct
  (`makeSnapshot()`, [stage_bridge.cpp](../src/plugins/rtx_render_native/stage_bridge.cpp)),
  and ovrtx reads UsdLux from that file itself. usdcc has no light code for RTX.
- **The private camera has no exposure attributes,** so ovrtx uses its own camera
  defaults (§2.2). A scene camera's exposure isn't copied either.
- **The panel shows `LdrColor`,** ovrtx's tonemapped 8-bit image (§2.4).
- **Storm** ([hdSt/light.cpp](https://github.com/PixarAnimationStudios/OpenUSD/blob/dev/pxr/imaging/hdSt/light.cpp),
  `_ApproximateAreaLight()`) treats every light as a point or direction:
  `intensity × 2^exposure × color`, then, only when `normalize` is **off**, × the light's
  facing area (πr² for sphere and disk, w×h for rect, length×radius for cylinder) or, for a
  distant light, its solid angle 2π(1 − cos(angle/2)). Local lights fall off with 1/d².
- **The usdcc viewport** renders through `SetCameraState()`, so Storm never applies a scene
  camera's exposure. It also always adds a camera light and a 0.15 ambient, and leaves
  `colorCorrectionMode` empty, so it shows linear values with no display transform
  (`HydraViewportWindow::renderNow()`; read from the code, not checked by a screen grab).

## 2. Measurements

### 2.1 Method

- **The scene:** one stage per case, with a radius-1 sphere at the origin. Its
  UsdPreviewSurface has diffuse 0.5, roughness 1 and ior 1, so it has no specular. A
  camera sits 10 units out on +Z. The case's light points at the sphere from +Z. Local
  lights sit at (0, 0, 4) with `visibleInPrimaryRay = false`.
- **The value read:** the 3×3 pixels at the image centre, where the surface faces the
  camera and the light.
- **Storm:** `usdrecord --renderer Storm --colorCorrectionMode disabled
  --disableCameraLight`, written to EXR and read back as linear floats. The values match
  the formula in §1 to 4 digits (distant, intensity 1, normalize off: 1.067e-5 =
  0.5/π × 6.72e-5).
- **RTX:** the ovrtx 0.6.0 Python package, the same runtime the plugin loads. Each stage is
  wrapped in a layer that sets the render mode, and the RenderProduct has an `HdrColor`
  (linear, float16) and an `LdrColor` var. Path Tracing ran 64 spp. Real-Time results are
  within 2% of Path Tracing, so only Path Tracing is listed.
- These are offline renders, not the app's panel. They use the same SDK build and the same
  snapshot shape (private camera without exposure, RenderProduct).

### 2.2 RTX's camera exposure

Writing USD's camera exposure attributes on the camera changes RTX's linear output by
exactly the expected amounts:

| Authored on the camera | RTX `HdrColor` (distant light, intensity 1) |
|---|---|
| nothing (today) | 4.417e-4 |
| `exposure:fStop = 1` | × 25 |
| `exposure:time = 1` | × 50 |
| `exposure:iso = 1000` | × 10 |
| `exposure:responsivity = 2` | × 1.814 |
| fStop 1, time 1, ISO 100, responsivity 1 | 0.5005, the white-surface value |
| any of the above plus `exposure = 3` | no change: ovrtx 0.6 ignores the EV attribute |

So with no attributes authored, ovrtx uses f/5, 1/50 s, ISO 100 and a responsivity of about
1.1025 (2 / 1.814). That is a scale of **k = 0.02 × 1/25 × 1.1025 = 8.82e-4**, about 1/1134
or −10.15 stops, on everything: lights, dome and emission. With the attributes set to 1,
ovrtx's `HdrColor` is in the same units as Storm's output. `omni:rtx:post:tonemap:fNumber`,
`cameraShutter` and `filmIso` authored on the RenderProduct changed nothing.

### 2.3 Per light type, at unit camera exposure

RTX `HdrColor` divided by Storm. 1.0 means the two match.

| Light (intensity 1) | normalize off | normalize on |
|---|---|---|
| Sphere r 0.5 / r 1 | 1.003 / 1.003 | 0.251 / 0.251 |
| Sphere r 0.5, exposure 2 | 1.003 | — |
| Rect 1×1 / 2×1 | 0.967 / 0.919 | 0.967 / 0.919 |
| Disk r 0.5 | 0.976 | 0.976 |
| Dome, constant 1.0 texture | 0.951 | — (the spec ignores it) |
| Distant, angle 0.53° (default) | 46,900 | 3.147 |
| Distant, angle 0.53°, exposure 2 | 46,800 | — |
| Distant, angle 5° | 523 | 3.131 |

What the table shows:

- **Sphere, rect, disk and dome agree.** The 0.92–0.98 for rect and disk comes from Storm
  treating a big, close light as a point. It isn't a unit difference, and it goes away as
  the light gets smaller or further away.
- **Light `exposure` agrees.** Both renderers apply 2^exposure.
- **Normalized sphere lights:** RTX divides by the surface area 4πr², as the UsdLux spec
  says. Storm divides by πr², which leaves it 4× brighter.
- **Distant lights:** RTX ignores `normalize` and `angle`, and lights a surface facing the
  light with π × intensity, so a white surface shows the intensity. Storm follows the
  UsdLux spec: intensity × solid angle when not normalized, intensity when normalized.
  The ratios match π / (2π(1 − cos(angle/2))): 46,750 at 0.53° and 525 at 5°.

### 2.4 The displayed image

`LdrColor` goes through a filmic tonemap and display encoding. The usdcc viewport shows
linear values as they are. Measured centre values, as 8-bit:

| Linear value | 0.01 | 0.05 | 0.1 | 0.2 | 0.5 | 1.0 | 2.0 | 8.0 |
|---|---|---|---|---|---|---|---|---|
| RTX `LdrColor` | 12 | 59 | 99 | 149 | 206 | 232 | 245 | 255 |
| Storm viewport (linear) | 3 | 13 | 26 | 51 | 128 | 255 | 255 | 255 |

At the same linear radiance, RTX looks brighter in the midtones and rolls highlights off.
No intensity factor can match that over the whole range. The difference is in display
transforms, and §8 asks about it.

### 2.5 The factors

To make an RTX render show what Storm shows, at RTX's default camera:

| Light | Multiply RTX intensity by |
|---|---|
| Sphere, rect, disk (either normalize), dome, emission | 1/k ≈ **1134** (2^10.15) |
| Sphere, normalize on | 1134 × 4 = 4536 |
| Distant, normalize on | 1134 / π ≈ 361 |
| Distant, normalize off | 1134 × 2(1 − cos(angle/2)): **0.0243** at 0.53°, 2.16 at 5° |
| Light `exposure` | no change |

**One constant isn't enough.** One factor, RTX's camera exposure, covers most lights.
Distant lights depend on `normalize` and `angle`, and sphere lights on `normalize`. A
distant light at its default angle needs the opposite correction to everything else: it is
41× too bright in RTX today, while a sphere light is 1134× too dark. Example: UsdLux's
default distant light (intensity 50000) shows a grey of 0.53 in Storm. In RTX today the
same light gives 22, which is clipped white.

## 3. Options

| # | Option | For | Against |
|---|---|---|---|
| A | Scale every light's intensity in the snapshot by its §2.5 factor | One mechanism | Misses emission and anything else lit by the camera scale. Needs k hard-coded |
| B | **Author unit exposure × multiplier on the snapshot's private camera, and scale only distant and normalized sphere lights in the snapshot** | The camera covers lights, dome and emission in one value, and no k is hard-coded. Per-type fixes are three small rules | Two mechanisms |
| C | RTX tonemap settings on the RenderProduct | Would leave the scene alone | Measured to do nothing in ovrtx 0.6 (§2.2) |
| D | Edit intensities on the app stage | — | Changes the user's data. Ruled out by the task |
| E | Change Storm/the viewport instead | — | Storm is the reference people light in, and it follows the spec for distant lights |

## 4. Recommendation: option B, in RTX Render only

- **Where it applies:** in `makeSnapshot()`, on the flattened snapshot layer only. The app
  stage, its layers and undo stay untouched, as with the private camera today.
  - **Camera:** the private camera gets `exposure:fStop = 1`, `exposure:time = 1`,
    `exposure:iso = 100` and `exposure:responsivity = multiplier`. Not `exposure`, which
    ovrtx ignores.
  - **Lights:** every `UsdLuxDistantLight` and normalized `UsdLuxSphereLight` gets its
    `inputs:intensity` default and time samples scaled by the §2.5 rule. Lights inside
    instance prototypes are included.
- **Which panels:** only the RTX Render panel. The Hydra viewport (Storm, or any delegate
  picked in it) is the reference and doesn't change. V-Ray keeps its own units.
- **Settings** (Settings > RTX, group "Lighting"), defined in `defineRtxSettings()`:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `RtxRender/matchStormLights` | Bool | on | "Match Storm light intensities": RTX shows the stage at Storm's brightness (§2.5). Off keeps ovrtx's own camera exposure, right for Omniverse-authored scenes |
| `RtxRender/lightMultiplier` | Float 0.001–1000 (±10 stops; the dialog shows three decimals) | 1.0 | "Intensity multiplier", used while matching is on. 1 is Storm's brightness, 2 is one stop brighter |

- **A change reloads the snapshot,** as the frame and sample budgets do.
- **Per-panel override:** the options menu (☰) gets "Match Storm Lights" (checkable) and
  "Light Multiplier…", in the style of `ViewportDisplayOverrides`: unset follows Settings,
  and a set value is kept. Duplicate copies it. RTX panels save no session state today
  (`saveSessionState()` isn't overridden), so the override lasts as long as the panel.

## 5. Decisions taken as defaults

| # | Question | Default |
|---|---|---|
| 1 | Match Storm, or match the UsdLux spec? (They differ: Storm follows the spec for distant lights, RTX for normalized spheres) | Storm, because it is what users light against in usdcc |
| 2 | On by default? | On (the user's choice), so a Storm-lit stage renders visibly in RTX at once. RTX-authored scenes need it turned off |
| 3 | A followed scene camera's own `exposure:*` attributes | Ignored, as the usdcc viewport ignores them. See §8 |
| 4 | Multiplier as a factor or in stops | Factor, as asked. The description gives the stops |
| 5 | Settings change applied by in-place write or reload | Reload. An in-place write of `exposure:responsivity` through `RtxEngine::write()` can come later |
| 6 | Distant light with angle 0 | Storm shows nothing (zero solid angle). Use factor 0, which matches Storm, and log it once |

## 6. Phases

| Phase | Content | Files | Verification |
|---|---|---|---|
| **1. Camera base** *(done)* | `LightMatch {bool on; double multiplier;}` passed to `makeSnapshot()` and kept on `Snapshot`. Unit exposure × multiplier on the private camera. The two settings, read by the panel, with a change calling `reload()` | `stage_bridge.h/.cpp`, `render_types.h`, `rtx_panel.cpp`, docs/SETTINGS.md §4, docs/RTX_RENDER.md | §7 unit tests. By eye: a Storm-lit sphere-light scene looks alike in both |
| **2. Per-type rules** *(on hold until phase 1 is tried)* | `lightIntensityScale(UsdPrim)` (Qt-free, no SDK), applied to the flattened layer: default and time samples, prototypes too | new `light_match.h/.cpp` in `src/plugins/rtx_render_native`, `stage_bridge.cpp`, tests/CMakeLists.txt (add the source to `usdcc_python_tests`) | §7 unit tests. The 0.53° and 5° distant cases match |
| **3. Per-panel override** | Options-menu entries, override struct, Duplicate | `rtx_panel.cpp`, docs/RTX_RENDER.md (controls table) | Panel test |
| **4. GPU check** | Fixture with a distant, a normalized sphere and a sphere light. The probe adds an `HdrColor` var and checks the centre value against Storm's formula within 5% | `test_data/rtx_light_match.usda`, `tests/rtx_native_sdk_probe.cpp`, `src/plugins/rtx_render_native/CMakeLists.txt` (`rtx_gpu_light_match`, labels `rtx;gpu`) | `ctest -L gpu`, opt-in like the other `rtx_gpu_*` |

Phases 1 and 2 are independent. Phase 3 needs 1. Phase 4 needs 1 and 2.

**Phase 1 as built.** `LightMatch` is in `render_types.h`, and `makeSnapshot()` takes it as
a last argument that defaults to off. The setting itself defaults to on. The setting keys and the multiplier's range are in
`rtx_panel.h`. With matching on, the private camera gets `exposure:fStop` 1, `time` 1,
`iso` 100 and `responsivity` = the multiplier, and never `exposure`. Turning matching on or
changing the multiplier reloads open panels. Distant and normalized sphere lights keep the
§2.3 differences until phase 2.

## 7. Tests

| Suite | File | Covers |
|---|---|---|
| `usdcc_python_tests` | `rtx_native_stage_test.cpp` | Off: the private camera has no `exposure:*` (today's snapshot). On: fStop 1, time 1, ISO 100, responsivity = multiplier. The source stage is unchanged |
| `usdcc_python_tests` | `rtx_native_stage_test.cpp` | Rules: distant at 0.53°, 5° and 0, normalized and not. Normalized sphere × 4. Rect, disk, dome, cylinder and a light's `exposure` untouched. Time samples scaled. A light in an instance prototype scaled |
| `usdcc_python_tests` | `rtx_native_panel_test.cpp` | The settings are defined with their ranges. A change reloads (fake engine). The per-panel override wins over Settings, and Duplicate copies it |
| GPU, opt-in | `rtx_native_sdk_probe.cpp` | Phase 4 |

## 8. Open questions

1. **Display transform.** With the radiance matched, RTX still looks brighter in the
   midtones (§2.4). Should RTX present `HdrColor` with the viewport's transform? Or should
   the viewport get sRGB color correction? Either is its own change.
2. **The viewport's camera light and 0.15 ambient** are always on, so Storm in usdcc shows
   more light than the scene's lights alone. Should the viewport drop them when the stage
   has lights, as usdview does?
3. **Scene camera exposure.** When RTX follows a camera prim with authored `exposure:*`,
   should its `ComputeLinearExposureScale()` (EV folded into responsivity) multiply in, even
   though the viewport ignores it?
4. **Not measured:** cylinder lights (Storm uses length×radius, the spec 2πrL), MeshLightAPI,
   portal lights, IES and shaping, Minimal mode, emissive materials (expected to follow the
   camera scale), and other GPUs and drivers.
5. **SDK upgrades.** k, and the responsivity of 1.1025 behind it, are ovrtx 0.6.0 defaults.
   With option B nothing depends on them while matching is on, but §2 should be measured
   again when the pin changes. Phase 4 covers the matched case.
6. **The reverse direction:** showing an RTX-lit stage correctly in Storm would need a
   viewport exposure. It is out of scope here.
