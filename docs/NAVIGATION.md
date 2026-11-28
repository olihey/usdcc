# Navigation

How a view is moved with the mouse: orbit, pan and zoom. Every view goes by one **scheme**,
chosen in Settings > Navigation: usdcc's own, Maya's, Blender's, 3ds Max's, or a custom one.
The wheel, a trackpad and a pinch do the same thing in every view, at the same speed.

## 1. The views

| View | Kind | Orbit | Pan | Zoom | Its own |
|---|---|---|---|---|---|
| Viewport (`HydraViewportWindow`) | 3D | the scheme's | the scheme's | the scheme's, the wheel, a pinch | A left drag no tool takes orbits (the Material editor's preview). The right button's menu opens unless the button was dragged to navigate |
| RTX panel (`usdcc::rtx::NavigationInput`) | 3D | the scheme's | the scheme's | the scheme's, the wheel, a pinch | A left drag orbits: it has no tools. `Home` resets the camera |
| Curve editor (`CurveView`) | 2D | | the scheme's | the scheme's, the wheel, a pinch | The wheel with Ctrl zooms the time only, with Shift the values only. A zoom drag's sideways part zooms the time, the rest the values |
| Dope sheet (`DopeSheetView`) | 2D | | the scheme's, the time | the scheme's, Ctrl and the wheel, a pinch | The wheel alone scrolls the rows, as the tree beside it does |
| UV view (`UvCanvas`) | 2D | | the scheme's | the scheme's, the wheel, a pinch | A left drag pans too: it has nothing else to do there |
| Material editor (QtNodes' view) | 2D | | the scheme's | the scheme's, the wheel, a pinch | A left drag on nothing pans too, and with Shift draws a box, as QtNodes has it |

- **A 3D view and a 2D editor are bound apart**, as they are in the applications the presets
  follow: Blender's middle button orbits a 3D view and pans a 2D one.
- **Zoom** is one step for a wheel notch: everything nine tenths as far away, or nine tenths of
  the range shown (`kNavigationZoomPerStep`). A zoom drag is a step every 24 pixels, in to the
  right and up. A 2D editor zooms about the cursor, or about where the drag began; a 3D view
  towards the middle of the view, or with **Zoom to cursor** along the cursor's ray.
- **The key that frames** is Settings > Shortcuts > Fit to view (F) in every view: the
  viewport has it as an application-wide action, and the view with the focus claims it first
  (`claimFrameKey()`).

## 2. Schemes

A scheme binds a mouse button with modifier keys to orbit, pan and zoom, once for 3D views and
once for 2D editors.

| Scheme | 3D orbit | 3D pan | 3D zoom | 2D pan | 2D zoom |
|---|---|---|---|---|---|
| **usdcc** (default) | Alt+Left | Middle | Alt+Right | Middle | Alt+Right |
| **Maya** | Alt+Left | Alt+Middle | Alt+Right | Alt+Middle | Alt+Right |
| **Blender** | Middle | Shift+Middle | Ctrl+Middle | Middle | Ctrl+Middle |
| **3ds Max** | Alt+Middle | Middle | Ctrl+Alt+Middle | Middle | Ctrl+Alt+Middle |
| **Custom** | whatever the five bindings say | | | | |

- **A press** does what the binding with exactly its modifiers says. Failing that, a middle
  button bound without modifiers answers: Alt+Middle pans in the usdcc scheme as Middle does
  (`navigationAction()`).
- **Navigation comes before the view's own clicks.** In the viewport a bound drag never reaches
  the active tool; in an editor it never picks or moves a key.
- **What can't be bound**: the left button without Alt, which is the tools' and the
  selection's, and the right button alone, which opens menus. Settings says why and keeps the
  binding as it was (`navigationBindingProblem()`).
- **Houdini has no preset.** Its Alt drags are the usdcc scheme's. Its other way, holding
  Space, needs a held key that a binding can't say.
- On macOS, Ctrl is the Command key and Alt the Option key, as everywhere in Qt.

## 3. Settings > Navigation

| Key | Type | Default | |
|---|---|---|---|
| `Navigation/scheme` | Choice: usdcc, Maya, Blender, 3ds Max, Custom | usdcc | Choosing a preset writes its bindings to the five below; Custom leaves them |
| `Navigation/view3d.orbit`, `view3d.pan`, `view3d.zoom` | MouseBinding | Alt+Left, Middle, Alt+Right | The bindings in use for 3D views |
| `Navigation/editor2d.pan`, `editor2d.zoom` | MouseBinding | Middle, Alt+Right | And for 2D editors |
| `Navigation/zoomSpeed` | Float 0.1–5 | 1 | How many steps a wheel notch is; a zoom drag too |
| `Navigation/invertZoom` | Bool | off | The wheel and a zoom drag go the other way |
| `Navigation/zoomToCursor` | Bool | off | 3D views: the wheel moves the camera towards what the cursor is on |
| `Navigation/trackpadScroll` | Choice: Zoom, Pan and orbit | Zoom | §4 |

- **The five bindings are always the ones in use.** Changing one names the scheme after the
  preset the five now are, or Custom. So a custom scheme starts from a preset: choose it, then
  change what should differ. Choosing another preset replaces all five.
- A preset saved by name gets the bindings it has in this release, should they change.
- **A binding is edited by pressing it**: click the field, then press the mouse button on it
  with the keys held. Escape leaves it; Backspace binds nothing. Two actions of one kind of view
  with the same binding are flagged on both, as shortcuts are; the first of orbit, pan, zoom
  gets the drag.
- `MouseBinding` is a setting type ([SETTINGS.md](SETTINGS.md) §2): the text `Alt+Left`, parsed
  and written by `usdcc/core/mouse_binding.h`. A plugin can define its own.

## 4. The wheel, a trackpad, a pinch

- **The wheel** zooms, in every scheme. A wheel that Shift or Alt turned sideways, as macOS
  does, still zooms.
- **A pinch** zooms about the cursor, by as much as the fingers moved apart
  (`Qt::ZoomNativeGesture`).
- **A two-finger scroll** zooms as a wheel does, or with **Two-finger scroll: Pan and orbit**:

  | | 3D view | 2D editor |
  |---|---|---|
  | Scroll | orbit | pan |
  | Shift+scroll | pan | pan |
  | Ctrl+scroll (Cmd on macOS) | zoom | zoom |

  A trackpad is told from a wheel by its events: it scrolls in pixels, in phases. Windows
  reports neither, so there a trackpad is always a wheel.

## 5. In code

`usdcc/ui/navigation.h`, in `usdcc_ui`:

| | |
|---|---|
| `NavigationScheme`, `navigationPreset()`, `navigationSchemeNames()` | The presets |
| `defineNavigationSettings()`, `navigationScheme()`, `navigationOptions()` | The settings, and what they say now |
| `navigationAction()` | What a press does in a scheme |
| `NavigationInput` | One view's navigation: `press()`, `move()`, `release()` for a drag, `wheel()` and `gesture()` for the rest, each giving a `NavigationStep` |
| `NavigationStep` | `action`; `delta` in pixels for an orbit or pan; `zoom` in steps, `zoomAxes` for a view with two, and the `anchor` that stays put |
| `navigationZoomFactor()` | Steps as a factor: 0.9 a step |
| `isFrameKey()`, `claimFrameKey()` | The key that frames |

A view keeps its own math (a camera, a `TimeAxis`) and nothing about buttons:

```cpp
usdcc::ui::NavigationInput m_navigation{usdcc::ui::NavigationSpace::Editor2D};

void MyView::mousePressEvent(QMouseEvent* event) {
    if (m_navigation.press(event) != usdcc::ui::NavigationAction::None) return;
    // ... the view's own clicks
}
void MyView::mouseMoveEvent(QMouseEvent* event) { navigate(m_navigation.move(event)); }
void MyView::mouseReleaseEvent(QMouseEvent* event) { m_navigation.release(event); }
void MyView::wheelEvent(QWheelEvent* event) { navigate(m_navigation.wheel(event)); }
bool MyView::event(QEvent* event) {
    if (usdcc::ui::claimFrameKey(event)) return true;
    if (const auto step = m_navigation.gesture(event)) { navigate(step); return true; }
    return QWidget::event(event);
}

void MyView::navigate(const usdcc::ui::NavigationStep& step) {
    if (step.action == usdcc::ui::NavigationAction::Pan) m_origin += step.delta;
    if (step.action == usdcc::ui::NavigationAction::Zoom) zoomAbout(step.anchor, 1.0 / usdcc::ui::navigationZoomFactor(step.zoom));
    update();
}
```

- `begin()` starts a drag the scheme doesn't name, such as the UV view's left drag.
- **The right button's menu.** A view that gets `QEvent::ContextMenu` passes it to
  `takeContextMenu()` in its `event()`: no menu opens when the right button's press began a
  drag (Alt+Right zooms in the usdcc scheme). Qt sends that event with the press on macOS and
  Linux and after the release on Windows, so a click with the binding's keys opens none
  either. The viewport opens its menu itself on the release, and does unless the drag
  `moved()`.
- The viewport's `orbitBy()`, `panBy()` and `zoomBy()`
  ([VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md) §6) are as they were: moves from elsewhere,
  not the mouse.

## 6. What changed for a view

Before this, each view read the mouse itself:

| | Before | Now |
|---|---|---|
| Zoom a notch | 0.9 in the viewport and RTX panel, 0.85 in the curve editor and dope sheet, 1.2 in the UV view, and 1.2 an event of any size in the Material editor | 0.9 everywhere, by how far the wheel turned |
| Zoom drag | none | the scheme's, in every view |
| Middle button in the Material editor | nothing | pans |
| Alt+Left in the UV view | panned | nothing in the usdcc scheme; a left drag pans as before |
| A pinch | nothing | zooms |
| F in the curve editor, dope sheet and UV view | read as the letter F, after the viewport's Fit to View had taken it | the Fit to view shortcut, claimed by the view with the focus |

## 7. Not done

- **A trackpad's scroll and pinch** are tested with made-up events only, not on a trackpad.
- **Driving a stage camera** ([VIEWPORT_CAMERA.md](VIEWPORT_CAMERA.md) §2.2) with a trackpad's
  scroll is an undo step an event, as the wheel's notches are.
- **Schemes as files**, to share them, as themes are. The five bindings are settings, so
  `usdcc.ini` and `usdcc.settings` carry them.
- **Keys that navigate** (Blender's numpad, Houdini's Space) and a fly mode.
- **The Material editor's left drag** stays QtNodes': pan on nothing, a box with Shift.

## 8. Tests

- `navigation_test.cpp` (`usdcc_ui_tests`): binding text; the presets and what a press does in
  each; the settings keeping the scheme's name and the bindings in step, refusing what can't
  be bound; `NavigationInput`'s steps for drags, the wheel, a trackpad's scroll and a pinch, and
  the right button's menu; the frame key, and the focused curve editor getting it ahead of an
  application-wide action; the dialog's binding editor; the curve editor and dope sheet panning
  and zooming by the scheme.
- `settings_test.cpp` (`usdcc_tests`): the `MouseBinding` type and a definition's `refuses`.
- `viewport_camera_test.cpp` (`usdcc_python_tests`), `viewport_navigates_by_the_scheme`: the
  usdcc and Blender drags, the right button's menu with and without a drag, Zoom to cursor
  keeping the cursor's point under it.
- `uv_view_test.cpp`, `material_editor_test.cpp`: their pan, wheel and zoom drag.
