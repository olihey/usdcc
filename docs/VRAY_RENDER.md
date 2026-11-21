# V-Ray Render

A view panel that renders its stage with V-Ray, progressively. It follows the viewport
camera, or a camera in the stage, and renders again after edits. It is a Python plugin,
[src/plugins/vray_render](../src/plugins/vray_render).
- The panel is the app's render view panel ([RENDER_VIEW_PANEL.md](RENDER_VIEW_PANEL.md)).
- The plugin supplies V-Ray, which runs in a helper process through the V-Ray App SDK.

## 1. Setup

1. **Install the V-Ray App SDK.** Version 7.x has a module for each Python from 3.6 to
   3.14, so also for 3.13, the Python usdcc runs. The installer puts it in `C:\Program Files\Chaos\V-Ray\AppSDK`, which the plugin
   finds on its own. V-Ray Standalone or V-Ray for a DCC is not enough: neither ships the
   SDK library (`VRaySDKLibrary.dll`) or a current Python module.
2. **Set up a license.** The App SDK takes an App SDK license and a render license from
   the Chaos license server. *Settings > V-Ray > License server* overrides V-Ray's own
   setup (`vrlclient.xml`).
3. **The plugin ships with usdcc:** a deploy has it in `plugins\`, always looked in. From a
   build, run `usdcc --plugin-path src/plugins`, or add `src\plugins` in Edit > Settings >
   Plugins. A copy in `%LOCALAPPDATA%\usdcc_config\plugins` wins over the shipped one.
4. **Open the panel** from Stages > (a stage) > Views > V-Ray Render > Create New, and press Render.

## 2. The panel

The toolbar, the image, the status line and following the camera are the app's render view
panel (RENDER_VIEW_PANEL.md §1). What V-Ray adds or decides:

| Control | What it does |
|---|---|
| Render / Stop | The helper process starts with the first render and stays until the panel is deleted, so a second render starts at once. Stop only stops sampling |
| Engine | In the options menu: CPU, CUDA or RTX (NVIDIA GPUs); added with `populate_options_menu()` |
| Update on Edits | In the options menu: sends the whole stage again, *Re-render after edits* ms after the last edit (`set_edit_delay()`) |
| Settings (⚙) | The V-Ray page of the Settings dialog |

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
- The user's stage isn't changed. The panel's export replaces the stage's own render
  settings. Final renders keep them (§10).
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
| Events, on its stdout | A JSON header line, then `size` bytes for an image | `ready {version, api}`, `loaded {path, seconds}`, `state {state}`, `image {w, h, change, format}` with linear float RGBA pixels (`format: "rgba32f"`, from `VRayImage.toIntList("rgba32")`, no sRGB or gamma), which the panel shows through the view transform ([COLOR_MANAGEMENT.md](COLOR_MANAGEMENT.md) §6); `"rgba8"` would be display colors, `log {level, msg}`, `error {msg, fatal}` |

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

`vray_shader_nodes` ([vray_shader_nodes_test.cpp](../tests/vray_shader_nodes_test.cpp))
generates definitions from plugininfo-like JSON written for it and loads them through the
cache, with a folder that passes for an App SDK. It checks each kind of parameter in §8
as Sdr reads it back, the palette's catalog, that registering again adds nothing, the
list counts in an export, and that stopping takes the profile away. plugininfo itself
isn't run.

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

## 8. Shader nodes for the Material Editor

The plugin gives the Material Editor V-Ray's own shader nodes: BRDFs, materials, textures
and UVW generators, 340 of them in V-Ray 7.40, in a "V-Ray" part of the palette. A network
built from them renders in this panel. [shaders.py](../src/plugins/vray_render/python/usdcc_vray_render/shaders.py)
makes and registers them; [node_loader.py](../src/plugins/vray_render/python/usdcc_vray_render/node_loader.py)
does so when the plugin starts.

**How V-Ray reads them.** V-Ray's USD reader (Chaos's Hydra delegate for Solaris, inside
the App SDK) takes a shader whose `info:id` is `vray:` and a plugin class, on the Material's
`outputs:vray:surface`:

```usda
def Material "Mat"
{
    token outputs:vray:surface.connect = </Looks/Mat/Mtl.outputs:out>
    def Shader "Mtl"
    {
        uniform token info:id = "vray:BRDFVRayMtl"
        color3f inputs:diffuse.connect = </Looks/Mat/Checker.outputs:out>
        float inputs:reflect_glossiness.connect = </Looks/Mat/Checker.outputs:out_alpha>
        uniform int inputs:brdf_type = 3
        token outputs:out
    }
    def Shader "Checker"
    {
        uniform token info:id = "vray:TexChecker"
        color3f inputs:white_color = (1, 1, 0)
        color3f outputs:out
        float outputs:out_alpha
    }
}
```

- An input is the V-Ray parameter of its name. `outputs:out` is the plugin itself, and any
  other output is the V-Ray output parameter of its name (`Checker::out_alpha`).
- A BRDF on the terminal is wrapped in a `MtlSingleBRDF` by V-Ray.
- `BRDFVRayMtl` or `VRayNodeBRDFVRayMtl` as the `info:id` is rejected ("Invalid info:id").
- A list of connections is `inputs:<list>_count` and one input per element, numbered from 1:
  `brdfs_count = 2`, `brdfs_1`, `brdfs_2`. Without the count the list is empty; numbered from
  0, the first element is lost. A list of values (`float[] opacities`) is an array.

All of this was read from what V-Ray made of test stages, not from Chaos's documentation:
`vray.exe` loads a USD file and writes V-Ray's own scene when `VRAY_USD_VRSCENE_DUMP_PATH`
names a file. That needs no license. Checked that way: colors (a `color3f` becomes an
`AColor`), floats, ints, bools, `float3`, `float[]`, asset paths, `matrix4d` for a V-Ray
transform, connections to the plugin and to an output, and lists.

**Where the definitions come from.** V-Ray's `plugininfo.exe`, in the App SDK's `bin`,
describes every plugin as JSON with no license (`-json -outputDir`), and
`-list -listByCategory` gives its categories. `generate()` writes one `.usda` of shader
definitions, as USD's `shaderDefs.usda` is written, for the JSON folders BRDF, material,
textures and uvwgen:

| V-Ray | Definition |
|---|---|
| `TEXTURE`, `FLOAT_TEXTURE`, `INT_TEXTURE`, `VECTOR_TEXTURE` | `color3f` (the default's alpha dropped), `float`, `int`, `float3`; connectable |
| `PLUGIN` | `token`, connectable, no value |
| `BOOL`, `INT`, `FLOAT`, `COLOR`, `ACOLOR`, `VECTOR`, `STRING` | `bool`, `int`, `float`, `color3f`, `color4f`, `float3`, `string`; `connectability = "interfaceOnly"`, as V-Ray takes a value only |
| `STRING` with file extensions in its UI | `asset` |
| `ENUM` | `int`, its items as Sdr `options` |
| `TRANSFORM`, `MATRIX` (and their textures) | `matrix4d`: the three axes as rows, then the offset. Sdr has no 3×3 matrix |
| `*_LIST` of values | arrays |
| `PLUGIN_LIST`, `TEXTURE_LIST`, `FLOAT_TEXTURE_LIST` | `<list>_count` and 8 slots, `<list>_1` to `<list>_8`, on a page of their own |
| `OUTPUT_*` | an output of that type |
| `LIST`, `LIST_LIST`, `scene_name`, `channels` | left out |

- The node's label is V-Ray's name for it ("Checker"). Its group in the palette is its
  folder or texture category: BRDF, Material, UVW, Texture, Float Texture, Bitmap, ...
- A parameter's label, page (its UI rollout), help and enum items are V-Ray's.
- `outputs:out` is a `token` for BRDFs, materials and UVW generators. For a texture, it's the
  type of its category (`color3f` for Textures, `float` for Textures Float).
- V-Ray's 677 `ND_*` plugins are its versions of MaterialX's nodes. They are left out:
  V-Ray reads MaterialX networks as they are.

**The cache.** plugininfo takes about 35 seconds, so the file is cached in the user data
folder, `vray/shader_nodes/<build>/vray_shaders.usda`, with its identifiers beside it. The
build key hashes the App SDK's `git_hash.txt` and the size and time of its SDK library and
plugin DLLs, so another V-Ray makes a new file. The first time, it's made in a thread, and
the nodes appear in open palettes when it's done.

**Registration.** Each node is added with `Sdr.Registry().AddDiscoveryResult()`: discovery
type `usda`, source type `vray`, the file as its URI, and the prim name as its
subIdentifier. USD's parser first looks for a prim named after the identifier, which
`vray:...` can't be, and warns once per node. So they are all parsed at once, in a
`Tf.DiagnosticTrap` that drops those warnings, and Sdr keeps the parsed nodes. Sdr can't
forget a node: a plugin reload finds them registered and adds nothing. Then the plugin adds
its renderer profile (prefix `vray`, terminal `surface`, default surface `vray:BRDFVRayMtl`)
and calls `usdcc.shading.nodes_changed()`, which open palettes listen for.

**The export fills in list counts.** `fill_list_counts()` raises each list's count to its
last slot that is set or connected. A count already larger stays. Only the copy V-Ray gets
changes, not the stage.

**Off:** `USDCC_VRAY_SHADER_NODES=0`. The tests that load every plugin set it, as the real
plugininfo would run on a machine with V-Ray.

**Checked by hand** (V-Ray App SDK 7.40.01, a test profile, an in-app script):
- the first start made the file in 36 s and registered 340 nodes; the next took it from
  the cache at once;
- the palette has a V-Ray part with BRDF, Material, Texture, Float Texture and the rest;
- Tab, "V-Ray Mtl", Return in the Material Editor made a `vray:BRDFVRayMtl` node, drawn with
  V-Ray's labels; the same for `vray:TexChecker`;
- wired to `outputs:vray:surface` (by script: the editor's connections are drags), the
  panel's export, given to `vray.exe`, became a `BRDFVRayMtl` with the checker in `diffuse`
  and its `out_alpha` in `reflect_glossiness`, on the sphere's `Node`.

Not checked: an image. The license server wasn't reachable, so nothing was rendered; what
V-Ray made of the files was read from its scene dumps.

## 9. Limits and next steps

- **Every edit reloads the whole stage.** V-Ray makes real plugins from the file (`Node`,
  `GeomStaticMesh`, lights), so an edited transform could update its `Node` in place
  instead.
- **Time:** the stage is rendered at frame 0 (V-Ray's `Using frame: 0`), not at the time the
  timeline shows ([ANIMATION.md](ANIMATION.md) phase 5).
- **Dome lights** are invisible to the camera in V-Ray's import, where `vray.exe` shows
  them. The background is then black, unless the scene has V-Ray's own settings for it.
- **The panel's export replaces the stage's own render settings.** Final renders keep them.
- **Materials:** what V-Ray's USD reader understands (UsdPreviewSurface, MaterialX, V-Ray's
  own shaders, §8). Storm-only looks differ.
- **V-Ray nodes don't draw in Storm:** it reads no `outputs:vray:surface`, and the Material
  Editor's preview says so. A Material for both needs a preview or MaterialX network on
  `outputs:surface` too.
- **List slots:** 8 per list. A longer list needs `inputs:<list>_9` and on, authored by
  script, and its count set by hand.
- **Only `outputs:vray:surface`:** V-Ray's displacement and volume terminals aren't
  offered, as what V-Ray makes of them wasn't checked.
- **Live tests need V-Ray:** the camera conversions and the real helper are checked by
  hand, not in CI.

## 10. Final renders

The plugin also adds V-Ray as a final renderer ([FINAL_RENDER.md](FINAL_RENDER.md) §3.2), in
[final.py](../src/plugins/vray_render/python/usdcc_vray_render/final.py). Render Settings offers
it like any other; the render queue renders with it.

**The scene** is the app's export of each task (`task.export_scene()`):
- the job's stage, with the stage's own RenderSettings prim, which V-Ray reads prims through;
- the task's camera, size, tile and frame on it;
- V-Ray shaders' list counts filled in, as for the panel (§8).

V-Ray renders through the file's camera, not a RenderView of the helper's: in production mode
nothing moves it.

**The helper:**
- **One helper process**, the panel's helper program, started with the first task and kept
  for the next ones. The plugin's `deactivate()` ends it.
- **The `final` command:** `{path, width, height, region, engine, settings, time_limit}`.
  - It loads the file and sets the size, and the render region for a tile
    (`setRenderRegion(x, y, w, h)`).
  - It applies the settings below, then renders once in production mode (`production`,
    `productionCuda` or `productionOptix`).
  - It sends `progress` events, then one `result`: the whole frame, linear float RGBA. The app
    cuts a tile out of it.
- **`stop`** cancels it.

| Setting | Attribute | What the helper sets |
|---|---|---|
| Engine | `vray:engine` | The production render mode: CPU, CUDA or RTX |
| Noise threshold | `vray:noiseThreshold` | `SettingsImageSampler.progressive_threshold` (progressive sampler) |
| Min subdivs, Max subdivs | `vray:minSubdivs`, `vray:maxSubdivs` | `progressive_minSubdivs`, `progressive_maxSubdivs` |
| Global illumination | `vray:gi` | `SettingsGI.on` |
| *(the job's time limit)* | `usdcc:render:timeLimit` | `progressive_maxTime`, in minutes |

**An older App SDK:** a setting or the render region that it lacks is logged as a warning, and
the render goes on without it.

**Not checked with V-Ray:** the license server wasn't reachable here.
- **Unchecked calls:** `setRenderRegion`, `getInstanceOrCreate`, `waitForRenderEnd`,
  `setOnProgress` and the production modes are App SDK calls whose behaviour hasn't been seen.
- **What is checked:** the fake renderer (`--fake`) covers the protocol, the regions and the
  stitching (`vray_final_render`).
