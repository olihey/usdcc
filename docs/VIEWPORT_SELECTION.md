# Viewport Selection Style

The Hydra viewport draws what's selected in one of three **selection styles**:

| Style | Looks like |
|---|---|
| **Solid** | The prim filled with the selection color (the look before this feature) |
| **Outline** | The prim keeps its shading; a line in the selection color runs around its silhouette |
| **Wireframe** | The prim keeps its shading; its edges are drawn on it in the selection color |

The style, the selection **color** and the line **width** are Settings > Viewport defaults
that each viewport can override, like the grid and background
([VIEWPORT_GRID.md](VIEWPORT_GRID.md) §3, §4).

§2 was researched in November 2026 against the OpenUSD 0.26.08 this app builds with
(`USD_INSTALL`, `usd_typhoon`), using the Hydra 2.0 (scene index) engine path, which is the
default (`USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX`).

## 1. The styles

- **Color.** All three styles use the selection color, `#ffa600` (orange) by default. It is
  opaque: Solid always fills fully, and the line styles draw at full strength.
- **Width.** Outline and Wireframe draw their lines this many pixels wide: 1 to 8, default 2,
  scaled with the UI scale and the screen's pixel ratio. Solid has no lines, so it ignores
  the width.
- **Solid:** Storm tints the prim in its shaders, in the selection color.
- **Outline**
  - The line is the width wide and runs outside the silhouette, so it doesn't cover the
    prim's edge pixels.
  - Where the selected prim is **visible**, the line is drawn at full strength.
  - Where it is **hidden** behind other geometry, the line is drawn at 35% opacity, so a
    selected prim behind a wall can still be found. See decision 3.
  - The line follows the combined silhouette of everything selected: two selected prims
    that overlap get one outline around both, because it is built from one mask.
- **Wireframe**
  - The edges Storm draws for the selected prims (`DRAW_WIREFRAME`, at the viewport's refine
    level) are drawn in the selection color. Implicit shapes (cube, sphere, cone) get the
    edges of their tessellation.
  - Storm draws them 1px wide, and the composite widens them to the width (§3). On a dense
    mesh seen from afar the edges are close enough to fill the prim; width 1 keeps them
    apart longest.
  - Edges on the far side of the prim, or behind other geometry, are not drawn.
- **Native instances** work in every style: selecting one instance draws only that one.
- **Picking and tools** don't change. Only the drawing of the selection changes.
- **No hover highlight.** A prim under the cursor is not highlighted, in any style. The
  engine's locate color stays unused.
- **No selection in the Material Editor's preview.** The preview viewport (preview mode)
  draws no selection, whatever the style, color or width (`params.highlight` is off there).
  It never creates an overlay engine, and it has no options menu, so it takes no overrides.

## 2. What Hydra offers

- **Storm draws selection inside its shaders.** `HdxSelectionTask` uploads a selection
  buffer, and every Storm shader mixes the selection color into the selected fragments
  (`ApplySelectionColor`, `hdx/shaders/selection.glslfx`), by the color's alpha, after the
  render's `overrideColor` (`ApplyColorOverrides`, `hdSt/shaders/renderPass.glslfx`).
- **Hydra has an outline, but not for Storm.** `HdxColorizeSelectionTask` can draw an
  outline (`enableOutline`, `outlineRadius`) from the primId AOV. The task controller
  creates that task only for renderers other than Storm, though, and in the Hydra 2.0
  path `UsdImagingGLEngine` exposes neither the task nor its outline settings. Only
  `SetSelectionColor()` reaches them.
- **Wireframe is a draw mode for the whole scene.** `UsdImagingGLRenderParams::drawMode`
  and `wireframeColor` apply to every prim the render draws, not to the selection only.
- **A render can be limited to some paths.** `UsdImagingGLEngine::PrepareBatch()` and
  `RenderBatch(paths, params)` draw only the prims under `paths` (the task controller's
  collection).
- **Native instances' geometry isn't under the instance.** In Hydra 2.0 their prototypes are
  propagated to `/UsdNiPropagatedPrototypes/...` (`UsdImagingTokens->niPropagatedPrototypesScope`)
  and drawn by an instancer there, so a collection rooted at the instance holds nothing.

So both line styles need a pass of our own.

## 3. How it's drawn

**A selection overlay pass** (`SelectionOverlay`, `src/cpp/usd/src/selection_overlay.cpp`,
private to `HydraViewportWindow`). Each viewport using a line style gets a second
`UsdImagingGLEngine`, the **overlay engine**. It renders a mask of the selection into a
framebuffer of its own, and two shader passes of ours turn the mask into lines over the
viewport's frame.

```
renderNow()
  main engine:    Render(pseudo root)                -> viewport FBO (color + depth)
                  highlight off in Outline / Wireframe: no tint
  drawGrid()
  drawSelectionOverlay()
    overlay engine: PrepareBatch + RenderBatch(roots) -> mask FBO (color + depth)
                    unlit, overrideColor black, selection white:
                    selected pixels are white, anything else drawn is black
                    Outline: DRAW_SHADED_FLAT   Wireframe: DRAW_WIREFRAME
    depth copy:     the viewport FBO's depth, blitted into a texture
    pass 1:         per pixel, the horizontal distance to the nearest "strong" and
                    "weak" mask pixel (-> RG16F)
    pass 2:         the 2D distance from pass 1's rows (an exact distance transform in
                    two passes), and the line from it, blended onto the viewport FBO
  drawGizmo()
```

- **Strong and weak pixels.** A mask pixel is selected when it's white. It is *visible* when
  its overlay depth is within 0.3% (of the distance from the camera) of the viewport's depth
  there: the same surface drawn by both engines, or a wire lying on it.
  - Outline: strong = selected and visible, weak = selected. The ring is drawn where a
    strong pixel is within the width, or at 35% where only a weak one is, and only outside
    the selection.
  - Wireframe: strong = a selected, visible wire pixel; no weak ones. Lines are widened to
    the width around them.
- **The render roots.** `SelectionOverlay::renderRoots()`: the selected paths, plus
  `/UsdNiPropagatedPrototypes` when the selection is, holds or sits inside a native
  instance. Which instances are selected is then told apart by the engine's own selection
  (`SetSelected()`, resolved per instance by Hydra's selection scene index), which is why
  the mask is drawn black with the selection white rather than everything white.
- **Engine.** The overlay engine is always Storm, whatever renderer the viewport uses. It
  is created the first time a line style draws a selection, kept while the selection is
  empty (so selecting again doesn't repopulate), and freed when the style goes back to
  Solid, with the main engine, or in preview mode. A new stage gives it a new engine
  (Hydra populates from one stage only). The selection is set after `PrepareBatch()`: one
  set before the engine has populated is lost.
- **What it costs.**
  - The overlay engine populates its own scene index and render index from the stage.
    The scene index reads the stage lazily, and Storm syncs, and builds GPU buffers for,
    only the prims in the collection.
  - Each frame does one extra render, of the roots only, and two full-screen passes, each
    reading 2 × (radius + 1) + 1 pixels per pixel.
  - Measured in November 2026 on an RTX 4090 at 1343×650 with 900 spheres (8.3M quads),
    two selected, GPU-finished frame times: Solid 5.0–5.3 ms, Outline 5.4 ms, Wireframe
    5.5 ms. Switching to a line style drew its first frame in under 4 ms.
- **GL state.** The passes use their own program, VAO and textures, and leave program 0,
  no textures, blending off and depth test on, for the gizmo's fixed-function drawing.
- **Color and width.** The main engine's `SetSelectionColor()` takes the selection color.
  The composite takes the color and the width in device pixels as uniforms, so changing
  either only redraws.

**Alternatives considered:**

- **A. The primId AOV, decoded on the CPU.** Read the main render's primId AOV, use
  `DecodeIntersection()` to find which ids are selected, and outline those pixels.
  Rejected: it reads the whole id buffer back every frame, and native instances need the
  instanceId resolved per instance as well.
- **B. A scene index that changes the selected prims' repr.** Register a filtering scene
  index for Storm (`HdSceneIndexPluginRegistry::RegisterSceneIndexForRenderer()` with a
  callback). It would set `legacyDisplayStyle.reprSelector = refinedWireOnSurf` on the
  selected prims, and Storm would draw their edges itself, exactly on the surface.
  Rejected:
  - it covers Wireframe only, not Outline;
  - it works with Storm only;
  - a native instance's prims are its prototype's, so selecting one instance would put the
    wireframe on every instance of that prototype.

  Worth trying if the depth tolerance in Wireframe shows artifacts.
- **C. `RenderBatch()` on the main engine.** One engine renders the scene, then renders the
  selection. Rejected: the collection would switch twice every frame, and Storm rebuilds
  its draw batches on each switch.
- **D. Patch OpenUSD.** Create `HdxColorizeSelectionTask` for Storm and expose its outline
  on `UsdImagingGLEngine`. Rejected: it means keeping a fork of OpenUSD, and it still
  gives no Wireframe.

## 4. Settings and per-viewport overrides

| Key | Type | Default |
|---|---|---|
| `Viewport/selectionStyle` | Choice: `Solid`, `Outline`, `Wireframe` | `Solid` |
| `Viewport/selectionColor` | Color | `#ffa600` |
| `Viewport/selectionWidth` | Int 1–8 (px) | 2 |

- **Definition.** `defineViewportDisplaySettings()` (`usdcc/usd/viewport_display.h`) defines
  the keys, beside the grid and background ones, under the Selection heading. Their labels
  are "Selection style", "Selection color" and "Selection line width".
- **Display options.** `HydraViewportWindow::DisplayOptions` has `selectionStyle` (an
  `enum class SelectionStyle { Solid, Outline, Wireframe }`), `selectionColor` and
  `selectionWidth`. `selectionStyleName()` / `selectionStyleFromName()` /
  `selectionStyleLabel()` convert a style to and from its session name and its label.
- **Overrides.** `ViewportDisplayOverrides` has `std::optional` `selectionStyle`,
  `selectionColor` and `selectionWidth`. They follow the same rules as the other values:
  the viewport follows a setting until it overrides it, and then keeps its own value. Each
  is overridden on its own, so a viewport can change only the color.
- **The options menu** has a **Selection** submenu with Solid, Outline and Wireframe as
  radio entries. Choosing one overrides the style for that viewport. Color and width are in
  the dialog only. Use Settings Defaults drops these overrides too.
- **The dialog**, **Display...** ("Viewport Display - <panel>", formerly Grid &
  Background...), has three rows for the selection, each with its own Default button:
  - Selection: a combo box.
  - Selection color: a swatch that opens the color picker. The viewport follows the picker
    live, and Cancel puts the color back, as the Background row does.
  - Line width: a spin box, 1–8 px. It is disabled while the style is Solid.
- **Sessions** save the overrides under `"display"`:

  ```json
  "display": {"selectionStyle": "outline", "selectionColor": "#33ccff", "selectionWidth": 3}
  ```

  A style that isn't one of the three, a color that isn't one, or a width that isn't a
  whole number from 1 to 8 is ignored.
- **Duplicate** copies the overrides with the others.

## 5. Phases

| Phase | Work | Status |
|---|---|---|
| **1. Settings and plumbing** | The three settings, style, color and width in `DisplayOptions` and the overrides, JSON, the Selection submenu, the dialog's rows and its rename | Done, with tests in `viewport_display_test.cpp` |
| **2. Overlay engine and Outline** | `SelectionOverlay`: the overlay engine and its roots, the depth copy, the two passes with visible/hidden opacity, highlight off in the main engine | Done. Seen in the app around a cube, a mesh, `/World` and one of two native instances, faint behind another prim, from the first frame |
| **3. Wireframe** | The overlay engine with `DRAW_WIREFRAME`, the depth-tested composite and widening to the width | Done. Seen on a cube, a cone instance and a sphere, back and hidden edges left out |
| **4. Docs and checks** | VIEWPORT_GRID.md §4, SETTINGS.md §4, the frame-time check (§3) | Done |

Not yet checked: the line styles with a Hydra renderer other than Storm in the viewport,
and whether Wireframe's lines flicker while orbiting.

## 6. Decisions

1. **One mechanism for both styles:** the overlay engine (§3). Alternative B stays the
   fallback for Wireframe.
2. **Solid is the default**, so nothing changes until a user picks another style.
3. **An outline behind other geometry is drawn faint (35%)**, not left out. *Open:* draw only
   the visible part, as Blender does, or offer both.
4. **The overlay engine is always Storm**, so the styles don't depend on the viewport's
   renderer. RTX Render and the render view panels have their own presentation and aren't
   affected ([RTX_RENDER.md](RTX_RENDER.md)).
5. **Color and width are options**, globally and per viewport, like the style. The color is
   opaque, and the width is one value for both line styles.
6. **No hover highlight**, in any style.
7. **No selection in the Material Editor's preview**, in any style.
8. **The overlay is drawn after the grid**, before the gizmo, so grid lines don't cross the
   outline and the gizmo stays on top.

## 7. Not done

- A separate width for Outline and Wireframe, and a translucent Solid (a color alpha).
  Either can be added on top of §4 later.
- A selected prim in front of a non-selected one that the overlay also draws (another
  instance of the same prototype, under `/UsdNiPropagatedPrototypes`) can have its mask, and
  so its outline, cut where that one covers it.

## 8. Tests

| Suite | File | Covers |
|---|---|---|
| `usdcc_ui_tests` | `viewport_display_test.cpp` | the three settings and their defaults, style names, the overrides' JSON (and what's ignored), the Selection submenu, the dialog's rows and Default buttons, the Display rename |

The drawing itself needs a GL context, which the test platform (offscreen) doesn't have; it
was checked in the app (§5).
