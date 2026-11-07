# Render View Panel

A view panel that shows its stage rendered by a renderer that isn't Hydra's: V-Ray or RTX
in a helper process, say. The app makes the panel, and a plugin supplies the renderer. The
panel is the renderer-independent part:
- the toolbar and the image;
- following the viewport's camera, the panel's size and the stage's edits.

The renderer does the rendering, behind a handful of hooks.

- **C++:** [usdcc::usd::RenderViewPanel](../src/cpp/usd/include/usdcc/usd/render_view_panel.h),
  in `usdcc_usd_ui`.
- **Python:** `usdcc.ui.RenderViewPanel` ([ui.py](../src/python/usdcc/ui.py)), hosted by
  [ScriptedRenderViewPanel](../src/cpp/scripting/include/usdcc/scripting/scripted_render_view_panel.h).
- **Used by:** the V-Ray Render and RTX Render plugins ([VRAY_RENDER.md](VRAY_RENDER.md), [RTX_RENDER.md](RTX_RENDER.md)).

Plugins don't share code with each other. Anything two renderers both need belongs here.
Each renderer keeps its own helper process, protocol, stage export and settings.

## 1. The panel

| Control | What it does |
|---|---|
| Render / Stop | Calls the renderer's `start()` / `stop()`. While `setup_problem()` gives a reason, Render shows it and stays off |
| Refresh | `reload()`: send the whole stage again |
| Camera | **Viewport Camera**: the viewport the user last clicked in, when it shows this panel's stage. Or one of the stage's `UsdGeom.Camera` prims; the list follows the stage |
| Resolution | **Fit Panel** at full, ½ or ¼ of the image area (in device pixels), or 1280 × 720 or 1920 × 1080. The longest side is at most 8192 |
| *(the renderer's own)* | What `create_toolbar(layout)` adds, such as V-Ray's engine choice |
| Update on Edits | Edits reach the renderer (`stage_edited()`), `edit_delay()` after the last one |
| Save Image… | PNG, JPEG or TIFF of the image shown |
| Settings… | The Settings dialog at the renderer's `settings_category`; hidden without one. The options menu has it too |

- **Status line:** the renderer's state (`set_state()`), the seconds since the last reload or
  camera change, and the image's size.
- **Errors:** an error shows in the status line. A fatal one also stops rendering and shows in
  the image area, and from Python a sticky toast says so too. Logging stays the renderer's: it
  knows its log's name.
- **Closing the tab** only hides a panel, as with every view panel. Rendering stops there and
  then.
- **The renderer's `close()`** runs when the panel is deleted: by File > Close Stage, by a
  plugin reload, or at quit.

## 2. The camera

A `RenderCamera` is the camera at the image's aspect ratio:
- `transform`: camera-to-world;
- `fov_x`, `fov_y`: in degrees;
- `aspect`, `near`, `far`;
- `orthographic` and `ortho_width`;
- `prim`: the camera prim, or None.

Which camera it is:
- **The viewport's:** its vertical field of view (45°) is kept, so a wider image shows more
  at the sides.
- **A camera prim's:** `UsdGeom.Camera.GetCamera()` at the default time. Its horizontal
  field of view is kept, as its aperture frames it. An orthographic one's `ortho_width` is
  its horizontal aperture in scene units.
- **No viewport on the stage:** the last camera sent. Before any, 10 units back from the
  origin, as a new viewport starts.

When it's sent:
- Camera moves reach `camera_changed()` at most every 30 ms.
- A new image size reaches `size_changed()` 150 ms after a resize stops. A
  `camera_changed()` follows it, for the new aspect ratio.

`usdcc.app.active_viewport_camera()` uses the same viewport code (`viewportRenderCamera()`).

C++ renderers can call `setLocalCamera(RenderCamera)` to add/select **Free Camera**.
It holds viewing state in the panel and coalesces camera callbacks at the same
30 ms interval, without changing an authored camera or the followed viewport.
`usesLocalCamera()` reports whether it is selected. Existing viewport/prim choices
remain available, and a stage change discards the local camera. The native RTX
plugin uses this for image navigation; ordinary Python render panels keep their
existing behavior.

## 3. Edits

The panel collects the stage's change notices while Update on Edits is on. It calls
`stage_edited(resynced, changed)` with all of them at once, `edit_delay()` (500 ms at first)
after the last.

- **By default, an edit calls `reload()`.**
- **A renderer that can update in place overrides `stage_edited()`.** For example, an RTX
  renderer writes a changed transform instead of loading the stage again.

So whether an edit means a reload is the renderer's choice, not the panel's. A resync also
refreshes the Camera list, with Update on Edits on or off.

## 4. A renderer in Python

```python
import usdcc.app, usdcc.ui

class MyRenderPanel(usdcc.ui.RenderViewPanel):
    renderer_name = "My Renderer"      # "Press Render to render the stage with My Renderer."
    settings_category = "My Renderer"  # what Settings… opens

    def start(self):
        width, height = self.render_size()
        camera = self.camera()
        ...                            # render; later, from a signal:
        self.show_image(qimage)
        self.set_state("Done")

usdcc.app.register_view_panel("my.render", "My Render", MyRenderPanel)
```

`register_view_panel()` takes a `RenderViewPanel` subclass as well as a `ViewPanel` one.
Each panel makes one instance of the class, without arguments.

**Hooks**, all optional:

| Hook | When |
|---|---|
| `setup_problem()` | When the panel is made and when Render is pressed. A string stops Render and is shown |
| `create_toolbar(layout)` | Once, at the start. `layout` is a `QHBoxLayout` on the toolbar |
| `start()`, `stop()` | Render pressed, Stop pressed |
| `reload()` | Refresh, another stage, and by default an edit |
| `camera_changed(camera)` | The camera moved or another was picked: a `RenderCamera` |
| `size_changed(width, height)` | The image's size changed |
| `stage_edited(resynced, changed)` | Edits, as Sdf.Paths (§3) |
| `stage_changed(stage)` | Another stage, or None. A `reload()` follows when there is one |
| `populate_options_menu(menu)` | The options menu opens |
| `close()` | The panel is going away. Called once |

**Calls:**

| Call | What it does |
|---|---|
| `show_image(qimage)`, `image_size()` | Show an image (copied), and the size of the one shown |
| `set_state(text)`, `show_message(text)` | The status line's state; a message in place of the image |
| `report_error(message, fatal=False)` | §1 |
| `set_rendering(on)`, `is_rendering()` | Check or uncheck Render without calling `start()`/`stop()`, for a renderer that stopped by itself |
| `set_edit_delay(ms)`, `edit_delay()`, `updates_on_edits()` | §3 |
| `camera()`, `render_size()` | What the image is seen through now, and its size |
| `widget`, `stage`, `is_open()` | The panel's content widget, the stage shown, and whether the panel is still there |

- **Exceptions** raised from a hook are logged (`python.exception`).
- **After `close()`** the instance has let go of the panel: `is_open()` is False, `widget` is
  None, and the calls raise RuntimeError. A queued signal arriving late can't reach a panel
  that's gone. Check `is_open()` in a slot that may run then.

## 5. A renderer in C++

A C++ plugin can subclass `RenderViewPanel` directly:

1. Override the hooks: `setupProblem()`, `startRender()`, `stopRender()`, `reload()`,
   `cameraChanged()`, `sizeChanged()`, `stageEdited()` and `closing()`.
2. Call `setRendererName()`, `setSettingsCategory()` and `setEditDelay()`, then end the
   constructor with `finishSetup()`.
3. Start the destructor with `closePanel()`, which calls `closing()` while the subclass is
   still whole.
4. Implement `duplicate()`.

## 6. How it's built

- **Why hosted, not bound.** `ViewPanel` isn't bound through Shiboken (SCRIPTING.md §8).
  So, as with `ScriptedViewPanel`, the panel is C++ and Python only supplies the renderer.
  `ScriptedRenderViewPanel` forwards each hook to the instance through the bridge functions
  at the end of `ui.py`.
- **The instance's calls** go back through a pybind11 `RenderViewPanelHandle` in
  `_usdcc_app`. It holds a `QPointer`, so it raises once the panel is gone.
- **Images** cross as a PySide `QImage`'s address (`shiboken6.getCppPointer`) and are copied.
  PySide is built against usdcc's own Qt.
- **The widget wrappers.** The instance keeps one Shiboken wrapper of the content widget and
  one of the toolbar layout for the panel's life. When the panel closes they're invalidated
  (`shiboken6.invalidate`), and with them the wrappers of what `findChild()` found in them.
  Two wrong ways were tried:
  - **A fresh wrapper on each use** doesn't work: freeing it invalidates the wrappers of the
    children found through it.
  - **One that is never invalidated** would outlive the widget.
- **Plugin reload:** `rebuildScriptedPanels()` gives an open panel an instance of the new
  class. The old instance is closed first, and its controls leave the toolbar.

## 7. Tests

| Test | What it covers |
|---|---|
| `render_view_panel` (usdcc_ui_tests, [render_view_panel_test.cpp](../tests/render_view_panel_test.cpp)) | A C++ renderer: 50 panels made and closed; 30 docked by the registry and deleted; sizes from the Resolution choice and resizes; the default camera; a camera prim's field of view and position; the camera list following the stage; edits coming together after the delay, and none with Update on Edits off; images and the status line; a fatal error; a setup problem; Duplicate; `closing()` once |
| `python_render_view_panel` (usdcc_python_tests, [render_view_panel_python_test.cpp](../tests/render_view_panel_python_test.cpp)) | A Python renderer: 20 panels opened and deleted; its toolbar control; every hook; an image handed back; edits; a fatal error; a plugin reload's rebuild; and the instance letting go when closed |
| `vray_render_plugin` (usdcc_python_tests) | The V-Ray plugin on the panel, with its fake renderer, the viewport's camera included (VRAY_RENDER.md §7) |

- **Delete a docked panel the way File > Close Stage does: `deleteDockWidget()`.** A plain
  `delete` leaves the dock manager holding it, and the next panel opened reads freed memory.
  The tests used to do that and got away with it only while one panel came after.
- **Make a `UiScaleController` for a panel test's lifetime,** as the other panel tests do.
  Without one, `scale_controller_test` fails when it runs later in the same process.

## 8. Limits

- **Time:** cameras and edits are at the default time; usdcc has no timeline yet.
- **One image:** the panel shows one RGBA image. AOVs, tiles in progress, and HDR or
  exposure controls are the renderer's for now.
- **Hiding the tab** stops rendering but keeps the renderer's process. Only deleting the panel
  calls `close()`.
