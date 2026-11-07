"""usdcc.ui: base classes for UI made in Python (docs/SCRIPTING.md section 8).

``SidePanel`` is the base class for side panels; dock one with
``usdcc.app.add_side_panel()``. ``ViewPanel`` is the base class for view
panels, the tabs in the centre that show one stage each; register one with
``usdcc.app.register_view_panel()``. ``RenderViewPanel`` is the base class for a
view panel that renders its stage with a renderer of a plugin's own
(docs/RENDER_VIEW_PANEL.md); it is registered the same way. Needs PySide6 built
for usdcc (tools/build-pyside.ps1); without it, importing this raises ImportError.
"""

# PySide6 first: importing it adds its folder (Qt, shiboken6 and pyside6
# DLLs) to the DLL search path that _ui.pyd needs.
import shiboken6 as _shiboken
from PySide6 import QtGui as _QtGui
from PySide6 import QtWidgets as _QtWidgets
from pxr import Tf as _Tf

# _ui.pyd also links OpenUSD's DLLs (through usdcc_ui), which Python 3.8+ only
# finds in folders added with os.add_dll_directory; pxr's own import helper
# adds the PATH folders for the duration of the import (as in usdcc.usd).
_wrapper = getattr(_Tf, "WindowsImportWrapper", None)
if _wrapper is not None:
    with _wrapper():
        from usdcc import _ui
else:
    from usdcc import _ui

# Shiboken6 puts the class at _ui.usdcc.ui.SidePanel: it needs the C++
# namespaces spelled out in the typesystem (src/cpp/ui/python/typesystem.xml),
# and flattening them hits a Shiboken6 codegen bug.
SidePanel = _ui.usdcc.ui.SidePanel

del _wrapper, _Tf


class ViewPanel:
    """Base class for view panels made in Python.

    Subclass it and register the class with ``usdcc.app.register_view_panel()``.
    The app then lists it in the Stages menu and can open any number of these
    panels, each showing one stage (or none). Each panel is a regular view
    panel, with the options button and Stage dropdown, docking and floating
    like the built-in ones; it creates one instance of the class, which fills
    it:

    - ``create_widget()`` (required) returns the panel's content widget, which
      the panel takes ownership of. Called once, when the panel is made.
    - ``stage_changed(stage)`` is called with the shown ``Usd.Stage`` (or
      ``None``) right after that and whenever the user picks another one.
    - ``populate_options_menu(menu)`` may add actions to the options menu (a
      ``QMenu``, built afresh each time it opens).

    ``self.stage`` is the stage shown, ``self.widget`` the content widget.
    Exceptions raised from these methods are logged (python.exception).
    """

    stage = None
    widget = None

    def create_widget(self):
        raise NotImplementedError("%s must implement create_widget()" % type(self).__name__)

    def stage_changed(self, stage):
        pass

    def populate_options_menu(self, menu):
        pass


class RenderCamera:
    """A camera as a ``RenderViewPanel`` gives it to its renderer: the viewport's,
    or a camera prim's, at the image's aspect ratio.

    - ``transform``: camera-to-world, a Gf.Matrix4d; the camera looks down -Z, +Y up
    - ``fov_x``, ``fov_y``: the horizontal and vertical fields of view, in degrees
    - ``aspect``: width / height of the image
    - ``near``, ``far``: the clipping range
    - ``orthographic``: True for an orthographic camera prim, whose ``ortho_width``
      is what it sees across, in scene units
    - ``prim``: the camera prim's Sdf.Path, or None for the viewport's camera
    """

    __slots__ = ("aspect", "far", "fov_x", "fov_y", "near", "ortho_width", "orthographic", "prim", "transform")

    def __repr__(self):
        position = tuple(self.transform.ExtractTranslation())
        return f"RenderCamera(prim={self.prim}, position={position}, fov_x={self.fov_x:.2f}, aspect={self.aspect:.3f})"


class RenderViewPanel:
    """Base class for a view panel that renders its stage with a renderer of a
    plugin's own, such as V-Ray in a helper process (docs/RENDER_VIEW_PANEL.md).

    The app makes the panel: the toolbar (Render, Refresh, Camera, Resolution,
    Update on Edits, Save Image, Settings), the image, the status line, and
    following the viewport's camera, the panel's size and the stage's edits. A
    subclass renders. Register it with ``usdcc.app.register_view_panel()``; each
    panel creates one instance and calls its hooks:

    - ``setup_problem()``: why it can't render (no SDK, say), or None. While there
      is one, Render shows it and does nothing.
    - ``create_toolbar(layout)``: add controls of its own (an engine choice) to
      ``layout``, a QHBoxLayout on the toolbar. Called once, at the start.
    - ``start()``, ``stop()``: Render was pressed, or Stop.
    - ``reload()``: send the whole stage again (Refresh, another stage, an edit).
    - ``camera_changed(camera)``: a ``RenderCamera``, after the camera moved or
      another was picked; at most every 30 ms.
    - ``size_changed(width, height)``: the image's size in pixels; a
      ``camera_changed()`` follows, for the new aspect ratio.
    - ``stage_edited(resynced, changed)``: the stage was edited (the panel waits
      ``edit_delay()`` after the last edit) while Update on Edits is on, with the
      Sdf.Paths changed since the last call. Calls ``reload()`` here; a renderer that
      can update in place overrides it.
    - ``stage_changed(stage)``: the panel shows another stage (or None); a
      ``reload()`` follows when there is one.
    - ``populate_options_menu(menu)``: entries for the options menu.
    - ``close()``: the panel is going away; end what was started. Called once.

    It calls back with ``show_image()``, ``set_state()``, ``show_message()``,
    ``report_error()``, ``set_rendering()`` and ``set_edit_delay()``, and reads
    ``camera()`` and ``render_size()``. ``self.stage`` is the stage shown,
    ``self.widget`` the panel's content widget (the app's, so don't keep it past
    ``close()``). Exceptions raised from the hooks are logged (python.exception).
    """

    # In "Press Render to render the stage with ...", and the error toast's title.
    renderer_name = None
    # The Settings dialog's category that Settings… opens; None hides the button.
    settings_category = None

    stage = None
    _panel = None  # the app's side, set before any hook is called
    # Python's wrappers of the app's widgets: one each, kept for the panel's life
    # (the wrappers of what findChild() finds in them live as long), and
    # invalidated when it closes, so Python can't reach them afterwards.
    _widget = None
    _toolbar = None

    @property
    def widget(self):
        """The panel's content widget, or None once the panel is gone."""
        return self._widget if self.is_open() else None

    # Hooks.

    def setup_problem(self):
        return None

    def create_toolbar(self, layout):
        pass

    def start(self):
        pass

    def stop(self):
        pass

    def reload(self):
        pass

    def camera_changed(self, camera):
        pass

    def size_changed(self, width, height):
        pass

    def stage_edited(self, resynced, changed):
        self.reload()

    def stage_changed(self, stage):
        pass

    def populate_options_menu(self, menu):
        pass

    def close(self):
        pass

    # Calls.

    def show_image(self, image):
        """Shows a QImage (copied)."""
        if not isinstance(image, _QtGui.QImage):
            raise TypeError("show_image() needs a QImage, not %s" % type(image).__name__)
        self._panel.show_image(_shiboken.getCppPointer(image)[0])

    def image_size(self):
        """The (width, height) of the image shown, or None."""
        return self._panel.image_size()

    def set_state(self, state):
        """What the renderer is doing, for the status line: "Rendering", "Done", ..."""
        self._panel.set_state(state)

    def show_message(self, message):
        """A message in the image area; "" shows the image again."""
        self._panel.show_message(message)

    def report_error(self, message, fatal=False):
        """Shows an error in the status line. A fatal one also stops rendering, says
        so in the image area and in a toast. Logging it is the caller's."""
        self._panel.report_error(message, fatal)
        if fatal:
            from usdcc import toast

            title = "%s Render" % self.renderer_name if self.renderer_name else "Render"
            toast.show(title, message + " The Log panel has more.", kind="error", sticky=True)

    def set_rendering(self, on):
        """Checks or unchecks Render without calling ``start()`` or ``stop()``, for
        when the renderer stopped by itself."""
        self._panel.set_rendering(on)

    def is_rendering(self):
        return self._panel.is_rendering()

    def set_edit_delay(self, ms):
        """How long after the last edit ``stage_edited()`` is called (500 ms at first)."""
        self._panel.set_edit_delay(int(ms))

    def edit_delay(self):
        return self._panel.edit_delay()

    def updates_on_edits(self):
        """Whether Update on Edits is checked."""
        return self._panel.updates_on_edits()

    def camera(self):
        """The ``RenderCamera`` the image is seen through now."""
        return _render_camera(self._panel.camera())

    def render_size(self):
        """The image's (width, height) in pixels."""
        return tuple(self._panel.render_size())

    def is_open(self):
        """False once the panel is gone; the calls above then raise RuntimeError."""
        return self._panel is not None and self._panel.alive()


# The C++ side of a Python view panel (ScriptedViewPanel) calls these with
# addresses and cache ids; they hand PySide objects and Usd.Stages on.


def _create_view_panel(cls, host_address):
    host = _shiboken.wrapInstance(host_address, _QtWidgets.QWidget)
    panel = cls()
    widget = panel.create_widget()
    if not isinstance(widget, _QtWidgets.QWidget):
        raise TypeError("%s.create_widget() returned %s, not a QWidget" % (cls.__name__, type(widget).__name__))
    # Parenting it from Python hands its ownership to the panel.
    widget.setParent(host)
    host.layout().addWidget(widget)
    panel.widget = widget
    return panel


def _view_panel_stage_changed(panel, cache_id):
    from usdcc import usd

    panel.stage = usd._stage(cache_id) if cache_id >= 0 else None
    panel.stage_changed(panel.stage)


def _view_panel_options(panel, menu_address):
    panel.populate_options_menu(_shiboken.wrapInstance(menu_address, _QtWidgets.QMenu))


# The C++ side of a render view panel (ScriptedRenderViewPanel) calls these.


def _create_render_view_panel(cls, handle, toolbar_address):
    panel = cls()
    if not isinstance(panel, RenderViewPanel):
        raise TypeError("%s isn't a usdcc.ui.RenderViewPanel" % cls.__name__)
    panel._panel = handle
    panel._widget = _shiboken.wrapInstance(handle.content_address(), _QtWidgets.QWidget)
    panel._toolbar = _shiboken.wrapInstance(toolbar_address, _QtWidgets.QHBoxLayout)
    panel.create_toolbar(panel._toolbar)
    return panel


class _ClosedPanel:
    """What a closed RenderViewPanel calls: nothing, so a late signal can't reach a
    panel that is gone, or one a plugin reload gave to a new instance."""

    def alive(self):
        return False

    def __getattr__(self, name):
        raise RuntimeError("this render view panel has been closed")


def _close_render_view_panel(panel):
    """close(), then the instance lets go of the panel: Python's wrappers of its
    widgets, which are about to go, are invalidated, and its calls raise."""
    try:
        panel.close()
    finally:
        panel._panel = _ClosedPanel()
        for name in ("_widget", "_toolbar"):
            wrapper = getattr(panel, name)
            setattr(panel, name, None)
            if wrapper is not None and _shiboken.isValid(wrapper):
                _shiboken.invalidate(wrapper)


def _render_camera(values):
    from pxr import Gf, Sdf

    prim, matrix, fov_x, fov_y, aspect, near, far, orthographic, ortho_width = values
    camera = RenderCamera()
    camera.transform = Gf.Matrix4d(*matrix)
    camera.fov_x = fov_x
    camera.fov_y = fov_y
    camera.aspect = aspect
    camera.near = near
    camera.far = far
    camera.orthographic = orthographic
    camera.ortho_width = ortho_width
    camera.prim = Sdf.Path(prim) if prim else None
    return camera


def _render_stage_edited(panel, resynced, changed):
    from pxr import Sdf

    panel.stage_edited([Sdf.Path(p) for p in resynced], [Sdf.Path(p) for p in changed])
