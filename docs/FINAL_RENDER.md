# Final Render

Renders a stage's frames to files: one frame, or a frame range, with any renderer usdcc has.
Every Hydra render delegate is one (Storm, Embree, Arnold, ...). Plugins add others: RTX and
V-Ray do (§3.2).

- **What to render:** the stage's own `UsdRenderSettings` prim, which is also where the
  settings are kept, so they go with the file.
- **The work:** jobs in a queue, each frame whole or in tiles, rendered while the app stays
  usable.

| | |
|---|---|
| **Code** | [src/cpp/usd/include/usdcc/usd/render](../src/cpp/usd/include/usdcc/usd/render), in `usdcc_usd_ui` |
| **Menu** | Render > Render Frame (F12), Render Animation (Ctrl+F12), Render Settings, Render Queue |
| **Python** | `usdcc.render` ([render.py](../src/python/usdcc/render.py)) |
| **Plugins** | `Host::addFinalRenderer()` ([plugin.h](../src/cpp/plugin_sdk/include/usdcc/plugin/plugin.h)) |

## 1. Rendering

1. Open **Render > Render Settings** and press **New**. That makes `/Render/Settings`, a product
   `/Render/Products/Main` with a `color` RenderVar, and names the prim in the root layer's
   `renderSettingsPrimPath`. It's one undo step.
2. Pick the renderer, the camera, the size, the frames and the output, and the renderer's own
   settings on its tab.
3. Press **Render Frame** or **Render Animation**. The **Render Queue** shows the job.

A stage without a RenderSettings prim renders too:
- the settings come from Settings > Final Render's defaults (§5.4);
- the camera is the active viewport's, when it shows the stage.

## 2. The RenderSettings prim

### 2.1 Which prim

[render_spec.h](../src/cpp/usd/include/usdcc/usd/render/render_spec.h)

- **The stage's default:** the prim its `renderSettingsPrimPath` metadata names, else the first
  RenderSettings prim. The Render Settings panel can show any of them.
- **What's read:** `UsdRenderComputeSpec()` reads the prim:
  - its camera, resolution and pixel aspect, and its products with their RenderVars;
  - its `includedPurposes`;
  - the attributes in the renderer's namespaces.
- **The first product decides the image.** Its own resolution and camera win over the settings'.
- **An unauthored resolution** is Settings > Final Render's, not UsdRender's built-in 2048 × 1080.

### 2.2 A renderer's settings

Each renderer lists its settings (`FinalRenderer::settingsSchema()`), and each is an attribute
in the renderer's namespace:

| Renderer | Namespace | Example |
|---|---|---|
| A Hydra delegate | Its plugin id without `Hd` and `RendererPlugin` | `embree:ty:maxBounces`, `storm:maxLights`, `arnold:AA_samples` |
| V-Ray | `vray` | `vray:noiseThreshold`, `vray:engine` |
| RTX | `omni` | `omni:rtx:rendermode` (ovrtx's own), `omni:rtx:usdcc:samples` |

- A key already in the namespace stays as it is (`arnold:AA_samples`); any other key gets the
  namespace in front.
- **Only authored values reach the renderer**, converted to the type of its default.
- **A Hydra delegate starts from Settings > Renderers' values** ([VIEWPORT_DISPLAY.md](VIEWPORT_DISPLAY.md)
  §7), the same ones its viewports use.

### 2.3 usdcc's options

These live on the same prim, as custom attributes:

| Attribute | Type | Default |
|---|---|---|
| `usdcc:render:renderer` | string, a renderer's id | Settings > Final Render > Renderer |
| `usdcc:render:frameStart`, `frameEnd` | double | the stage's start and end time codes |
| `usdcc:render:frameStep` | double | 1 |
| `usdcc:render:output` | string, a file pattern (§4.3) | the product's `productName`, else `<stage>_<settings>` |
| `usdcc:render:format` | token: `exr` or `png` | Settings > Final Render > Format |
| `usdcc:render:tiles` | int2 | 1 × 1 |
| `usdcc:render:timeLimit` | double, seconds per task | Settings > Final Render > Time limit |

## 3. Renderers

[final_renderer.h](../src/cpp/usd/include/usdcc/usd/render/final_renderer.h):
`FinalRenderer` and the `FinalRenderers` registry.

A renderer has:
- an `id()`, a `name()`, and `setupProblem()`: why it can't render here, or nothing;
- capabilities: `tiles` (it renders a region on its own);
- its namespaces and `settingsSchema()`;
- `start(task, callbacks)`, which reports progress, then images for each AOV the size of the
  task's region, or a failure.

A plugin adds one with `Host::addFinalRenderer()` and removes it when it's turned off.

### 3.1 Hydra

[hydra_final_renderer.h](../src/cpp/usd/include/usdcc/usd/render/hydra_final_renderer.h)

- **One per delegate:** `registerHydraFinalRenderers()` makes one for each of
  `UsdImagingGLEngine::GetRendererPlugins()`, at start, after Arnold is set up.
- **A task gets an engine of its own** on the UI thread. It's driven by a timer:
  - Storm draws a frame each turn of the event loop;
  - other delegates render in their own threads and are asked how they're doing every 20 ms.
- **When it stops:** once the delegate says it has converged (Storm after three frames, so what
  the first one found has loaded), or at the time limit.
- **Storm** renders in an offscreen OpenGL context. **Every other delegate runs without the GPU**
  (`gpuEnabled = false`), so it needs no OpenGL. That's how the tests render with Embree.
- **The engine is set up as follows:**
  - the camera is the job's camera prim (`SetCameraPath`);
  - the render buffer is the whole image, and the framing's data window is the task's region,
    so a tile renders only its pixels;
  - AOVs: `SetRendererAovs`, with color first; an AOV the delegate doesn't have is left out,
    with a warning;
  - color correction is off, so the images are linear;
  - delegates other than Storm get `SetActiveRenderSettingsPrimPath`, for those that read the
    prim themselves.
- **A stage without lights** gets Hydra's dome, as a viewport does.
- **Read back:** each AOV's render buffer is mapped and turned into float RGBA. Hydra's rows run
  bottom to top. One-channel AOVs (depth) have the value in R, G and B.

### 3.2 Renderers that read a USD file

[scene_file_renderer.h](../src/cpp/usd/include/usdcc/usd/render/scene_file_renderer.h)

Plugins don't share code with each other, so what RTX and V-Ray both need is here.
`exportRenderScene()` writes a task's scene to a folder:
- **The content:** a copy of the job's snapshot, so a task's changes don't reach the others.
- **The RenderSettings:** the stage's own prim, kept and named in the layer's
  `renderSettingsPrimPath`. Without one, `/__usdccRender/Settings` is made, beside the job's
  private camera.
- **On the settings and their first product:**
  - the task's camera, resolution and pixel aspect (a product is made if there's none);
  - the tile, as `dataWindowNDC`;
  - a `color` RenderVar, if the product has none.
- **Time:** the frame, as the layer's start and end time code.
- **The renderer's own touches**, last.

`SceneFileRenderer` is a `FinalRenderer` base built on it:
- **A subclass implements** `customizeScene()` and `renderFile(task, scene, callbacks)`.
- **The base** gives each task a temporary folder, removed with the task.
- **Whole frames for a tile** are cut down to the tile, so a renderer that can't render a region
  still fits.
- **A Python renderer** gets the same export from `task.export_scene()` (§6.1).

| | RTX ([RTX_RENDER.md](RTX_RENDER.md)) | V-Ray ([VRAY_RENDER.md](VRAY_RENDER.md) §10) |
|---|---|---|
| Built on | `SceneFileRenderer`, C++ | `usdcc.render.FinalRenderer`, Python |
| Its touches | Emission scaled for light matching; a private camera from the task's camera at the frame; the product's `omni:rtx:rendermode`; an `HdrColor` var only | V-Ray shaders' list counts filled in |
| Renders with | An engine of its own per task, with a CUDA-only bridge that reads the frame back; done at the sample or frame budget | The V-Ray helper, kept between tasks, in production mode (`final` command) |
| Tiles | No: whole frames, until ovrtx's handling of `dataWindowNDC` has been checked | Yes: V-Ray's render region |
| Settings | Mode, samples or frames, light matching, multiplier | Engine, noise threshold, min and max subdivs, GI |
| AOVs | Color (HdrColor) | Color |

## 4. Jobs

### 4.1 The snapshot

[render_job.h](../src/cpp/usd/include/usdcc/usd/render/render_job.h)

- **`makeRenderJob()` runs on the UI thread** when the job is submitted.
- **It flattens the stage** under its resolver context: unsaved edits and the session layer
  are included, and asset paths are anchored. The copy is opened as a stage of its own.
- **Edits after that** don't reach the job.
- **No camera named:** the active viewport's is added to the copy, at the image's aspect, as
  `/__usdccRender/Camera`. The stage itself gets nothing.

### 4.2 Tasks and tiles

- **A task is a frame, or a tile of one.** Tiles are a grid (`usdcc:render:tiles`). Each grows
  by 8 pixels where it meets another, so the pixel filter has room, and only its own part goes
  into the frame.
- **A renderer without `tiles`** renders whole frames: the grid is ignored.

### 4.3 Output files

`outputPath()` builds each file's name from a pattern:
- **Where the pattern comes from:** `usdcc:render:output`, else the product's `productName`,
  else `<stage>_<settings>` (`_<product>` added when there are several products).
- **Tokens:** `<stage>` (the root layer's name), `<settings>`, `<product>`, `<aov>`,
  `<renderer>`, and `<frame>`, `####`, `$F4` or `$F` for the frame. Frames have 4 digits; a
  fractional one is written `0012.500`.
- **A range without a frame token** gets `.<frame>` before the extension.
- **Folders:** a relative pattern is in Settings > Final Render > Output folder (Pictures/usdcc
  renders by default). Missing folders are made.
- **OpenEXR:** a file for each product, with every AOV as a layer. The first layer has the plain
  R, G, B and A channels, and the others are `<aov>.R`, ... (`<aov>.Y` for one channel). Values
  are linear, with Rec.709/D65 chromaticities (`writeLinearExrLayers()`, [linear_exr.h](../src/cpp/usd/include/usdcc/usd/linear_exr.h)).
- **PNG:** a file for each AOV. The first goes through the view transform
  ([COLOR_MANAGEMENT.md](COLOR_MANAGEMENT.md)); the others are saved as they are, with
  `_<aov>` added to their names.

### 4.4 The queue

[render_scheduler.h](../src/cpp/usd/include/usdcc/usd/render/render_scheduler.h)

- **Order:** jobs run in the order they came. Each worker takes the next waiting task it can
  render, one at a time. This instance is one worker (`LocalWorker`).
- **Reports** come back through the event loop, whatever thread the renderer calls from.
- **A finished task's images** go into its frame. Once every tile is in, the frame is written
  and shown in the Render Queue.
- **A failed task** is tried again, up to three tries in all. Then the job fails and its
  other tasks stop. A sticky toast says why.
- **Cancel** stops the running tasks; frames already written stay.
- **Retry** queues the failed and cancelled tasks again, with the tiles of their frames.
- **Quitting** asks first while something renders, then cancels it.

## 5. The UI

### 5.1 Render Settings panel

[render_settings_view_panel.h](../src/cpp/usd/include/usdcc/usd/render/render_settings_view_panel.h)
is a view panel, opened from the Render or Stages menu.

- **Toolbar:** the RenderSettings prim and **New**; below it, the renderer. A renderer that
  can't render here shows why.
- **Tabs:**
  - **Image:** the camera (or the active viewport's), the resolution with common sizes, and the
    pixel aspect. The resolution and camera are written to the first product when it has its
    own, else to the settings.
  - **Output:** the frame range (the stage's, or its own) and step, the output pattern with an
    example name, the format, the tiles, and the time limit.
  - **Renderer:** the renderer's settings, made from its list. Authored values are bold, and ↺
    clears one back to its default.
  - **AOVs:** the products' RenderVars. **Add** makes one under the first product from a
    renderer AOV name (color, depth, normal, primId, `primvars:st`, ...), and **Remove** takes
    one away.
- **Edits:** each one is an undo step on the stage's edit target.
- **Render Frame** and **Render Animation** buttons.
- **Sessions** keep which prim the panel shows.

### 5.2 Render Queue

[render_queue_panel.h](../src/cpp/usd/include/usdcc/usd/render/render_queue_panel.h) is a side
panel. It opens when a render is queued.

- **The list:** each job, then its tasks, with their status, progress, worker and time.
- **The preview:** the newest frame, through the view transform.
- **Buttons:** Cancel, Retry, Remove, Clear Ended, and Open Folder.

### 5.3 Render menu

[render_menu.h](../src/cpp/usd/include/usdcc/usd/render/render_menu.h)

| Entry | What it does |
|---|---|
| Render Frame (F12) | The current stage's default settings, at the frame the timeline shows |
| Render Animation (Ctrl+F12) | The same, for the frame range |
| Render Settings | Opens a Render Settings panel for the current stage |
| Render Queue | Shows the Render Queue |

### 5.4 Settings > Final Render

| Key | Default |
|---|---|
| `FinalRender/renderer` | `HdStormRendererPlugin` |
| `FinalRender/outputFolder` | empty: Pictures/usdcc renders |
| `FinalRender/format` | `exr` |
| `FinalRender/width`, `FinalRender/height` | 1920 × 1080 |
| `FinalRender/timeLimit` | 0 s: until done |

## 6. Python

```python
import usdcc.render as render
render.renderers()                    # [Renderer('HdStormRendererPlugin', 'GL'), ...]
job_id = render.render_frame()        # the current stage, the frame shown
render.render_animation(stage, settings="/Render/Settings", renderer="HdEmbreeRendererPlugin")
render.job(job_id).status             # "queued", "running", "done", "failed", "cancelled"
render.cancel(job_id)
```

The calls raise RuntimeError with the reason a job can't be made (no camera, a renderer that
can't render here). The settings themselves are attributes, set with `Usd` as any others.

### 6.1 A renderer in Python

```python
class MyRenderer(render.FinalRenderer):
    id = "com.example.my-renderer"
    name = "My Renderer"
    namespaces = ("my",)
    tiles = False

    def settings_schema(self):
        return [render.setting("my:samples", "Samples", "int", 64, minimum=1, maximum=4096)]

    def start(self, task):
        scene = task.export_scene()           # Scene: file, settings, product, camera
        ...                                   # later, on the UI thread:
        task.finished({"color": qimage})      # a float QImage per AOV; or task.failed(why)

    def cancel(self, task):
        ...

render.register_renderer(MyRenderer())        # unregister_renderer(id) in deactivate()
```

- **`task`:**
  - `frame`, `resolution`, `pixel_aspect`;
  - `region` (x, y, width, height from the top left);
  - `camera`, `render_settings`, `aovs`, `time_limit`;
  - `settings` (the authored ones, by key);
  - `progress(fraction, state)`;
  - `active`, False once it has ended or been cancelled.
- **Images:** a whole-frame image for a tile is cut to the tile.
- **Settings:** `setting()` types are `bool`, `int`, `float`, `string`, `choice` and `color`.
- **Hosting:** [scripted_final_renderer.cpp](../src/cpp/scripting/src/scripted_final_renderer.cpp)
  hosts the instance. Renderers from Python are removed before Python stops.

## 7. Next

Milestones 1 (core and Hydra) and 2 (RTX and V-Ray) are done.

- **3: transfers.** Chunked file transfers between peers (PEERS.md §7.3), and scene bundles
  made with `UsdUtilsComputeAllDependencies`.
- **4: distributed rendering.**
  - **Nodes:** peers advertise their renderers in a PeerState. Each peer is a `Worker` for the
    scheduler.
  - **Splitting:** jobs go out by frames and by tiles.
  - **Scenes:** sent as a bundle, or as the unflattened stage to nodes whose asset preflight
    shows they resolve every asset.
  - **Trust:** opt-in render nodes accept jobs only from trusted fingerprints.
  - **Peers only:** the submitting app doesn't render.

## 8. Tests

`final_render_test.cpp`, in `usdcc_tests`:

| Test | What it checks |
|---|---|
| `final_render_tiles` | The tiles cover the image once; how overlaps grow and are clipped |
| `final_render_spec` | Defaults without a prim; New's prims and metadata; resolution from the settings and from the product; namespace filtering; usdcc's options; values converted to the renderer's types; setting and clearing a field |
| `final_render_output_names` | Patterns, tokens, frame numbers, PNG AOV names, `productName` |
| `final_render_scheduler_tiles` | 3 frames × 6 tiles with a gradient renderer, stitched exactly; the snapshot isolated from later edits; the renderer's settings; the EXR files |
| `final_render_scheduler_retry_and_fail` | Two failures, then success; failing after three tries; Retry; Cancel; Clear Ended |
| `final_render_job_errors` | An unknown renderer; no camera; the viewport's camera added to the snapshot only; a renderer without tiles |
| `final_render_embree` | Embree renders a frame (with a depth AOV) without a GPU, the right way up; the same frame in 2 × 2 tiles matches it |
| `final_render_scene_export` | The stage's RenderSettings kept and named; its renderer settings; the product's size, camera and tile as `dataWindowNDC`; the frame as the time range; the renderer's touches; the snapshot left alone; settings, product and var made when there are none |
| `final_render_scene_file_renderer` | A renderer that renders whole frames from the file, in 2 × 2 tiles: cut, stitched exactly, at the frame's time; the files removed with their tasks |

Also:
- **`render_settings_panel`** (usdcc_ui_tests, [render_settings_panel_test.cpp](../tests/render_settings_panel_test.cpp)):
  New as one undo step; the width written; Embree's settings tab written and reset; undo;
  the Render Queue listing a job and its cancelling.
- **`python_render_api`** (usdcc_python_tests): `renderers()`; the errors; a job queued from
  settings made by script, and cancelled.
- **`rtx_native_final`, `rtx_native_final_scene`** (usdcc_python_tests,
  [rtx_native_final_test.cpp](../tests/rtx_native_final_test.cpp)): RTX with a fake engine:
  - the prim's mode, budget and light matching reach the engine;
  - the frame read back;
  - the scene file's `omni:rtx:rendermode`, `HdrColor` var, private camera and RenderSettings;
  - a missing runtime, and an engine error;
  - a camera at a frame.
- **`vray_final_render`** (usdcc_python_tests, [vray_render_plugin_test.cpp](../tests/vray_render_plugin_test.cpp)):
  V-Ray with the helper's fake renderer:
  - the renderer added and removed with the plugin;
  - its settings list;
  - two tiles rendered as regions by one helper and stitched exactly, with the prim's noise
    threshold.

A real ovrtx or V-Ray render hasn't been checked: neither SDK is on this machine.

Storm's offscreen path needs OpenGL, which the tests' platform lacks; it was checked by hand
in the app (F12 on `test_data/materials.usda`).

## 9. Limits

- **One task at a time on this instance.** Renderers keep their own threads busy.
- **Storm on the UI thread:** a big Storm frame holds the app up while it draws.
- **The snapshot is a flattened copy.** Big stages take a while to flatten when submitted.
- **Motion blur and shutter** come from the renderer's own settings. usdcc gives a frame's time only.
- **A failed write** fails the job; the folder must be writable.
