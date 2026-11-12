# Viewport Grid and Background

The Hydra viewport draws a ground grid behind the stage, on a background color. Settings >
Viewport holds the app-wide defaults. Each viewport panel can override any of them.

## 1. The grid

- A square **size** units wide, centred on the origin, with a line every **step**.
- It shows only while the viewport has a stage. An empty viewport shows just its
  background.
- It lies on the stage's ground plane: XZ for a Y-up stage, XY for a Z-up one
  (`UsdGeomGetStageUpAxis`). The orbit camera itself is always Y-up.
- The lines through the origin are the axes, in the gizmo's colours: X red, Z blue (Y
  green on a Z-up stage). Every tenth line is brighter.
- Lines sit on whole multiples of the step. The grid ends at the last whole step inside
  half the size: size 5, step 1 draws lines from -2 to 2.
- The lines are light on a dark background and dark on a light one.
- **The line cap.** A step too fine for the size is coarsened by whole multiples so that
  at most 1001 lines are drawn each way (`kMaxGridLinesPerDirection`). A size or step
  that isn't positive draws no grid.
- **Depth.** The grid is drawn after Hydra, depth-tested against the depth Hydra's
  composite leaves in the viewport's framebuffer. Geometry in front of it hides it. The
  grid writes no depth of its own. A prim that reaches below the ground shows the grid
  lines across its lower part, because the ground there is in front of it.

The lines are built by `buildViewportGrid()` (`usdcc/usd/viewport_grid.h`, Qt- and GL-free)
and drawn by `HydraViewportWindow::drawGrid()` with the same fixed-function GL the gizmos
use.

## 2. Background color

- `glClearColor` and Hydra's `clearColor` render param both take it.
- The built-in default is `#1c1c1e`, the dark theme's panel surface.
- **RTX Render** ([RTX_RENDER.md](RTX_RENDER.md)) doesn't use it. That plugin is a
  separate panel with its own Vulkan presentation. It shows the path-traced scene,
  environment included, and its black clear only fills the area outside the image. The
  same goes for the render view panels (V-Ray, scripted renderers): their background is
  the renderer's.

## 3. Defaults: Settings > Viewport

| Key | Type | Default |
|---|---|---|
| `Viewport/showGrid` | Bool | on |
| `Viewport/gridSize` | Float 0.01–1000000 | 20 |
| `Viewport/gridStep` | Float 0.001–100000 | 1 |
| `Viewport/backgroundColor` | Color | `#1c1c1e` |

`defineViewportDisplaySettings()` (`usdcc/usd/viewport_display.h`) defines them.
`main.cpp` calls it at start, so the page is there before a viewport opens.

## 4. Per viewport

Each value is overridden separately. A viewport shows the defaults with its overrides on
top (`ViewportViewPanel::displayOptions()`).

- **Changing a default updates every viewport that doesn't override that value**, at
  once. A value a viewport overrides is kept, whatever the default becomes. This way
  Settings works as "the look of my viewports" without overwriting a choice made in one
  panel.
- A new viewport overrides nothing.

The options menu (☰, or pinned beside the panel) has:

| Entry | Does |
|---|---|
| Show Grid | Ticked while the grid shows. Toggling it overrides Show grid for this viewport |
| Selection | Solid, Outline or Wireframe, the one in use ticked. Choosing one overrides the selection style for this viewport ([VIEWPORT_SELECTION.md](VIEWPORT_SELECTION.md)) |
| Display... | Opens this viewport's dialog. Its rows are Show grid, Grid size, Grid step, Background (a swatch that opens Qt's color picker), Selection, Selection color and Line width. The viewport follows a picker live, and Cancel puts the color back. Each row has a **Default** button, shown while the value is overridden, that drops the override. Edits apply at once |
| Use Settings Defaults | Drops all of this viewport's overrides. Disabled when there are none |

**Duplicate** copies the overrides.

**Sessions** save them under the viewport's `"display"` key, only the values that are set:

```json
"display": {"showGrid": false, "gridSize": 50, "gridStep": 0.5, "background": "#202830",
            "selectionStyle": "outline", "selectionColor": "#33ccff", "selectionWidth": 3}
```

- A session without the key, such as an older one, restores no overrides.
- Values that don't fit are ignored: a size or step out of range, something that isn't
  a color, a style that isn't one of the three, or a width outside 1–8.

## 5. Tests

| Suite | File | Covers |
|---|---|---|
| `usdcc_tests` | `viewport_grid_test.cpp` | line positions, axes and major lines, the Z-up ground, the line cap, invalid sizes |
| `usdcc_ui_tests` | `viewport_display_test.cpp` | the settings (grid, background and selection), overrides kept and defaults followed, the options menu and its Selection submenu, the dialog, duplicate and session round trip |
