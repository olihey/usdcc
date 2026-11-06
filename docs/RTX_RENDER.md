# RTX Render

A view panel that renders its stage with NVIDIA's Omniverse RTX renderer, through
[ovrtx](https://nvidia-omniverse.github.io/ovrtx), NVIDIA's C and Python SDK for it. It
follows the viewport camera or a camera in the stage, and renders again after edits.
Moved prims, camera moves and size changes are written into the renderer's stage in place,
with no reload. It's a Python plugin, [src/plugins/rtx_render](../src/plugins/rtx_render):
- The panel is the app's render view panel ([RENDER_VIEW_PANEL.md](RENDER_VIEW_PANEL.md)).
- The plugin supplies ovrtx, which runs in a helper process, in a Python of its own.

ovrtx is pre-release, under NVIDIA's proprietary licence, and needs an NVIDIA RTX GPU with
a recent driver. usdcc doesn't ship it: each user installs it. The licence also forbids
publishing benchmark results without NVIDIA's permission, so this page gives no timings.

## 1. Setup

1. **Make a Python with ovrtx.** Use Python 3.11 to 3.13. The installed package is about
   2.5 GB:

   ```bat
   py -3.12 -m venv %LOCALAPPDATA%\usdcc\rtx\venv
   %LOCALAPPDATA%\usdcc\rtx\venv\Scripts\python -m pip install ovrtx ovstage numpy
   ```

   The plugin finds a Python there on its own. Otherwise set *Settings > RTX > Python with
   ovrtx*, or the `USDCC_OVRTX_PYTHON` environment variable.
2. **Load the plugin.** Run `usdcc --plugin-path src/plugins`, or copy the folder into
   `%LOCALAPPDATA%\usdcc\plugins`.
3. **Open the panel** from Stages > RTX Render, and press Render.

The first render on a machine compiles ovrtx's shaders, which takes a minute or two; the
panel says so. They're cached in `%LOCALAPPDATA%\ovrtx`, and later starts take seconds.

## 2. The panel

The toolbar, the image, the status line and following the camera are the app's render view
panel (RENDER_VIEW_PANEL.md §1). What RTX adds or decides:

| Control | What it does |
|---|---|
| Render / Stop | The helper starts with the first render and stays until the panel is deleted. Stop stops rendering frames |
| Mode | **Real-Time** (real-time path tracing, denoised), **Path Tracing** (progressive) or **Minimal** (rasterized). Changing it loads the stage again |
| Update on Edits | Transform edits are written in place, *Update after edits* ms after the last one (30 by default). Anything else loads the stage again (§3) |
| Settings… | The RTX page of the Settings dialog |

- **The state:** "Starting RTX…", Rendering, Done, Stopped. After each change the helper
  renders *Frames after a change* frames (128 by default), sending an image every *Image
  updates* ms. Then it's Done and stops using the GPU until the next change.
- **The Log panel:** the helper's output and ovrtx's messages go there under
  `plugin:org.usdcc.rtx-render`.
- **A failed start** (ovrtx not installed in that Python, no RTX GPU) or a crashed helper is a
  fatal error: Render goes off, and a sticky toast says so.
- **Without a Python set,** or with one that doesn't exist, `setup_problem()` says so, and
  Render starts nothing.

## 3. What ovrtx gets

**The stage.** `stage.Flatten()` with unsaved edits and the session layer, exported to
`%TEMP%\usdcc-rtx-*\scene_<n>.usdc` (the last two are kept; the folder goes with the panel).
ovrtx's own OpenUSD reads the `.usdc` that usdcc's OpenUSD writes.

The export adds, under `/__usdccRtx`:
- **`Camera`**: a `UsdGeom.Camera` with focal length 10 mm, whose apertures carry the panel
  camera's fields of view, and its clipping range.
- **`Product`**: a `RenderProduct` with the image's `resolution`, `camera` pointing at that
  camera, `omni:rtx:rendermode` from the Mode menu, and one `RenderVar`, `LdrColor`. ovrtx
  hands that back as 8-bit sRGB RGBA, which goes to the panel as it is.

The user's stage isn't changed. Its own render settings and products are left alone, and
ovrtx renders only `/__usdccRtx/Product`.

**In place, without a reload** ([rtx_helper.py](../src/plugins/rtx_render/python/usdcc_rtx_render/helper/rtx_helper.py)):

| Change | Written to |
|---|---|
| Camera move or another camera | The camera's `omni:xform` (camera-to-world, USD's row layout), and `horizontalAperture`/`verticalAperture` |
| Image size | The product's `resolution` |
| Transform edits | Each edited prim's `omni:xform`: its local transform as usdcc's OpenUSD computes it |

- **What counts as a transform edit** (`scene.in_place_edits()`): when all an edit changed is
  `xformOp:*` or `xformOpOrder` properties of transformable prims. The camera follows too, in
  case the panel looks through a camera prim that moved.
- **Everything else loads the stage again:** a prim added, removed or renamed; a material,
  visibility or other value; metadata; another mode; switching between a perspective and an
  orthographic camera.
- **A write that fails** makes the helper send `stale`, and the panel loads the stage again.
- **Queued work is skipped:** a load waiting in the helper's queue drops the camera moves,
  sizes and edits queued before it.

## 4. The helper process

[rtx_helper.py](../src/plugins/rtx_render/python/usdcc_rtx_render/helper/rtx_helper.py) runs
in the Python set in Settings, never in usdcc. ovrtx brings its own OpenUSD, whose DLLs have
the same names as usdcc's, and a GPU renderer's crash shouldn't take the app with it.

**Its environment** ([client.py](../src/plugins/rtx_render/python/usdcc_rtx_render/client.py)):
- `PYTHONHOME`, `PYTHONPATH`, `PXR_PLUGINPATH_NAME` and the like are removed.
- **`PATH`** is that Python's folder and the Windows folders, never usdcc's `bin`, which holds
  usdcc's OpenUSD.
- **`OMNICLIENT_HUB_MODE=disabled`.** Without it, ovrtx tries to start an Omniverse Hub
  installed on the machine. It waits for the Hub for seconds on every start, and logs about
  forty warnings when the Hub doesn't come up.

**Protocol** ([protocol.py](../src/plugins/rtx_render/python/usdcc_rtx_render/helper/protocol.py)):

| Direction | Format | Messages |
|---|---|---|
| Commands, to the helper's stdin | One JSON line each | `init {max_frames, image_ms}`, `load {path, width, height}`, `camera {transform[16], h_aperture, v_aperture}`, `size {width, height}`, `xforms {items: [{path, matrix[16]}]}`, `start`, `stop`, `quit` |
| Events, on its stdout | A JSON header line, then `size` bytes for an image | `ready {version, seconds}`, `loaded {path, seconds}`, `state {state}`, `image {w, h, frame}` with RGBA8888 pixels, `log {level, msg}`, `error {msg, fatal}`, `stale` |

**How it runs:**
- **ovrtx renders on the helper's main thread,** one `step()` per frame. Commands are read on
  a thread of their own and queued.
- **Stdin is moved out of the way first.** The helper reads commands from a copy of its stdin
  and points stdin itself at `NUL`. On Windows, while one thread waits in a read on a pipe,
  anything else touching that pipe's handle waits too. ovrtx's start-up did, and stalled
  until the next command arrived.
- **So is stdout:** before importing ovrtx, the event stream moves to a copy of stdout, and
  stdout points at stderr. Native output can't break into the stream.
- **Images are coalesced:** only the newest one waiting is sent.
- **Quitting:** stdin closing, or `quit`, ends the helper at once, with `os._exit()`. ovrtx's
  own shutdown takes seconds, and the process ending frees its GPU memory anyway. Deleting
  the panel sends `quit`, waits up to 3 s, then kills the helper.
- **`--fake`** renders a gradient instead of importing ovrtx, in usdcc's own Python. The tests
  use it.

## 5. Settings

On the RTX page of the Settings dialog
([config.py](../src/plugins/rtx_render/python/usdcc_rtx_render/config.py)):

| Key | Type | Default |
|---|---|---|
| `RtxRender/python` | String | `USDCC_OVRTX_PYTHON`, or `%LOCALAPPDATA%\usdcc\rtx\venv\Scripts\python.exe` if it exists |
| `RtxRender/mode` | Choice | Real-Time |
| `RtxRender/maxFrames` | Int 1–4096 | 128 |
| `RtxRender/imageInterval` | Int 10–2000 ms | 33 |
| `RtxRender/editDelay` | Int 0–10000 ms | 30 |

## 6. Tests

`rtx_render_plugin` in `usdcc_python_tests`
([rtx_render_plugin_test.cpp](../tests/rtx_render_plugin_test.cpp)) uses the fake renderer
and records what the panel sends. It checks that:
- the panel starts the helper and shows images at the asked size;
- the export has the render product, looking through its own camera, and leaves the stage
  alone;
- a viewport's camera is sent;
- a transform edit is written in place (`xforms`) without a load;
- moving the camera prim the panel looks through moves the render's camera;
- a new prim, and another mode, load the stage again, in that mode;
- Stop keeps the helper, and deleting the panel ends it and removes its files;
- a missing Python is reported.

**Checked by hand, with ovrtx 0.6 on an RTX 4090:**
- **The helper on its own,** driven over its pipes: load, a camera orbit, a prim moved in
  place, a resize, and quitting.
- **In the app,** with a script driving a real panel: the first render, a viewport zoom
  followed, a prim moved in place (only `xforms` and `camera` sent), and Path Tracing. The
  images were right.

## 7. Limits and next steps

- **Light units:** RTX's lights are physical. A stage lit for Storm, with a distant light of
  intensity 3, renders almost black. There's no exposure control in the panel yet.
- **Implicit shapes** (`Sphere`, `Cube`, ...) are tessellated coarsely by ovrtx: a sphere
  shows its facets.
- **Materials:** only display colours are checked so far. RTX's own materials are MDL;
  how UsdPreviewSurface and MaterialX look compared with Storm isn't checked.
- **More could be written in place:** visibility, display colours and material bindings
  still load the whole stage.
- **Time:** the stage renders at the default time; usdcc has no timeline yet.
- **Hiding the tab** stops rendering but keeps the helper and its GPU memory. Only deleting
  the panel ends it.
- **Live tests need an RTX GPU and ovrtx:** the real helper is checked by hand, not in CI.
