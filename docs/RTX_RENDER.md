# RTX Render

A view panel that renders its stage with NVIDIA's Omniverse RTX renderer, through
[ovrtx](https://nvidia-omniverse.github.io/ovrtx), NVIDIA's C and Python SDK for it. It
follows the viewport camera or a camera in the stage, and renders again after edits.
Moved prims, camera moves and size changes are written into the renderer's stage in place,
with no reload. It's a Python plugin, [src/plugins/rtx_render](../src/plugins/rtx_render):
- The panel is the app's render view panel ([RENDER_VIEW_PANEL.md](RENDER_VIEW_PANEL.md)).
- The plugin supplies ovrtx, which runs in a helper process: usdcc's own Python, with
  ovrtx installed as the plugin's packages.

ovrtx is pre-release, under NVIDIA's proprietary licence, and needs an NVIDIA RTX GPU with
a recent driver. usdcc doesn't ship it: it's downloaded from NVIDIA's package index when the
user installs the plugin's packages. The licence also forbids publishing benchmark results
without NVIDIA's permission, so this page gives no timings.

## 1. Setup

1. **The plugin ships with usdcc:** a deploy has it in `plugins\`, always looked in. From a
   build, run `usdcc --plugin-path src/plugins`, or add `src\plugins` in Edit > Settings >
   Plugins. A copy in `%LOCALAPPDATA%\usdcc\plugins` wins over the shipped one.
2. **Install its packages.** The plugin shows **Needs packages** in View > Plugins, and a
   toast offers **Install**. The dialog lists ovrtx, ovstage and numpy, and says they come
   from `https://pypi.nvidia.com`, named by RTX Render. The download is about 1.3 GB.
   Unattended: `usdcc --install-plugin-packages`.
3. **Restart usdcc,** as for any package install. Then open the panel from Stages > RTX
   Render, and press Render.

**Why NVIDIA's index:** PyPI has no ovrtx wheel, only NVIDIA's "wheel stub". That is a
source package whose build downloads the real wheel from `pypi.nvidia.com`. usdcc's installer
only takes wheels, so that no package code runs during an install. The manifest names
NVIDIA's index (`python.indexes`, PYTHON_PACKAGES.md §4), which has the wheels. numpy comes
from PyPI, as NVIDIA's index doesn't have it.

**The versions are pinned to `ovrtx>=0.6,<0.7` and `ovstage>=0.3,<0.4`:** ovrtx changes its
API between minor versions, and Update Packages would otherwise move to the next one.
*Settings > RTX > Other Python* can name a `python.exe` with another ovrtx installed instead.

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
in usdcc's own `python.exe`, in a process of its own, never inside usdcc:
- **A separate process isolates crashes:** a GPU renderer's crash shouldn't take the app with
  it.
- **ovrtx can't share a process with another OpenUSD.** ovrtx says so, and usdcc's plugins on
  `PXR_PLUGINPATH_NAME` would meet ovrtx's OpenUSD.

**Its packages** ([client.py](../src/plugins/rtx_render/python/usdcc_rtx_render/client.py)):
- **The plugin finds the folders** ovrtx, ovstage and numpy are installed in with
  `importlib.util.find_spec()`, so they're never imported into usdcc. It passes them to the
  helper as `--packages`.
- **The helper puts them on `sys.path`,** and takes off the entry holding usdcc's `pxr`.
- **The app never loads them either:** its check at start reads their metadata only.
- **A `python.exe` in *Other Python*** runs with its own packages instead, and gets no
  `--packages`.

**Its environment:**
- `PYTHONHOME`, `PYTHONPATH`, `PXR_PLUGINPATH_NAME` and the like are removed.
- **`PATH`** is the Python's folder, its DLL's folder and the Windows folders.
  - In a deploy, `python312.dll` is in `bin`, beside usdcc's OpenUSD DLLs (`usd_*.dll`).
    That's harmless: ovrtx's OpenUSD is one DLL named its own way, `ov_<version>usd_ms.dll`.
  - Checked: the helper in the deploy's `python.exe`, with `bin` on `PATH`, rendered the same
    images as in a separate Python.
  - *Other Python* gets only its own folder.
- **`OMNICLIENT_HUB_MODE=disabled`.** Without it, ovrtx tries to start an Omniverse Hub
  installed on the machine. It waits for the Hub for seconds on every start, and logs about
  forty warnings when the Hub doesn't come up.

**Protocol** ([protocol.py](../src/plugins/rtx_render/python/usdcc_rtx_render/helper/protocol.py)):

| Direction | Format | Messages |
|---|---|---|
| Commands, to the helper's stdin | One JSON line each | `init {max_frames, image_ms}`, `load {path, width, height}`, `camera {transform[16], h_aperture, v_aperture}`, `size {width, height}`, `xforms {items: [{path, matrix[16]}]}`, `start`, `stop`, `quit` |
| Events, on its stdout | A JSON header line, then `size` bytes for an image | `ready {version, seconds}`, `loaded {path, seconds}`, `state {state}`, `image {w, h, frame}` with RGBA8888 pixels, `log {level, msg}`, `error {msg, fatal}`, `stale`, `slow {seconds, frame}` |

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
- **A frame that takes long is reported.** `step()` returns only when ovrtx is done. For a
  scene with many complex MDL materials, the first frame can take minutes, with the GPU
  busy throughout. A watchdog thread sends `slow` after 10 s and every 5 s after that. The
  panel shows how long it's been, in the status line and in place of the image, and logs it
  every minute. Meanwhile commands wait: Stop takes effect once the frame is done, and
  deleting the panel kills the helper after 3 s. `--fake` takes
  `USDCC_RTX_FAKE_FIRST_FRAME` (seconds) and `USDCC_RTX_FAKE_SLOW_AFTER` for the tests.
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
| `RtxRender/python` (*Other Python*) | String | empty: usdcc's own Python, with the plugin's packages |
| `RtxRender/mode` | Choice | Real-Time |
| `RtxRender/maxFrames` | Int 1–4096 | 128 |
| `RtxRender/imageInterval` | Int 10–2000 ms | 33 |
| `RtxRender/editDelay` | Int 0–10000 ms | 30 |

## 6. Tests

`rtx_render_plugin` in `usdcc_python_tests`
([rtx_render_plugin_test.cpp](../tests/rtx_render_plugin_test.cpp)) checks that the plugin as
shipped shows **Needs packages** naming ovrtx, with none of its code run. Test machines don't
have ovrtx. Then it loads a copy without `requires` and `indexes`, uses the fake renderer,
and records what the panel sends. It checks that:
- the panel starts the helper and shows images at the asked size;
- the export has the render product, looking through its own camera, and leaves the stage
  alone;
- a viewport's camera is sent;
- a transform edit is written in place (`xforms`) without a load;
- moving the camera prim the panel looks through moves the render's camera;
- a new prim, and another mode, load the stage again, in that mode;
- Stop keeps the helper, and deleting the panel ends it and removes its files;
- ovrtx not installed, and an *Other Python* that isn't there, are reported.

`package_extra_index` and `plugin_package_indexes` cover `python.indexes`
(PYTHON_PACKAGES.md §7).

**Checked by hand, with ovrtx 0.6 on an RTX 4090:**
- **The helper on its own,** driven over its pipes: load, a camera orbit, a prim moved in
  place, a resize, and quitting. Both in a separate Python and in the deploy's `python.exe`
  with `bin` on `PATH`.
- **The install:** `usdcc --install-plugin-packages`, into a scratch packages folder. ovrtx
  and ovstage came from NVIDIA's index as wheels, and numpy from PyPI.
- **In the app,** with ovrtx from that install and a script driving a real panel: the first
  render, a viewport zoom followed, a prim moved in place (only `xforms` and `camera` sent),
  and Path Tracing. The images were right.
- **Not checked by hand:** the install dialog itself, clicked through. Its list of indexes
  is the same plan that `--install-plugin-packages` logged.

## 7. Limits and next steps

- **Heavy first frames:** a product scene (`omniverse_preview.usda`: 10 MDL materials built
  on OmniSurface, about 1,550 shaders, lit by a dome light) didn't finish its first frame within ten minutes
  in ovrtx 0.6, in any mode (Real-Time, Path Tracing, Minimal), whatever the texture
  streaming or sync mode. The GPU stayed busy at 45–80 % and over 200 W, and nothing was
  logged. Without the dome light, or with only some of the materials, it rendered in
  seconds. It's ovrtx's own work, not the plugin's: the plugin shows it's still working, but
  can't make that frame finish.
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
- **Size:** about 2.5 GB in the shared packages folder once installed.
- **Everyone who loads `src/plugins` is asked:** the plugin shows **Needs packages**, with
  the install offer, also on machines without an RTX GPU. Turn it off in View > Plugins there.
- **ovrtx is importable in usdcc's own Python** once installed, like any shared package.
  Nothing imports it there, and nothing should: `import ovrtx` in the Script Editor would load
  a second renderer and OpenUSD into the app.
