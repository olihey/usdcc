# Viewport Camera

Status: **phase 1 done** (looking through, Detach), phases 2–4 planned. Planned 2026-11-20
against master `566fb99`. §2's navigation default (Detach) is decided; §8 lists what is still
open, with proposed answers. §10 says what was built where, and where it differs from the plan.

A viewport can look through a `UsdGeomCamera` of its stage, not only through its own orbit
camera. Around that: a fixed image size (a **resolution gate**, 1920 × 1080 say) instead of
filling the viewport, a **render scale** (100, 75, 50 or 25 %), and **safe frames** drawn over
the gate.

[VIEWPORT_HUD.md](VIEWPORT_HUD.md) §2.1 left a slot for the camera's name in the HUD, and
[WORKSPACES.md](WORKSPACES.md) §3.5.5 a `"path"` key for a workspace's viewport; this fills both.

## 1. What exists

- **usdcc today**: one camera per viewport, the orbit camera (`HydraViewportWindow`: target,
  distance, yaw, pitch, orthographic size, `CameraView`; `orbit_camera.h`). `currentFrustum()`
  builds its frustum at the window's aspect, and `buildToolContext()` hands the same view and
  projection to the engine (`SetCameraState()`), the grid, the selection overlay, the gizmos and
  picking. The render buffer is always the window's size in device pixels.
- **The Render View panel** ([RENDER_VIEW_PANEL.md](RENDER_VIEW_PANEL.md) §2) already reads camera
  prims (`primRenderCamera()`, `cameraPrims()`) and has a Resolution choice: Fit Panel at full, ½
  or ¼, 1280 × 720 or 1920 × 1080.
- **usdview**: looking through a camera; any navigation switches to the free camera, starting
  where the stage camera was, which stays untouched. A gate mask, conformed with
  `CameraUtilFraming`.
- **Maya**: navigation moves the camera looked through (its transform, undoable); a camera can
  be locked. Resolution gate, film gate, safe action/title.
- **Houdini**: *Lock Camera/Light to View* off by default: navigating jumps to "No cam" and
  leaves the camera; on, it moves the camera. Safe area, mask opacity.
- **Blender**: *Lock Camera to View*, off by default; the same split as Houdini.

## 2. Navigating while looking through a stage camera

Moving a USD camera means authoring opinions: they can land in an unexpected layer, be hidden by
a stronger one, or fight the camera's time samples. A tumble shouldn't do that by accident. So
navigation has three modes, per viewport, and **Detach** is the default.

| Mode | A tumble, pan or dolly | Notes |
|---|---|---|
| **Detach** (default) | Switches to the free orbit camera, starting exactly where the stage camera is; the stage camera is not touched | §2.1 |
| **Drive** | Moves the stage camera: one undo step per drag, at the edit target | §2.2 |
| **Locked** | Does nothing; the HUD flashes "Camera locked" | For reviews and broadcasts ([VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md)) |

The mode is in the Camera menu (HUD label and options menu), kept in sessions and copied by
Duplicate. Selecting and gizmo drags work the same in all three: only the camera drags differ.

### 2.1 Detach

- The orbit state is set from the camera's view (§2.3) and the viewport is on Persp again, with
  the camera's field of view and projection. The HUD's camera label reads `Persp ← Cam`.
- **Back to Camera** (Camera menu) looks through it again. While detached, **Reset Camera** does
  the same, rather than going to the default Persp view; Reset Camera a second time does that.
- **Copy View to Camera** puts the stage camera where the free view is: one undo step, at the
  edit target, with the same checks as Drive (§2.2). Then it looks through it again.
- **New Camera from View** (also in Persp, any time) defines a `UsdGeomCamera` at
  `/Cameras/Camera1`, ... with the view's transform, focal length and aperture for its field of
  view, and clipping; undoable; then looks through it.
- Playing the timeline while detached keeps the free view: the stage camera's animation doesn't
  pull it back.

### 2.2 Drive

- A camera drag computes the new orbit pose as now, then writes it to the camera: the
  camera-to-world from §2.3, times the inverse of the parent's world transform at the current
  time, through `UsdGeomXformCommonAPI` (translate, rotate, no change to the op order).
- One undo step per drag, as a gizmo drag is (`UndoManager`), so the edits in between don't each
  become a step.
- **Not available**, with a toast saying why and the mode left on Detach for that drag, when:
  - the camera's transform has time samples (until there's auto-key, [ANIMATION.md](ANIMATION.md));
  - its xform ops aren't compatible with `UsdGeomXformCommonAPI` (a matrix op, say): rewriting a
    camera's op order is not something a tumble should do;
  - the edit target is weaker than an opinion on those ops, so the edit wouldn't show;
  - the camera is an instance proxy, or its layer can't be edited.
- The orbit target is kept by the viewport between drags; it's not authored.

### 2.3 Camera ↔ orbit pose

The orbit camera is target, distance, yaw and pitch. A stage camera is a matrix and lens.

- **Target**: `focusDistance` along the view when the camera has one; else the depth under the
  centre pixel (`drawnDepthAt()`), else the ground (`groundDepthAlongRay()`), else the distance to
  the centre of the stage's bounds along the view.
- **Yaw and pitch**: from the view direction, about the stage's up axis
  (`orbitAnglesLookingFrom()`).
- **Roll**: the orbit camera has none, so a rolled camera would snap level on the first drag.
  `orbitCameraAxes()` gets a `rollDegrees` (default 0), about the view direction: tumbling, panning
  and dollying keep it, Reset Camera and the axis triad set it to 0.
- **Lens**: the vertical field of view at the gate's aspect (§3), the clipping range, and
  orthographic with its aperture as the view's height.
- Round trip: camera → pose → camera gives the same matrix (to a tolerance), for both up axes
  and with roll. That is the main unit test.

## 3. Looking through a camera

- **Picking one**: the HUD's camera label lists Persp and the stage's cameras (`cameraPrims()`),
  as does the options menu's Camera submenu, then Back to Camera, Copy View to Camera, New Camera
  from View, the navigation mode, Fit to View and Reset Camera. The Outliner's context menu on a
  camera prim gets **Look Through** (the active viewport). Python: `Viewport.camera` (a path or
  `None`) and `Viewport.camera_navigation` (`"detach"`, `"drive"`, `"locked"`).
- **Evaluated at the viewport's time**: `UsdGeomCamera::GetCamera(m_time)` each frame it changes,
  so an animated camera plays; the stage's change notices for the camera or an ancestor redraw.
- **One set of matrices**: with a stage camera, `currentFrustum()` comes from its `GfCamera`
  (aperture offsets included), conformed to the gate (§4). `buildToolContext()` passes that on, so
  the engine, grid, selection overlay, gizmos and picking agree, as they do now for the orbit
  camera. The engine gets `SetCameraState()` as now, not `SetCameraPath()`: one path for both
  cameras, and the overlays can't drift from what Hydra draws. *Delegates that want the camera's
  physical attributes (depth of field, shutter) would need `SetCameraPath()`; §8.*
- **Fit to View** in a stage camera: detaches (or drives, in Drive) and frames, as in Persp.
- **The camera's own geometry** isn't drawn (Hydra draws no camera prims in Storm today), so it
  doesn't get in the way of its own view.
- **Gone**: a camera deleted, deactivated or no longer a camera switches the viewport to Persp at
  its last view, with a toast.
- **Sessions and workspaces**: the camera path and mode are in the session JSON
  ([SESSIONS.md](SESSIONS.md) §2.4). A path missing on restore falls back to Persp, with a toast.
  A workspace's `"path"` (a prim path or `"first"`, [WORKSPACES.md](WORKSPACES.md) §3.5.1) is
  read at last; its other keys still apply when the stage has no such camera.

## 4. Resolution gate

| Choice | The image is |
|---|---|
| **Fill Viewport** (default) | The viewport, as now |
| **1280 × 720**, **1920 × 1080**, **2048 × 858**, **3840 × 2160**, **1080 × 1080** | That aspect, as large as fits in the viewport, centred |
| **Custom…** | Width and height, each 16–16384 |
| **Render Settings** | The stage's `UsdRenderSettings` `resolution` (the stage's `renderSettingsPrimPath`, else the first one); only listed when there is one |

- The resolution only fixes the **aspect** in the viewport: the gate is fitted inside the window,
  not drawn 1:1. The number matters for the HUD, for render scale (§5), and for the Render View
  panel when it follows this viewport.
- **Outside the gate**: the scene is drawn there too (overscan) and darkened by a mask, opacity
  `Viewport/gateMaskOpacity` (default 0.7; 1 is solid). A thin line marks the gate.
- **How**: `UsdImagingGLEngine::SetFraming(CameraUtilFraming)` with the display window as the gate
  and the data window as the whole viewport, `SetOverrideWindowPolicy(CameraUtilFit)`; the
  projection for the overlays from `CameraUtilFraming::ApplyToProjectionMatrix()`. That's what
  usdview does.
- **Camera and gate of different shapes**: the camera's aperture is conformed to the gate with
  the render settings' `aspectRatioConformPolicy` when there are some, else USD's default
  (expand the aperture). The orbit camera keeps its vertical field of view, as now.
- **Fit to View** fits the gate, not the window.
- Picking and the pan rule (`panUnitsPerPixel()`) go by the gate's pixels: the cursor stays on
  what it grabbed.
- The HUD's camera label gets the size: `Cam · 1920 × 1080`.

## 5. Render scale

**100 %** (default), **75 %**, **50 %**, **25 %** of the device pixels, for slow delegates
(Embree, path tracers) and big screens (three 4K at 125 %).

- Hydra renders into `m_framebuffer` at the scaled size, with the framing scaled to match.
- That image goes to a full-size framebuffer: colour with linear filtering, depth with nearest
  (`glBlitFramebuffer` allows only nearest for depth).
- The grid, the selection overlay and the gizmos then draw into the full-size one at full
  resolution, against the upscaled depth, so lines stay sharp; the HUD after the blit, as now.
- Picking stays at full resolution: it's its own pass.
- `grabFrame()` (session previews) gets the full-size image.
- The HUD's label: `Cam · 1920 × 1080 @ 50 %` (no `@` at 100 %).

This is the riskiest step: the grid's depth test against an upscaled depth, and the selection
overlay's own engine, which renders at the framebuffer's size (`SelectionOverlay::Frame::size`).

## 6. Safe frames

Drawn over the gate (the whole viewport with Fill Viewport), in the HUD's pass so it's never in
`grabFrame()`; thin lines, the HUD's light ink on a dark edge.

| Overlay | Default | Setting |
|---|---|---|
| Action safe | 93 % (SMPTE ST 2046-1) | `Viewport/safeAction`, `Viewport/safeActionPercent` |
| Title safe | 90 % | `Viewport/safeTitle`, `Viewport/safeTitlePercent` |
| Centre cross | off | `Viewport/safeCenter` |
| Aspect guide | off; 2.39, 1.85, 1.78, 1.33, 1 | `Viewport/safeAspect` |

A **Safe Frames ▸** submenu in the options menu, rows in the Display… dialog. The percentages
are of the gate's width and height; a preset menu offers the legacy 90 / 80 as well.

## 7. Settings, overrides, sessions

Like the other display options ([VIEWPORT_GRID.md](VIEWPORT_GRID.md) §3–4): a default in
Settings > Viewport, overridable per viewport (`ViewportDisplayOverrides`), saved in sessions,
copied by Duplicate, in the Display… dialog.

| Key | Type | Default |
|---|---|---|
| `Viewport/resolution` | `"fill"`, `"WxH"`, `"renderSettings"` | `"fill"` |
| `Viewport/renderScale` | 100, 75, 50, 25 | 100 |
| `Viewport/gateMaskOpacity` | 0–1 | 0.7 |
| `Viewport/cameraNavigation` | `"detach"`, `"drive"`, `"locked"` | `"detach"` |
| `Viewport/safe*` | §6 | §6 |

The camera path itself is per viewport only: no default.

**Shared with the Render View panel**: the gate and scale math goes in a Qt-free
`usdcc/usd/camera_framing.h` (gate rect for a window and resolution, safe rects, scaled buffer
size, pose ↔ camera from §2.3), and the panel's Resolution choice moves onto it, gaining 75 %.

## 8. Open decisions

1. **Where the resolution comes from.** Proposed: per viewport, Fill Viewport by default, with
   Render Settings as one choice (§4), not taken over automatically when a stage has render
   settings.
2. **Safe frame values.** Proposed: SMPTE 93 / 90 by default, legacy 90 / 80 as a preset.
3. **Roll.** Proposed: add it to the orbit camera (§2.3) rather than levelling on detach.
4. **`SetCameraPath()` for delegates with physical cameras.** Proposed: not in this plan; matrices
   only (§3). Revisit for depth of field.
5. **Drive on animated cameras.** Proposed: refused until auto-key exists (§2.2).

## 9. Phases

Each ships on its own, as one PR.

| Phase | Contents | Verified |
|---|---|---|
| **1. Look through** | `camera_framing.h` pose ↔ camera with roll; `currentFrustum()` from a stage camera; Detach, Back to Camera, Copy View to Camera, New Camera from View; HUD label and Camera menu, Outliner Look Through, Python; sessions and the workspace `"path"` | Tests: round trip on both up axes with roll; the frustum matches `GfCamera`'s; detach keeps the view; Copy View is one undo step; session restore with a missing camera. In the app: an animated camera plays; a tumble detaches; grid and gizmo line up with the image |
| **2. Gate and safe frames** | Resolution choices, `CameraUtilFraming`, mask, Fit to View in the gate, safe frames, HUD size label; the Render View panel on the same math | Tests: gate rects per window and resolution, safe rects, conform policies. In the app: 1920 × 1080 in tall and wide panels; picking and pan inside the gate |
| **3. Render scale** | Scaled framebuffer, colour and depth up, overlays at full size | Tests: scaled sizes. In the app: 25 % on Storm and Embree, grid and outline sharp and in place, picking exact, frame time down |
| **4. Drive and Locked** | Writing the pose through `UsdGeomXformCommonAPI`, the refusals in §2.2, Locked | Tests: a drag is one undo step; each refusal; parent transforms. In the app: driving a camera under a moved parent |

## 10. What was built

### Phase 1: looking through, Detach (2026-11-20)

| Piece | Where |
|---|---|
| Roll | `orbitCameraAxes(..., rollDegrees)` in `usdcc/usd/orbit_camera.h`; `CameraState::roll` |
| Frustum through a camera, pose ↔ camera | `stageCameraFrustum()`, `orbitPoseFromFrustum()`, `orbitCameraToWorld()`, `cameraSeeingView()` in `usdcc/usd/camera_framing.h` (Qt-free) |
| Reading and setting camera prims | `stageCameraAt()`, `stageCameraPaths()`, `whyNotSetCamera()`, `setCameraFromView()`, `defineCameraFromView()` in `usdcc/usd/stage_camera.h` (Qt-free, undo-agnostic) |
| Looking through, Detach, losing the camera | `HydraViewportWindow::setStageCamera()`, `detachFromStageCamera()`, `stageCameraEdited()`; `currentFrustum()` and `clipping()` take the camera's |
| Menus, Back to / Copy View / New Camera, sessions, workspaces | `ViewportViewPanel::addCameraEntries()` (the HUD's Camera menu and the options menu's Camera submenu), `lookThrough()`, `backToCamera()`, `copyViewToCamera()`, `newCameraFromView()`, `viewportFor()` |
| Outliner | **Look Through** on a camera prim (`populatePrimMenu()`) |
| Python | `usdcc.app.Viewport.camera` (a path or None; settable) |

Tests: `stage_camera_test.cpp` (roll; the fit in wider and taller windows; camera → pose → the
same placement and window on both up axes and three aspects; the pose straight down; an
orthographic camera; a camera set from the view sees it, both projections, four aspects;
setting a camera keeps its ops, scale and order under a moved, turned parent; a matrix op and
no ops; the refusals: animated transform, animated lens, pivot ops; a stronger layer;
defining cameras and their names), `viewport_camera_test.cpp` (looking through, the HUD label,
a parent moving the view, Python's `camera` both ways and its ValueError; Detach keeping the
view and the roll, the HUD's "Persp ← Shot", Reset Camera twice, the triad forgetting the
camera, a deactivated camera; Copy View to Camera as one undo step that undoes, New Camera
from View; sessions, a saved workspace, a workspace's `"first"`). In the app (Storm, 125 %): a
2.39 camera with a 10° roll seen through, the grid lining up; one wheel step leaving it for
"Persp ← Shot" from exactly that view; looking through it again. Not checked in the app: the
menus by hand, Copy View with the real undo stack, an orthographic stage camera, an animated
camera playing.

Where it differs from the plan:

- **Copy View to Camera takes a matrix op too**, not only translate / rotate / scale: a camera
  with one `xformOp:transform` (as many DCCs write) gets the matrix. Other stacks (pivots,
  single-axis rotates, orients) are refused with the reason in the menu entry's tooltip.
- **The camera keeps its own scale** when it's set from the view: it changes nothing it sees.
- **A stronger layer is found after writing**: the edit is checked against what the stage
  makes of it and undone at once when it wouldn't show (`UndoManager::apply()` rolls back).
- **Copy View is offered only after a Detach**, for the camera left; New Camera from View
  only while looking through the orbit camera (through a camera, the view is that camera).
- **Persp, picked while looking through a camera**, goes back to the orbit camera where it
  was, rather than to Persp's default angles; the axis views are as before.
- **Not kept**: which camera the orbit camera detached from (sessions keep the camera looked
  through, the roll and the field of view). A camera renamed or moved in the hierarchy is
  lost (a toast says so), not followed.
- **The orbit target on Detach**: the camera's focus distance, the depth at the centre of the
  frame, the ground, the stage's bounds, then 10 units; a lens shift is dropped (the orbit
  camera is always centred).
- **A scaled camera**: its focus distance, clipping range and orthographic aperture are in its
  own space, so a camera that is scaled, or sits under a scaled parent (a camera rig in
  centimetres in a millimetre stage), has them scaled in the world, as Hydra draws it
  (`stageCameraScale()`). Copy View writes an orthographic aperture back in the camera's own
  space.
