# Viewport HUD

Status: **done**, all phases (§7), with the proposed answers to §8. Planned and built
2026-11-12 against master `2f171fa`. §9 says what was built where, and where it differs from
the plan.

A heads-up display: text and a few small controls drawn over the Hydra viewport, in its
corners. It says what the viewport is showing and how (camera, renderer, shading), what is
in front of it (counts), how fast it draws, and what a tool drag is doing. A few of its
items can be clicked.

[VIEWPORT_DISPLAY.md](VIEWPORT_DISPLAY.md) left the HUD out; this picks it up.

## 1. What exists

- **usdview's HUD** (Display > Heads-Up Display): plain text in the four corners. Upper
  left: counts for the selection (prims, CVs, vertices, faces). Lower left: complexity,
  camera, renderer. Lower right: frame and fps while playing. Upper right, optional: GPU
  stats. Nothing in it is clickable. *To re-check against `stageView.py` before phase 1.*
- **Maya, Houdini, Blender**: the same corners, plus a view-axis widget (an orientation
  cube or axis triad) that snaps the camera to a side when clicked, and clickable camera
  and shading labels at the top left (Houdini, Blender).
- **usdcc today**: the panel toolbar above the viewport (stage combo, options menu) and the
  options menu with Renderer, Shading, Complexity, Purposes, Lights and the rest. Nothing
  is drawn over the frame but the grid, the selection overlay and the gizmo.

## 2. What it could show

By corner. **v1** marks what I'd build first (§7); the rest are candidates.

### 2.1 Top left: how it's drawn — *v1, clickable*

```
Persp ▾   Storm ▾   Smooth Shaded ▾
```

- **Camera**: "Persp" (the orbit camera). Once the viewport can look through a stage's
  `UsdGeomCamera`, its name; clicking lists Persp and the stage's cameras.
- **Renderer**: the display name. Clicking opens the Renderer menu.
- **Shading**: clicking opens the Shading menu. Greyed with "(Storm only)" when the renderer
  can't draw it, as the menu does.

These duplicate the options menu on purpose: the menu hides them, the HUD shows the state.

### 2.2 Top right: orientation — *v1, clickable*

An **axis triad** in the gizmo's colours (X red, Y green, Z blue), turning with the camera,
on the stage's up axis (Y or Z, `UsdGeomGetStageUpAxis`, as the grid).

- Click an axis end: the camera looks down that axis (front, side, top and their opposites),
  keeping its target and distance. Only yaw and pitch change, via `setCameraState()`.
- Click the centre: back to the default yaw and pitch (`kDefaultOrbitYaw`/`Pitch`).
- Hover: the axis end under the cursor brightens; a tooltip-like label says "Top", "Front"...

An orientation **cube** (faces, edges, corners) is the bigger version; §8 asks which.

### 2.3 Bottom left: what's in front of it — *v1 partly*

```
Prims 1,204   Meshes 312   Faces 2.1M   Points 1.3M
Selection: 3 prims   Faces 4,812
Y up · 0.01 m/unit                        Edit target: shot_anim.usda
```

- **Stage counts** (optional, `hudStats`): prims, meshes, faces, points. Counted on a
  background thread from a `UsdPrimRange` over the stage, cached, and recounted when a
  change notice resyncs something (`StageChanges::resyncedPaths`). Faces and points read
  `faceVertexCounts` and `points` lengths, which is the slow part on big stages: a cap or
  "≥" when it runs long.
- **Selection counts** (optional, same switch): a second line with the same four numbers
  for what's selected, shown only while something is (usdview shows only this one).
  - A selected prim counts with everything below it, as the viewport draws it; a prim
    selected along with one of its ancestors is counted once.
  - Only what the viewport draws: inactive and invisible prims, and purposes switched off
    (Guide, Proxy, Render), are left out, in the stage line too.
  - A native instance counts its prototype's prims, faces and points each time; point
    instancers count their prototypes times the instance count. The stage line counts the
    same way, so the selection never shows more than the stage.
  - Recounted when the selection changes (debounced ~100 ms, so drag-selecting doesn't
    recount on every step) and when a change notice touches a selected prim or anything
    below it. From the stage count's cache where it has the subtree's totals; on its own
    background job otherwise, cancelled when the selection changes again.
  - One prim selected: its type too ("Mesh · Faces 4,812 · Points 2,410").
- **Up axis and meters per unit**: small, always there; a common source of "why is it
  huge / lying down". *v1.*
- **Edit target**, only when it isn't the root layer (`Stage::editTargetLayer()`,
  `editTargetChanged`). Edits going somewhere unexpected is the mistake this prevents.
  *v1.*
- **Display warnings**: one line naming any display option that differs from the
  defaults and would make the view look wrong: "Proxy off", "Scene lights off",
  "Backfaces culled", "Scene materials off". *Candidate.*

### 2.4 Bottom right: how fast — *candidate*

```
16.2 ms · 61 fps
Converging 43%
```

- **Frame time**: `QElapsedTimer` around the work in `renderNow()`. It only means something
  while the view is moving: shown while frames keep coming, faded out after a second idle.
- **Convergence** for progressive renderers (Embree, path tracers): `IsConverged()`, and a
  percentage where the delegate reports one (`GetRenderStats()`, `percentDone`, delegate
  dependent).
- **GPU memory** (`GetResourceAllocation()`): off by default, behind its own toggle.
- **Time code / frame**: nothing yet, as there's no timeline. The slot is kept for it.

### 2.5 Centre and transient

- **Tool readout** while a gizmo drag runs, next to the cursor or bottom centre:
  "Move  ΔX 1.25  ΔY 0  ΔZ 0", "Rotate  Y 37.5°", "Scale  1.2". From the tool's start and
  current values; needs a small `Tool` addition (`readout()` returning a string). *Candidate,
  high value.*
- **Paused** badge while `isPaused()`. *v1.*
- **Empty viewport**: "No stage. File > Open, or drop a .usd file here." *v1* (the drop
  itself is a separate feature).
- **Rollover prim info** (usdview's Show Rollover Prim Info): path and type of the prim
  under the cursor, after the cursor rests ~400 ms (one pick, not one per mouse move).
  *Candidate, off by default.*

## 3. Interactive or not

Yes, but little, and only where the HUD shows state the user wants to change on the spot:

| Element | Click | Hover |
|---|---|---|
| Camera label | Camera menu | highlight |
| Renderer label | Renderer menu | highlight |
| Shading label | Shading menu | highlight |
| Axis triad | snap the view; centre resets | highlight, side name |
| Edit target | opens the Layer Editor on that layer | highlight |
| Everything else | nothing: clicks go through to tools and camera | — |

Not interactive: counts, stats, readouts. Making them clickable would steal clicks from the
viewport, where a click on empty space means "select nothing".

## 4. How it's drawn

**Child widgets don't work.** The viewport is a native `QWindow` in
`createWindowContainer()`; a plain widget on top vanishes behind it on Windows
([NOTIFICATIONS.md](NOTIFICATIONS.md) §2). Owned tool windows, as the toasts use, would work
but have to follow the panel through docking, floating and tab switches. Too fragile for
something drawn every frame.

**Drawn into the frame, from an image.** Proposed:

1. `buildViewportHud(const HudInput&) -> HudLayout` (Qt Gui only, no GL): the items, their
   text, colour and rect in the window, from the camera, renderer, counts, timings, size and
   device pixel ratio. Pure, unit-testable, like `buildViewportGrid()`.
2. `paintViewportHud(QPainter&, const HudLayout&)` paints it into a transparent `QImage`
   at the window's device pixels, in the app font, with a soft shadow behind the text so it
   reads on any background.
3. `HydraViewportWindow` keeps the image, repaints it only when the layout changed, and
   draws it as one textured quad with fixed-function GL after the gizmo, the same way the
   grid and gizmo are drawn. No `QPainter` on the GL context, so Hydra's GL state is never
   touched by Qt's paint engine.

Where in the frame: **after the blit to the window, not into `m_framebuffer`**, so
`grabFrame()` (session previews) stays a clean picture of the scene. `showLastFrame()` while
paused draws it too.

**Hit testing.** `HudLayout` carries each interactive item's rect. In `mousePressEvent()` the
HUD gets first refusal, before the active tool and the camera (the order becomes HUD → tool
→ camera). `mouseMoveEvent()` without a button updates the hover item; `QWindow` gets those
moves without mouse tracking. A click emits `hudMenuRequested(item, globalPos)`;
`ViewportViewPanel` builds the menu from the same code as its options menu and pops it up
(a top-level popup, so the native window isn't in its way).

**Cost.** Text changes a few times a second at most; the frame time line is the only one
that changes per frame and is throttled to 4 updates a second. One quad per frame otherwise.

## 5. Settings, overrides, sessions

Like the other display options ([VIEWPORT_GRID.md](VIEWPORT_GRID.md) §3–4):

| Key | Type | Default |
|---|---|---|
| `Viewport/hud` | Bool | on |
| `Viewport/hudCamera` (top-left labels) | Bool | on |
| `Viewport/hudAxes` | Bool | on |
| `Viewport/hudStats` (stage and selection counts) | Bool | off |
| `Viewport/hudPerformance` | Bool | off |
| `Viewport/hudToolReadout` | Bool | on |
| `Viewport/hudRollover` | Bool | off |

Each is overridable per viewport, saved in the session JSON, copied by Duplicate, and in the
Display... dialog. The options menu gets a **HUD ▸** submenu with the switches. A shortcut
toggles the whole HUD in the active viewport (none by default; a Shortcut setting, as Fit
to View). The Material Editor preview (`isPreviewMode()`) shows no HUD.

## 6. Python

Not in v1. Later: `viewport.hud_visible`, and a way for a plugin to add a line to a corner
(a callback returning text), which is how a render plugin would show its own stats.

## 7. Phases

| Phase | Contents | Verified |
|---|---|---|
| **0. Spike** | The textured-quad path with one fixed string, after the blit; HiDPI; paused | In the app: crisp text at 100% and 150% scale, on Storm and Embree; not in `grabFrame()` |
| **1. Layout and labels** | `buildViewportHud()`, top-left labels (not clickable yet), up axis and units, edit target, Paused, empty stage; settings and the HUD submenu | Tests: layout per input (sizes, corners, eliding at narrow widths); in the app: each item |
| **2. Interaction** | Hit testing and hover; the three label menus; edit target → Layer Editor | Tests: hit rects; in the app: menus open at the label, a click beside a label still selects |
| **3. Axis triad** | Drawn, turning, snapping and resetting; Y- and Z-up | Tests: the yaw/pitch for each axis; in the app: each snap on both up axes |
| **4. Counts** | Background counter, per-subtree cache, recount on resync; selection counts with debounce and cancel | Tests: stage and selection counts on a test stage (nested selection counted once, instances, purposes off), after adding a prim, after changing the selection; in the app: a big stage stays responsive while drag-selecting |
| **5. Performance and readout** | Frame time, convergence, `Tool::readout()` for Move/Rotate/Scale | In the app: readout during each drag; convergence on Embree |
| **6. Rollover** | Delayed pick and the label | In the app |

Each phase ships on its own; 0–3 make a useful v1.

## 8. Open decisions

1. **Which items in v1?** Proposed: §2.1 labels, axis triad, up axis/units, edit target,
   Paused, empty stage; stats and performance in v1 but off by default.
2. **On by default?** Proposed: the HUD on, with only the quiet items.
3. **Axis triad or orientation cube?** Proposed: triad, smaller and enough to snap to six
   views; the cube's edge and corner views can come later.
4. **HUD in session previews?** Proposed: no (§4).
5. **Clickable labels at all, or show-only like usdview?** Proposed: clickable (§3); they
   save a trip through the options menu for the three things changed most.

## 9. What was built

All phases, with the answers proposed in §8: the v1 items on by default (labels, triad, up
axis and units, edit target, Paused, the empty-viewport hint, the tool readout), counts,
performance and the prim under the cursor in but off; a triad, not a cube; no HUD in session
previews; clickable labels.

| Piece | Where |
|---|---|
| Layout, hit testing, painting | `buildViewportHud()`, `hudItemAt()`, `paintViewportHud()` in `usdcc/usd/viewport_hud.h` (Qt Gui, no GL) |
| Drawing over the frame, hover, clicks, rollover, frame time | `HydraViewportWindow::drawHud()`, `refreshHud()`, `snapToAxis()`, `hudInput()` |
| Counting | `SceneCountJob`, `countScene()`, `countRoots()` in `usdcc/usd/scene_counts.h` (Qt-free) |
| Driving the counts, the labels' menus, the HUD submenu and dialog rows | `ViewportViewPanel` (`populateHudMenu()`, `recountStage()`, `recountSelection()`, `runCounts()`) |
| Snap angles | `orbitAnglesLookingFrom()` in `usdcc/usd/orbit_camera.h` |
| Tool readout | `Tool::readout()`, set by Move, Rotate and Scale; `formatReadoutNumber()` |
| Settings | `Viewport/hud*` (§5), `Shortcuts/viewport.toggleHud` (Toggle HUD, no key by default) |

Tests: `scene_counts_test.cpp` (the counting rules: nested selection, invisible, inactive,
purposes, native instances, point instancers, slices), `viewport_hud_test.cpp` (layout, hit
testing, formats, settings, session JSON, the panel's counts and recount, the labels' menus,
the edit target, Toggle HUD, snapping), the readout in `move_tool_test.cpp`. In the app
(Storm, 125% UI scale): the labels, triad, counts, edit target and units drawn crisp over the
frame; a click on the triad's +Y end gives the top view; a click on the renderer label opens
the Renderer menu under it; the frame time shows while frames come and goes after; the HUD
off leaves the frame as before. Not checked in the app: hover highlights, the rollover and a
drag's readout (they need the real cursor; the layout tests cover what they show), Embree's
convergence line, a light background.

Where it differs from the plan:

- **Counting runs on the main thread, in slices** of about 8 ms, not on a background thread.
  USD can't be read on one thread while the main thread edits it, and edits come from
  anywhere (tools, undo, scripts). A big stage takes longer to count but the UI stays
  responsive; there's no cap. The selection is counted before the stage, and on its own:
  there's no per-subtree cache shared with the stage count.
- **The edit target opens a menu of the stage's layers** (the edit target checked; pick one
  to make it the edit target) rather than the Layer Editor, which has no way yet to be
  opened on a given layer.
- **The readout sits beside the cursor**, as the rollover does, not bottom centre, where it
  would run into long count lines.
- **The top and bottom views put X to the right** (yaw 0): any yaw looks the same way
  there, and keeping the current one gave a turned top view.
- **Values at the earliest time**, as the viewport draws them, since there's no timeline.
- The **Camera menu** has Persp (the only camera so far), Fit to View and Reset Camera.
- **Plates, not shadows.** Each corner's text (top left, bottom left, bottom right, and the
  view name under the triad) sits on a semi-transparent black plate (`HudLayout::plates`),
  as the triad does on its disc and Paused, the readout and the rollover on their own. The
  text on them is always light, whatever the background; only the empty viewport's hint,
  on no plate, follows the background (light ink on dark, dark on light).
