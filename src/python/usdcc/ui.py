"""usdcc.ui: base classes for UI made in Python (docs/SCRIPTING.md section 8).

``SidePanel`` is the base class for side panels; dock one with
``usdcc.app.add_side_panel()``. ``ViewPanel`` is the base class for view
panels, the tabs in the centre that show one stage each; register one with
``usdcc.app.register_view_panel()``. ``RenderViewPanel`` is the base class for a
view panel that renders its stage with a renderer of a plugin's own
(docs/RENDER_VIEW_PANEL.md); it is registered the same way. Needs PySide6 built
for usdcc (tools/build_pyside.py); without it, importing this raises ImportError.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

# PySide6 first: importing it adds its folder (Qt, shiboken6 and pyside6
# DLLs) to the DLL search path that _ui.pyd needs.
import shiboken6 as _shiboken
from PySide6 import QtCore as _QtCore
from PySide6 import QtGui as _QtGui
from PySide6 import QtWidgets as _QtWidgets
from pxr import Tf as _Tf

if TYPE_CHECKING:
    import os
    from pxr import Gf, Sdf, Usd

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
if TYPE_CHECKING:

    class SidePanel(_QtWidgets.QFrame):
        """A panel without a stage, docked like the view panels or collapsed to a tab at an edge
        of the main window (``usdcc.app.add_side_panel()``)."""

        def __init__(self, title: str, parent: _QtWidgets.QWidget | None = None) -> None: ...
        def setWidget(self, widget: _QtWidgets.QWidget) -> None: ...
        def show(self) -> None: ...
        def hide(self) -> None: ...
        def raise_(self) -> None: ...
        def reveal(self) -> None: ...
        def isHidden(self) -> bool: ...
        def isCollapsed(self) -> bool: ...
        def setCollapsed(self, collapsed: bool) -> None: ...
        def toggleCollapsed(self) -> None: ...
        def canClose(self) -> bool: ...

else:
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
    - ``create_options_panel()`` may return a widget for the options instead of the
      menu: the options button opens it in a popup. Called once, when first needed;
      the panel takes ownership. ``None`` (the default) keeps the menu.
    - Either can be pinned, from the menu or the popup: then it stays in a column on
      the panel's left (a menu as checkboxes and buttons), until unpinned.
      ``options_changed()`` says what the options show changed outside them, so a
      pinned menu is built again.

    ``self.stage`` is the stage shown, ``self.widget`` the content widget.
    Exceptions raised from these methods are logged (python.exception).
    """

    stage: Usd.Stage | None = None
    widget: _QtWidgets.QWidget | None = None

    def create_widget(self) -> _QtWidgets.QWidget:
        raise NotImplementedError("%s must implement create_widget()" % type(self).__name__)

    def stage_changed(self, stage: Usd.Stage | None) -> None:
        pass

    def populate_options_menu(self, menu: _QtWidgets.QMenu) -> None:
        pass

    def create_options_panel(self) -> _QtWidgets.QWidget | None:
        return None

    def options_changed(self) -> None:
        """Says what the options show has changed, outside them: a pinned menu is
        built again, as a popup menu is each time it opens."""
        _notify_options_changed(self.widget)

    def busy(self, text: str, cancellable: bool = False) -> Busy:
        """The panel is busy while the returned context manager is open
        (docs/PANEL_BUSY.md §6): it dims, and a card shows ``text``, a progress bar
        and, with ``cancellable``, Cancel::

            with self.busy("Baking lights", cancellable=True) as busy:
                for i, item in enumerate(items):
                    if busy.cancelled:
                        break
                    bake(item)
                    busy.progress = (i + 1) / len(items)

        On the UI thread, as a panel's code usually runs, the card shows at once and
        setting ``progress``, ``text`` or ``detail`` repaints it; from another thread
        it shows after a moment and follows on its own."""
        return _busy(self.widget, text, cancellable)

    def save_state(self) -> Any:
        """What a session keeps of this panel (docs/SESSIONS.md §2.4): anything that
        converts to JSON, such as a dict. None keeps nothing."""
        return None

    def restore_state(self, state: Any) -> None:
        """Gets back what ``save_state()`` returned, when a session is loaded."""
        pass


class Busy:
    """What ``ViewPanel.busy()`` returns: a context manager. ``progress`` (0 to 1, or
    None for an indeterminate bar), ``text`` and ``detail`` (a second line) update the
    card; ``cancelled`` is True once Cancel was clicked, Esc pressed or the panel
    closed. Leaving the ``with`` block, or ``finish()``, ends it."""

    def __init__(self, native: Any, text: str) -> None:
        self._native = native
        self._text = text
        self._detail = ""
        self._progress = None

    def __enter__(self) -> Busy:
        return self

    def __exit__(self, *exc: object) -> bool:
        self.finish()
        return False

    def finish(self) -> None:
        self._native.finish()

    @property
    def cancelled(self) -> bool:
        return self._native.is_cancelled()

    @property
    def progress(self) -> float | None:
        return self._progress

    @progress.setter
    def progress(self, fraction: float | None) -> None:
        self._progress = fraction
        self._native.set_progress(-1.0 if fraction is None else float(fraction))

    @property
    def text(self) -> str:
        return self._text

    @text.setter
    def text(self, text: str) -> None:
        self._text = text
        self._native.set_text(str(text))

    @property
    def detail(self) -> str:
        return self._detail

    @detail.setter
    def detail(self, text: str) -> None:
        self._detail = text
        self._native.set_detail(str(text))


def _busy(widget, text, cancellable):
    import threading

    import _usdcc_app

    if widget is None:
        raise RuntimeError("the panel has no widget yet")
    on_ui_thread = threading.current_thread() is threading.main_thread()
    native = _usdcc_app.begin_busy(_shiboken.getCppPointer(widget)[0], str(text), bool(cancellable), on_ui_thread)
    return Busy(native, text)


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

    aspect: float
    far: float
    fov_x: float
    fov_y: float
    near: float
    ortho_width: float
    orthographic: bool
    prim: Sdf.Path | None
    transform: Gf.Matrix4d

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
    - ``time_changed(time)``: the stage's time changed (docs/ANIMATION.md phase 5),
      coalesced while it plays. ``time()`` is the time to render. Here: a camera prim
      is sent again where it is then, and with Update on Edits on and anything
      animated, ``reload()``. A renderer that can move in time in place overrides it.
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
    renderer_name: str | None = None
    # The Settings dialog's category that Settings… opens; None hides the button.
    settings_category: str | None = None

    stage = None
    _panel = None  # the app's side, set before any hook is called
    # Python's wrappers of the app's widgets: one each, kept for the panel's life
    # (the wrappers of what findChild() finds in them live as long), and
    # invalidated when it closes, so Python can't reach them afterwards.
    _widget = None
    _toolbar = None

    @property
    def widget(self) -> _QtWidgets.QWidget | None:
        """The panel's content widget, or None once the panel is gone."""
        return self._widget if self.is_open() else None

    # Hooks.

    def setup_problem(self) -> str | None:
        return None

    def create_toolbar(self, layout: _QtWidgets.QHBoxLayout) -> None:
        pass

    def start(self) -> None:
        pass

    def stop(self) -> None:
        pass

    def reload(self) -> None:
        pass

    def camera_changed(self, camera: RenderCamera) -> None:
        pass

    def size_changed(self, width: int, height: int) -> None:
        pass

    def time_changed(self, time: float) -> None:
        self._panel.default_time_changed(time)

    def stage_edited(self, resynced: list[Sdf.Path], changed: list[Sdf.Path]) -> None:
        self.reload()

    def stage_changed(self, stage: Usd.Stage | None) -> None:
        pass

    def populate_options_menu(self, menu: _QtWidgets.QMenu) -> None:
        pass

    def options_changed(self) -> None:
        """Says what the options show has changed: a pinned menu is built again."""
        _notify_options_changed(self._widget)

    def save_state(self) -> Any:
        """What a session keeps of this panel (docs/SESSIONS.md §2.4): anything that
        converts to JSON, such as a dict. None keeps nothing."""
        return None

    def restore_state(self, state: Any) -> None:
        """Gets back what ``save_state()`` returned, when a session is loaded."""
        pass

    def close(self) -> None:
        pass

    # Calls.

    def show_image(self, image: _QtGui.QImage) -> None:
        """Shows a QImage (copied)."""
        if not isinstance(image, _QtGui.QImage):
            raise TypeError("show_image() needs a QImage, not %s" % type(image).__name__)
        self._panel.show_image(_shiboken.getCppPointer(image)[0])

    def show_linear_image(self, image: _QtGui.QImage) -> None:
        """Shows a float QImage (Format_RGBA32FPx4 or RGBA16FPx4) of linear values in
        the working space (docs/COLOR_MANAGEMENT.md §6): the panel shows it through
        the view transform, Settings' or the panel's own (View Transform in its
        options), and again when either changes. Copied."""
        if not isinstance(image, _QtGui.QImage):
            raise TypeError("show_linear_image() needs a QImage, not %s" % type(image).__name__)
        self._panel.show_linear_image(_shiboken.getCppPointer(image)[0])

    def image_size(self) -> tuple[int, int] | None:
        """The (width, height) of the image shown, or None."""
        return self._panel.image_size()

    def set_state(self, state: str) -> None:
        """What the renderer is doing, for the status line: "Rendering", "Done", ..."""
        self._panel.set_state(state)

    def show_message(self, message: str) -> None:
        """A message in the image area; "" shows the image again."""
        self._panel.show_message(message)

    def report_error(self, message: str, fatal: bool = False) -> None:
        """Shows an error in the status line. A fatal one also stops rendering, says
        so in the image area and in a toast. Logging it is the caller's."""
        self._panel.report_error(message, fatal)
        if fatal:
            from usdcc import toast

            title = "%s Render" % self.renderer_name if self.renderer_name else "Render"
            toast.show(title, message + " The Log panel has more.", kind="error", sticky=True)

    def set_rendering(self, on: bool) -> None:
        """Checks or unchecks Render without calling ``start()`` or ``stop()``, for
        when the renderer stopped by itself."""
        self._panel.set_rendering(on)

    def is_rendering(self) -> bool:
        return self._panel.is_rendering()

    def set_edit_delay(self, ms: int) -> None:
        """How long after the last edit ``stage_edited()`` is called (500 ms at first)."""
        self._panel.set_edit_delay(int(ms))

    def edit_delay(self) -> int:
        return self._panel.edit_delay()

    def updates_on_edits(self) -> bool:
        """Whether Update on Edits is checked."""
        return self._panel.updates_on_edits()

    def camera(self) -> RenderCamera:
        """The ``RenderCamera`` the image is seen through now."""
        return _render_camera(self._panel.camera())

    def render_size(self) -> tuple[int, int]:
        """The image's (width, height) in pixels."""
        return tuple(self._panel.render_size())

    def time(self) -> float:
        """The time to render: the stage's, as the timeline shows it."""
        return self._panel.time()

    def stage_is_animated(self) -> bool:
        """Whether anything on the stage is animated."""
        return self._panel.stage_is_animated()

    def is_open(self) -> bool:
        """False once the panel is gone; the calls above then raise RuntimeError."""
        return self._panel is not None and self._panel.alive()


def _notify_options_changed(widget):
    # The panel is the content's first ancestor that has the slot.
    while widget is not None:
        if widget.metaObject().indexOfMethod("notifyOptionsChanged()") >= 0:
            _QtCore.QMetaObject.invokeMethod(widget, "notifyOptionsChanged")
            return
        widget = widget.parentWidget()


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


def _view_panel_options_panel(panel, host_address):
    widget = panel.create_options_panel()
    if widget is None:
        return False
    if not isinstance(widget, _QtWidgets.QWidget):
        raise TypeError("%s.create_options_panel() returned %s, not a QWidget" % (type(panel).__name__, type(widget).__name__))
    host = _shiboken.wrapInstance(host_address, _QtWidgets.QWidget)
    # Parenting it from Python hands its ownership to the panel.
    widget.setParent(host)
    host.layout().addWidget(widget)
    return True


def _view_panel_save_state(panel):
    import json

    state = panel.save_state()
    return "" if state is None else json.dumps(state)


def _view_panel_restore_state(panel, text):
    import json

    panel.restore_state(json.loads(text) if text else None)


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


def set_prim_icon(type_name: str, path: str | os.PathLike[str]) -> None:
    """Gives prims of `type_name` (a schema's name, "MyStudioLight") the icon in the
    file at `path`, in the Outliner, the Attributes panel and wherever prims show
    their icons (docs/PRIM_ICONS.md phase 2). An SVG is drawn as the built-in icons
    are: ``currentColor`` in the theme's grey, white on a selected row, faded for an
    inactive prim, badged for references, payloads and instances. Other images are
    drawn as they are. Types derived from `type_name` without an icon of their own
    get it too. ``None`` (or "") goes back to the built-in icon. For a startup
    script, so a pipeline's schemas have their icons."""
    import os

    import _usdcc_app

    _usdcc_app.set_prim_icon(str(type_name), "" if path is None else os.fspath(path))


def prim_icon(type_name: str) -> str:
    """The icon prims of `type_name` get: a built-in icon's name ("cube", "bulb"), or
    "file:" and the path set with ``set_prim_icon()``."""
    import _usdcc_app

    return _usdcc_app.prim_icon(str(type_name))
