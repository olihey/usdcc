# Viewport Display Options

Status: **done** (§5), with the choices in §6.

usdview's viewport has a **Display** menu with options that change how Hydra draws the
stage, and a **Lights** menu next to it. This looks at each one, says which usdcc's Hydra
viewport has too, and how they're built. Color management and the HUD are left out: they
are planned separately.

Researched in November 2026 against usdview's `mainWindowUI.ui`, `stageView.py` and
`viewSettingsDataModel.py`, and Hydra's `taskControllerSceneIndex.cpp` (OpenUSD 26.03), and
usdcc's `HydraViewportWindow`, `SelectionOverlay` and `ViewportViewPanel`.

## 1. usdview's menus

### 1.1 Display

| Entry | What it does in usdview |
|---|---|
| Shading Mode | Wireframe, WireframeOnSurface, Smooth Shaded, Flat Shaded, Points, Geom Only, Geom Smooth, Geom Flat, Hidden Surface Wireframe. `params.drawMode`, except Hidden Surface Wireframe, which is two passes (§3.1) |
| Complexity | Low, Medium, High, Very High: `params.complexity` 1.0, 1.1, 1.2, 1.3, the subdivision refine level. `+` / `-` keys step it |
| Bounding Box | Show Bounding Boxes, Axis-Aligned, Oriented, During Playback, Use Extents Hints. The selection's box, passed as `params.bboxes` and drawn dashed by Hydra |
| Display Purposes | Guide, Proxy, Render: `params.showGuides`, `showProxy`, `showRender` |
| Heads-Up Display | *Out of scope* |
| Selection Highlighting | Never, Only when paused, Always; Highlight Color: Yellow, White, Cyan |
| Show Rollover Prim Info | A tooltip with the prim under the cursor: path, type, material |
| Enable Scene Materials | `params.enableSceneMaterials`. Off: everything in the fallback material |
| Default Material Settings... | The fallback material's ambient and specular (`GlfSimpleMaterial`) |
| Color Management | *Out of scope* |
| Background Color | Black, Grey (Dark), Grey (Light), White |
| Cull Backfaces (GL) | `params.cullStyle` = `CULL_STYLE_BACK_UNLESS_DOUBLE_SIDED` |
| Show Dome Light Textures | `domeLightCameraVisibility`: a dome light's map behind the scene |

### 1.2 Lights

| Entry | Default | What it does in usdview |
|---|---|---|
| Enable Scene Lights | on | `params.enableSceneLights`. Off: the stage's own lights are pruned from the render (Hydra's light pruning scene index), so only the default lights below light it |
| Enable Default Camera Light | on | A `GlfSimpleLight` at the camera, passed to `SetLightingState()` |
| Enable Default Dome Light | off | A `GlfSimpleLight` with `isDomeLight`, passed the same way. Hydra gives it its packaged default texture (`HdxPackageDefaultDomeLightTexture()`); usdview rotates it 90° about X on a Z-up stage |

usdview adds the default lights only in shaded modes: wireframe and points get none.

## 2. What usdcc had already

- **Background color**, with a color picker instead of four presets ([VIEWPORT_GRID.md](VIEWPORT_GRID.md)).
- **Selection highlighting**: Solid, Outline and Wireframe, any color, a line width
  ([VIEWPORT_SELECTION.md](VIEWPORT_SELECTION.md)). usdview's Never / Only when paused / Always
  are about playback, which usdcc doesn't have.
- **Dome light visibility**: `HydraViewportWindow::setDomeLightVisible()`, used only by the
  Material Editor's preview.
- **The pattern to add options with**: `DisplayOptions` (what the window draws),
  `ViewportDisplayOverrides` (what one panel sets, saved in sessions and copied by Duplicate),
  Settings > Viewport defaults, the options menu and the Display... dialog.

`HydraViewportWindow::renderNow()` hard-coded `DRAW_SHADED_SMOOTH`, complexity 1.0, proxy
purpose only, scene materials on, no culling, and the lighting: always a camera light, always
the stage's lights, no default dome, and a 0.15 scene ambient (usdview: 0.01). Picking used a
blank `UsdImagingGLRenderParams`, and Fit to View's bounds hard-coded default + proxy.

## 3. What usdcc has now

### 3.1 The options

1. **Purposes: Guide, Proxy, Render.** Default Proxy on only (Hydra's and usdview's). What's
   drawn, picked, outlined and framed agree:
   - `pickPrim()` tests intersections with the frame's params (purposes, draw mode,
     complexity, culling); Hidden Line picks its surfaces, which hide what's behind them.
   - `SelectionOverlay` draws the selection with the frame's purposes, complexity and
     culling, in its own draw mode and colors.
   - `sceneBounds()` (Fit to View) takes the purposes the viewport draws (`viewportPurposes()`).
2. **Lights: Scene Lights, Camera Light, Dome Light**, usdview's three, with its defaults.
   - **Scene Lights** is `params.enableSceneLights`. With it off and both default lights off
     the frame is black, as in usdview: it's what was asked for.
   - **Camera Light** is the light usdcc always had, made optional. Embree draws no distant
     lights, so it has no camera light; the entry says "Storm only" in its tooltip.
   - **Dome Light** is a `GlfSimpleLight` with `SetIsDomeLight(true)` and Hydra's packaged
     texture, turned 90° about X on a Z-up stage, as usdview does.
   - **Unlike usdview, the default lights stay in the line modes.** Storm shades a wire as it
     would the surface it's on, so without them Wireframe and Hidden Line were near black
     (seen in the app: only the stage's red key light lit the wires).
   - The **Material Editor's preview** keeps its own lighting (no default lights, its HDRIs,
     its stage's lights).
   - The 0.15 ambient stays.
3. **Shading**, a curated list rather than usdview's nine:

   | usdcc | `drawMode` | Notes |
   |---|---|---|
   | Smooth Shaded | `DRAW_SHADED_SMOOTH` | The default |
   | Flat Shaded | `DRAW_SHADED_FLAT` | Facets, for checking topology |
   | Wireframe on Shaded | `DRAW_WIREFRAME_ON_SURFACE` | |
   | Wireframe | `DRAW_WIREFRAME` | |
   | Hidden Line | two passes | usdview's Hidden Surface Wireframe, below |

   **Hidden Line**: `DRAW_GEOM_ONLY` first, for the depth (its color is cleared to the
   background again), then `DRAW_WIREFRAME`, which Hydra's composite depth-tests against that
   depth, so the edges behind a surface are left out. usdview pushes the surfaces back with
   `glPolygonOffset()`, which doesn't reach Hydra's own pipeline; here the depth pass renders
   with a projection that adds 0.0005 to every NDC depth (`kHiddenLineDepthBias`). Without
   it a wire and its own surface tie and the wire comes out dashed.

   **Only Storm draws them.** Any other renderer is drawn Smooth Shaded
   (`effectiveShading()`), and the Shading submenu is greyed out with a tooltip.

   Geom Only / Geom Smooth / Geom Flat are left out: they are "shaded without materials", which
   Scene Materials (5.) covers more clearly. Points too: §6 q2.
4. **Complexity: Low, Medium, High, Very High** (1.0-1.3), from the menu only: no shortcuts. Low
   (1.0) draws a subdivision surface's cage, as usdview does; the overlay uses the same value.
5. **Scene Materials.** For "is it the material or the geometry?" and for heavy scenes.
6. **Cull Backfaces.** `CULL_STYLE_BACK_UNLESS_DOUBLE_SIDED`. Finds flipped normals.
7. **Dome Light Textures.** The display options' `domeLightTextures`;
   `setDomeLightVisible()` sets it, for the Material Editor's preview.

### 3.2 Not doing

- **No additional selection highlight options**: no bounding boxes for the selection, no
  Never highlight, no highlight color presets. Solid, Outline and Wireframe with a color stay
  what the viewport offers.
- **No keys for Complexity.**
- **Show Rollover Prim Info.** A pick per mouse move and a tooltip: a feature of its own, and
  [VIEWPORT_SELECTION.md](VIEWPORT_SELECTION.md) chose no hover highlight.
- **Default Material Settings.** Rarely touched.
- **Use Extents Hints, Show During Playback.** usdcc always uses extents hints for bounds, and
  has no playback.
- **Background presets.** The color picker covers them.

## 4. How it's built

- **`HydraViewportWindow::DisplayOptions`** has `shading` (`ShadingMode`), `complexity`
  (`Complexity`), `showGuides` / `showProxy` / `showRender`, `sceneMaterials`,
  `cullBackfaces`, `domeLightTextures`, `sceneLights`, `cameraLight`, `domeLight`.
- **`viewport_render_params.h`**: `viewportRenderParams()` builds the
  `UsdImagingGLRenderParams` for the frame, picking and the overlay; `effectiveShading()`,
  `drawModeFor()`, `complexityValue()` and `viewportPurposes()` go with it.
- **`ViewportDisplayOverrides`** has an optional for each. Settings > Viewport has defaults for
  complexity, the purposes, culling and the lights (groups Display and Lights); shading, scene
  materials and dome textures have none, so without an override a viewport shows the built-in
  value, and the dialog's Default button says "Use the built-in value" for them.
- **Sessions** save them under the viewport's `"display"` key, only what's set:

  ```json
  "display": {"shading": "hiddenLine", "complexity": "high", "showGuides": true,
              "sceneMaterials": false, "domeLight": true}
  ```

  `shading` is `smooth`, `flat`, `wireframeOnShaded`, `wireframe` or `hiddenLine`;
  `complexity` is `low`, `medium`, `high` or `veryHigh`; the switches are bools.
- **The options menu**, below the renderer:

  ```
  Renderer              ▸
  ─────────────────────────
  Shading               ▸  Smooth Shaded / Flat Shaded / Wireframe on Shaded / Wireframe / Hidden Line
  Complexity            ▸  Low / Medium / High / Very High
  Purposes              ▸  ☐ Guide  ☑ Proxy  ☐ Render
  Lights                ▸  ☑ Scene Lights  ☑ Camera Light  ☐ Dome Light
  ☑ Scene Materials
  ☐ Cull Backfaces
  ☑ Dome Light Textures
  ─────────────────────────
  ☑ Show Grid
  Selection             ▸
  Display...
  Use Settings Defaults
  ```

- **The Display... dialog** has the same rows below the selection's, each with its Default
  button.
- **Python** has no access to a viewport's display options yet (nor to the grid's).
- **Off screen** — its dock tab behind another, its panel closed, or the main window minimized
  (`HydraViewportWindow::isOnScreen()`) — a viewport renders nothing, and a progressive renderer's
  own threads (Embree, a path tracer) pause with `PauseRenderer()`, or `StopRenderer()` where it
  can't pause. Back on screen, it resumes and renders at once with whatever changed meanwhile.
  Minimizing needs watching the top-level window: it doesn't take its native children's exposure.

## 5. Phases

| Phase | Contents | Verified |
|---|---|---|
| **1. Shared params** | `viewportRenderParams()`; `pickPrim()` and `SelectionOverlay` use it | Tests: the params for each option |
| **2. Purposes** | The three toggles, through picking, the overlay and `sceneBounds()` | Tests: bounds with guides; in the app: Proxy off, Render and Guide on shows the render cube and guide cone, not the proxy sphere |
| **3. Lights** | Scene, Camera and Dome Light, the Z-up turn | In the app: camera light off leaves the stage's red key; dome only; no lights is black |
| **4. Shading and complexity** | The draw modes, Hidden Line's passes and depth bias, the non-Storm greying, complexity | In the app: each mode; Hidden Line drops back edges and hides the grid behind surfaces, with Outline selection on top; Very High rounds the subdivided cube |
| **5. Toggles** | Scene Materials, Cull Backfaces, Dome Light Textures | Tests: menu, dialog, overrides |

Each has its Settings key (where it has one), override, session JSON, Duplicate, menu, dialog
and `viewport_display_test` cases. The app was checked by loading sessions with each mode's
overrides and reading back the session previews (`HydraViewportWindow::grabFrame()`), on Storm.
Not checked in the app: Cull Backfaces, Scene Materials and Dome Light Textures (the test stage
had nothing to show them), and Embree.

## 6. Decisions

1. **Settings defaults for some.** Taken: **purposes, complexity, cull backfaces and the three
   lights** have Settings > Viewport defaults; **shading, scene materials and dome textures**
   are per viewport only. All are saved in sessions and copied by Duplicate.
2. **Points shading mode?** Taken: **no**, until someone has point clouds to look at.
3. **Per viewport or per stage?** Taken: **per viewport**, so one panel can show proxies and
   another render geometry of the same stage.
4. **The default dome light's texture.** Taken: **Hydra's packaged one**, as usdview. The
   Material Editor preview's HDRIs (Neutral, Sky, Studio) would be the next step if it's wanted.

## 7. Renderer settings

Each Hydra render delegate lists settings of its own (`HdRenderDelegate::GetRenderSettingDescriptors()`:
a key, a name and a default), and some have one-shot commands. usdcc shows both, on the
Display options' model: app-wide defaults with a viewport's own overrides.

- **What a delegate offers** (`renderer_settings.h`): `rendererSettings(pluginId)` reads them from a
  delegate made for the purpose, as `renderDelegateShading()` does (no GPU or GL context), and keeps
  them. Bools, numbers and strings are shown (an unsigned as an Int, a token or asset path as a
  String); other types are left out, as is `domeLightCameraVisibility`, which Display's Dome Light
  Textures sets. Here: Storm 7, Embree 20 (its path tracer's `ty:` settings), Gatling 15.
- **Settings > Renderers:** a group per delegate, keyed `Renderers/<pluginId>/<key>`, defaulting to the
  delegate's own default, so Reset gives its value back. Defined when needed, not at start: making a
  delegate to list them takes Gatling about 0.7 s (its MDL runtime), Storm and Embree about 10 ms.
  A viewport defines its renderer's as it starts using it (so the saved values apply), and all of them
  are defined before the Settings dialog opens (`SettingsDialog::setBeforeShow()`, set in main.cpp).
- **A viewport's overrides:** per renderer, kept for a renderer that isn't in use. The viewport hands
  the current renderer's values to `HydraViewportWindow::setRendererSettings()`, which sets those that
  differ from the delegate's, and again whenever the engine gets a new delegate (a new stage, a switch
  of renderer), which starts from its defaults. A value it doesn't override follows its setting.
- **Renderer > Renderer Settings...** (the options menu and the HUD's Renderer label): a row per setting
  of the current renderer, with a Default button while it's overridden; rebuilt for another renderer.
- **Commands** (`GetRendererCommandDescriptors()`): listed under it in the Renderer menu, run with their
  arguments' defaults. Here only Gatling has one: Print Licenses.
- **Sessions** keep the overrides as `"rendererSettings": {"HdEmbreeRendererPlugin": {"ty:maxBounces": 8}}`;
  a renderer this installation hasn't keeps its values for the next save. Duplicate copies them.
- **Not done:** ranges and choices (delegates don't publish them), argument dialogs for commands,
  `UsdRenderSettings` prims, AOVs, and Python.
- **Tests:** `renderer_settings_test` (Embree's list and types, the conversions, the Settings
  defaults, overrides through a duplicate and a session, the dialog). What reaches the engine is
  checked in the app.
