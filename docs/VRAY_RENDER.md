# V-Ray Render

A view panel that renders its stage with V-Ray, progressively. It follows the viewport
camera, or a camera in the stage, and renders again after edits. It is a Python plugin,
[src/plugins/vray_render](../src/plugins/vray_render).
- The panel is the app's render view panel ([RENDER_VIEW_PANEL.md](RENDER_VIEW_PANEL.md)).
- The plugin supplies V-Ray, which runs in a helper process through the V-Ray App SDK.

## 1. Setup

1. **Install the V-Ray App SDK.** Version 7.x has a Python 3.12 module, the Python usdcc
   runs. The installer puts it in `C:\Program Files\Chaos\V-Ray\AppSDK`, which the plugin
   finds on its own. V-Ray Standalone or V-Ray for a DCC is not enough: neither ships the
   SDK library (`VRaySDKLibrary.dll`) or a current Python module.
2. **Set up a license.** The App SDK takes an App SDK license and a render license from
   the Chaos license server. *Settings > V-Ray > License server* overrides V-Ray's own
   setup (`vrlclient.xml`).
3. **The plugin ships with usdcc:** a deploy has it in `plugins\`, always looked in. From a
   build, run `usdcc --plugin-path src/plugins`, or add `src\plugins` in Edit > Settings >
   Plugins. A copy in `%LOCALAPPDATA%\usdcc\plugins` wins over the shipped one.
4. **Open the panel** from Stages > V-Ray Render, and press Render.

## 2. The panel

The toolbar, the image, the status line and following the camera are the app's render view
panel (RENDER_VIEW_PANEL.md §1). What V-Ray adds or decides:

| Control | What it does |
|---|---|
| Render / Stop | The helper process starts with the first render and stays until the panel is deleted, so a second render starts at once. Stop only stops sampling |
| Engine | CPU, CUDA or RTX (NVIDIA GPUs); added with `create_toolbar()` |
| Update on Edits | Sends the whole stage again, *Re-render after edits* ms after the last edit (`set_edit_delay()`) |
| Settings… | The V-Ray page of the Settings dialog |

- **The state** is V-Ray's (Preparing, Rendering, Done, ...) or the helper's ("Starting
  V-Ray…", "Loading the stage…").
- **The Log panel:** V-Ray's warnings and errors, and anything the helper prints, go there
  under `plugin:org.usdcc.vray-render`.
- **A license error or a crashed helper** is a fatal error (`report_error(..., fatal=True)`):
  Render goes off, and a sticky error toast says so.
- **Without an App SDK folder,** `setup_problem()` says so, and Render starts nothing.

## 3. What V-Ray gets

**The stage.** `stage.Flatten()`, so unsaved edits and the session layer are included,
exported to `%TEMP%\usdcc-vray-*\scene_<n>.usdc`.
- The last two files are kept, and the folder goes with the panel.
- The export adds `/__usdccVRay/Camera` and `/__usdccVRay/RenderSettings`, and sets the
  layer's `renderSettingsPrimPath` to the latter. **V-Ray reads a USD file's prims only
  through its render settings:** without any, it loads lights and nothing else.
- The user's stage isn't changed. Its own render settings are replaced in the export, for
  now.
- `.usdc` written by usdcc's OpenUSD 0.26.8 loads in V-Ray's 0.25.5 reader.

**Space.** V-Ray reads USD in USD's own space:
- A Y-up stage stays Y-up (`SettingsUnitsInfo.scene_upDir`), and `metersPerUnit` becomes
  `meters_scale`.
- A RenderView's `transform` is the camera-to-world matrix: V-Ray's `Matrix(a, b, c)`
  takes the USD matrix's first three rows, and the offset is the fourth.
- `fov` is horizontal, in radians.

**The camera.**
- **Which camera, and when:** the panel decides that, and gives a `RenderCamera` at the
  image's aspect ratio (RENDER_VIEW_PANEL.md §2).
  - For the viewport, its vertical field of view (45°) is kept, so a wider image shows more at
    the sides.
  - For a camera prim, its horizontal field of view.
  - Camera moves come at most every 30 ms.
- **Into V-Ray's terms:** `scene.Camera.from_render()` takes the horizontal field of view in
  radians. An orthographic camera's `ortho_width` becomes `orthographicWidth`.
- The helper renders through a RenderView of its own, `usdccCamera`, made the current
  camera with `renderer.cameraName`. The RenderViews V-Ray makes from the file's render
  camera can't be used: changes to them are ignored while interactive rendering runs
  (checked: the image doesn't change).

**Edits** export and load the whole stage again. That takes about 0.3 s on a small stage,
but a big one takes longer, and V-Ray starts again from a noisy image.

## 4. The helper process

This helper remains specific to V-Ray. RTX now runs through a separate native
C++ plugin with GPU presentation; removing Python RTX does not change V-Ray's
protocol, package support or CPU image hooks.

[helper/vray_helper.py](../src/plugins/vray_render/python/usdcc_vray_render/helper/vray_helper.py)
runs in a `python.exe` of its own: usdcc's (`sys.base_prefix`), or *Settings > V-Ray >
Python*.

**Why not inside usdcc:**
- V-Ray reads USD through its own OpenUSD (0.25.5, in `bin\vray_usd`), whose DLLs have
  the same names as usdcc's (`usd_tf.dll`, ...). Windows would hand V-Ray the ones already
  loaded.
- A crash in V-Ray would take the app with it.
- `usdcc.exe` can't be the helper either: it loads usdcc's OpenUSD.

**Its environment** ([client.py](../src/plugins/vray_render/python/usdcc_vray_render/client.py)):
- `PYTHONHOME`, `PYTHONPATH`, `PXR_PLUGINPATH_NAME`, `VRAY_PATH` and the like are removed.
- `PATH` is the App SDK's `bin`, the Python DLL's folder (in a deploy that is `bin`, next to
  `usdcc.exe`) and the Windows folders.
- Checked: with usdcc's OpenUSD folders on `PATH` after V-Ray's, V-Ray still loads its own
  and renders.

**Protocol** ([protocol.py](../src/plugins/vray_render/python/usdcc_vray_render/helper/protocol.py)):

| Direction | Format | Messages |
|---|---|---|
| Commands, to the helper's stdin | One JSON line each | `init {sdk, engine, update_ms, license_servers}`, `size {width, height}`, `camera {transform[12], fov, ortho, ortho_width}`, `load {path}`, `engine {engine}`, `start`, `stop`, `quit` |
| Events, on its stdout | A JSON header line, then `size` bytes for an image | `ready {version, api}`, `loaded {path, seconds}`, `state {state}`, `image {w, h, change}` with RGBA8888 pixels, `log {level, msg}`, `error {msg, fatal}` |

- **Before importing V-Ray,** the helper moves its event stream to a copy of stdout and
  points stdout at stderr. Whatever V-Ray's native code prints then can't break into the
  stream; it reaches the Log panel through stderr.
- **Images are coalesced:** only the newest image waiting is sent, so a slow reader never
  falls behind. *Image updates* sets how often V-Ray offers one.
- **stdin closing means usdcc is gone:** the helper stops and exits. Checked by ending
  usdcc with `os._exit()`.
- **Deleting the panel** (File > Close Stage, a plugin reload, quitting) calls the plugin's
  `close()`, which sends `quit`, waits 3 s, then kills the helper. So does the plugin's
  `deactivate()` at quit. Closing the tab only hides the panel. That stops rendering, and
  the helper stays.
- **The helper's `QProcess` has no parent:** the panel's widgets are the app's, and `close()`
  ends the process.
- **`--fake`** renders a gradient instead of loading V-Ray. The tests use it.
- **The frame buffer:** the helper makes the renderer with `enableFrameBuffer=False`, so the
  App SDK doesn't open its own window.

## 5. Settings

On the V-Ray page of the Settings dialog
([config.py](../src/plugins/vray_render/python/usdcc_vray_render/config.py)):

| Key | Type | Default |
|---|---|---|
| `VRayRender/appSdkFolder` | Folder | `VRAY_APPSDK`, the folder above `VRAY_APPSDK_BIN`, or the installer's folder if it exists |
| `VRayRender/python` | String | empty: usdcc's own `python.exe` |
| `VRayRender/licenseServers` | String | empty: V-Ray's own setup |
| `VRayRender/engine` | Choice | CPU |
| `VRayRender/updateInterval` | Int 30–5000 ms | 200 |
| `VRayRender/editDelay` | Int 50–10000 ms | 500 |

## 6. Measured

Measured with V-Ray App SDK 7.40.01 and a 32-thread CPU, on a small test stage:

| What | Time |
|---|---|
| Loading the stage | 0.2–0.35 s |
| First image | about 0.3 s after start |
| New image after a camera change | about 80 ms |
| A 1920 × 1080 frame to RGBA bytes (`toBitmapData`) | about 10 ms |
| Stop, clear, load and start again after an edit | about 0.3 s |
| Quitting the helper | 2–3 s (closing the renderer) |

## 7. Tests

`vray_render_plugin` in `usdcc_python_tests`
([vray_render_plugin_test.cpp](../tests/vray_render_plugin_test.cpp)) uses the fake
renderer. It drives the app's panel through its controls' object names (`renderButton`,
`renderCamera`, `renderResolution`, `renderStatus`). It checks that:
- the panel starts the helper and shows images at the asked size;
- the export has the render prims and leaves the stage alone;
- the render follows a viewport's camera and a camera prim;
- an edit exports again;
- Stop keeps the helper, and deleting the panel ends it and removes the files;
- a missing App SDK is reported.

The panel itself is tested on its own (RENDER_VIEW_PANEL.md §7). `active_viewport_camera` and
`stage_edited_event` ([viewport_camera_test.cpp](../tests/viewport_camera_test.cpp)) cover
the app calls underneath.

**Checked by hand, with V-Ray:** with an in-app script driving a real panel:
- the viewport camera's framing (a projected point lands on the same pixel);
- a camera prim's framing (the same as `vray.exe` renders it);
- an edit;
- the helper ending with usdcc.

These checks were made before the move to the app's render view panel and haven't been
repeated since. The camera numbers V-Ray gets are the same; the fake-renderer test checks
them.

## 8. Limits and next steps

- **Every edit reloads the whole stage.** V-Ray makes real plugins from the file (`Node`,
  `GeomStaticMesh`, lights), so an edited transform could update its `Node` in place
  instead.
- **Time:** the stage is rendered at frame 0 (V-Ray's `Using frame: 0`); usdcc has no
  timeline yet.
- **Dome lights** are invisible to the camera in V-Ray's import, where `vray.exe` shows
  them. The background is then black, unless the scene has V-Ray's own settings for it.
- **The stage's own render settings are replaced in the export.**
- **Materials:** what V-Ray's USD reader understands (UsdPreviewSurface, MaterialX, V-Ray's
  own shaders). Storm-only looks differ.
- **Live tests need V-Ray:** the camera conversions and the real helper are checked by
  hand, not in CI.
