"""usdcc.app: the running usdcc application (docs/SCRIPTING.md section 4.1).

``is_embedded()`` tells whether this python runs inside usdcc. Inside it,
``stage_manager()`` and ``undo_manager()`` are the app's own objects; most
scripts use the friendlier usdcc.usd functions instead. With PySide6,
``main_window()`` is the window, ``add_side_panel()`` docks a side panel made
in Python, and ``register_view_panel()`` adds a view panel type
(docs/SCRIPTING.md section 8). ``add_menu_action()``, ``add_prim_menu_action()``
and ``connect()`` are how plugins add to the app (docs/PLUGINS.md).
``active_viewport_camera()`` is what the viewport looks through;
``viewports()`` and ``active_viewport()`` give ``Viewport``s, whose renderer and
renderer settings a script can read and change. ``version()`` is usdcc's version.
"""

import math

from usdcc import _plugins

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    _native = None


def is_embedded():
    """True when running inside usdcc (and the app is up)."""
    return _native is not None and _native.is_available()


def _app():
    if _native is None:
        raise RuntimeError("not running inside usdcc")
    return _native


def version():
    """usdcc's version: "0.2.0" for a release, and for a development build the
    last release, the commits since it and the commit, as in "0.2.0+5.3b70ef2"
    (docs/RELEASE.md section 2)."""
    return _app().version()


def is_release_build():
    """True for a build made from a release, False for a development build."""
    return _app().is_release_build()


def stage_manager():
    """The app's StageManager (see usdcc.usd for working with its stages)."""
    return _app().stage_manager()


def undo_manager():
    """The app's UndoManager (see usdcc.usd.edit/undo/redo)."""
    return _app().undo_manager()


_AREAS = ("left", "right", "top", "bottom")


def main_window():
    """The app's main window, as a PySide6 ``QMainWindow``. Needs PySide6."""
    import shiboken6
    from PySide6 import QtWidgets

    return shiboken6.wrapInstance(_app().main_window_address(), QtWidgets.QMainWindow)


def add_side_panel(panel, area="right", id=None):
    """Docks ``panel``, a ``usdcc.ui.SidePanel``, at the window's ``area`` edge
    ("left", "right", "top" or "bottom") and lists it in the View menu.

    The window takes ownership of it. Its object name (the title it was made
    with) is how the saved layout knows it: a panel shown in an earlier session
    goes back where it was then, instead of to ``area``. Give a stable ``id`` to
    use as the object name instead, so that two panels with the same title don't
    clash; a plugin should. Needs PySide6.
    """
    import shiboken6
    from usdcc import ui

    if not isinstance(panel, ui.SidePanel):
        raise TypeError("add_side_panel() needs a usdcc.ui.SidePanel, not %s" % type(panel).__name__)
    if area not in _AREAS:
        raise ValueError("no dock area named %r (%s)" % (area, ", ".join(_AREAS)))
    # Parenting it from Python is what hands it over: from here on PySide
    # leaves deleting it to the window.
    if id is not None:
        panel.setObjectName(id)
    panel.setParent(main_window())
    _app().add_side_panel(shiboken6.getCppPointer(panel)[0], area)
    _plugins.note("Side panel", panel.windowTitle())
    _plugins.track(lambda: _remove_side_panel(panel))


def _remove_side_panel(panel):
    import shiboken6

    if shiboken6.isValid(panel):
        _app().remove_side_panel(shiboken6.getCppPointer(panel)[0])


def register_view_panel(id, name, cls):
    """Makes ``cls`` a view panel type named ``name`` (as the Stages menu shows
    it) with the stable ``id``. ``cls`` is a ``usdcc.ui.ViewPanel`` subclass, or a
    ``usdcc.ui.RenderViewPanel`` subclass for a renderer (docs/RENDER_VIEW_PANEL.md).

    Registering an id again replaces that type, so re-running the script that
    registers it picks up changes; panels already open keep their old class.
    The built-in types (``view_panel_types()``) can't be replaced. Needs PySide6.
    """
    from usdcc import ui

    if isinstance(cls, type) and issubclass(cls, ui.RenderViewPanel):
        _app().register_render_view_panel(id, name, cls)
    elif isinstance(cls, type) and issubclass(cls, ui.ViewPanel):
        _app().register_view_panel(id, name, cls)
    else:
        raise TypeError(
            "register_view_panel() needs a usdcc.ui.ViewPanel or usdcc.ui.RenderViewPanel subclass, not %r" % (cls,)
        )
    _plugins.register_panel_class(id, cls)
    _plugins.note("View panel", name)
    _plugins.track(lambda: unregister_view_panel(id))


def unregister_view_panel(id):
    """Removes a view panel type made in Python. Panels of it that are open stay, and
    say the type is gone. True if there was one."""
    _plugins.forget_panel_class(id)
    return _app().unregister_view_panel(id)


def open_view_panel(id, stage=None):
    """Opens a new view panel of type ``id`` showing ``stage`` (a ``Usd.Stage``
    open in the app; by default the current one), tabbed like one opened from
    the Stages menu. Raises RuntimeError if no stage is open.

    Returns the panel's Python object for a type registered from Python, None
    for a built-in one.
    """
    from usdcc import usd

    if stage is None:
        stage = usd.current_stage()
        if stage is None:
            raise RuntimeError("no stage is open to show in a view panel")
    return _app().open_view_panel(id, usd.stage_id(stage))


def view_panel_types():
    """The view panel types as (id, name) pairs, built-in ones first."""
    return _app().view_panel_types()


def current_plugin():
    """The ``Plugin`` whose ``activate()`` is running, or None."""
    return _plugins.current()


def add_menu_action(path, callback, shortcut="", tip=""):
    """Adds an entry to the main menu. ``path`` is "Menu/Action", or
    "Menu/Submenu/Action"; menus that don't exist yet are made, before Debug.

    ``callback()`` runs as one undo step, however many stages it edits, named after
    the action. An exception is logged, and the step is kept, marked as failed.
    ``shortcut`` is a key sequence such as "Ctrl+Shift+N". Adding a path again
    replaces its callback. Needs the main window.
    """
    _app().add_menu_action(path, callback, shortcut, tip)
    _plugins.note("Menu action", path)
    _plugins.track(lambda: remove_menu_action(path))


def remove_menu_action(path):
    """Removes an entry added with ``add_menu_action()``. True if there was one."""
    return _app().remove_menu_action(path)


def add_prim_menu_action(label, callback, applies=None, id=None):
    """Adds an entry to the end of the Outliner's prim menu. "Group/Action" in
    ``label`` makes a submenu. ``id`` says what to replace or remove later (by
    default, the label).

    ``callback(stage, paths, clicked)`` gets the Usd.Stage, the selected Sdf.Paths and
    the right-clicked Sdf.Path, and runs as one undo step. ``applies``, with the same
    arguments, returns whether the entry is in the menu this time.
    """
    from pxr import Sdf
    from usdcc import usd

    def arguments(cache_id, paths, clicked):
        return usd._stage(cache_id), [Sdf.Path(path) for path in paths], Sdf.Path(clicked)

    _app().add_prim_menu_action(
        id or label,
        label,
        lambda *plain: callback(*arguments(*plain)),
        None if applies is None else lambda *plain: bool(applies(*arguments(*plain))),
    )
    _plugins.note("Prim menu action", label)
    _plugins.track(lambda: remove_prim_menu_action(id or label))


def remove_prim_menu_action(id):
    """Removes an entry added with ``add_prim_menu_action()``, by its id or label."""
    _app().remove_prim_menu_action(id)


class ViewportCamera:
    """What ``active_viewport_camera()`` returns: a viewport's camera as plain values.

    - ``stage``: the Usd.Stage the viewport shows, or None
    - ``transform``: camera-to-world, a Gf.Matrix4d. The camera looks down its -Z
      axis with +Y up, as a UsdGeom.Camera does.
    - ``fov_y``: the vertical field of view in degrees
    - ``aspect``: width / height of the viewport
    - ``near``, ``far``: the clipping range
    """

    __slots__ = ("stage", "transform", "fov_y", "aspect", "near", "far")

    def fov_x(self):
        """The horizontal field of view in degrees."""
        return math.degrees(2.0 * math.atan(math.tan(math.radians(self.fov_y) / 2.0) * self.aspect))

    def __repr__(self):
        position = self.transform.ExtractTranslation()
        return f"ViewportCamera(position={tuple(position)}, fov_y={self.fov_y:.1f}, aspect={self.aspect:.3f})"


def active_viewport_camera():
    """The camera of the viewport the user last worked in (the last one clicked, or
    else the oldest one open), as a ``ViewportCamera``. None with no viewport open.
    "viewport_camera_changed" (see ``connect()``) says when it changes."""
    from pxr import Gf
    from usdcc import usd

    plain = _app().active_viewport_camera()
    if plain is None:
        return None
    cache_id, matrix, fov_y, aspect, near, far = plain
    camera = ViewportCamera()
    camera.stage = usd._stage(cache_id)
    camera.transform = Gf.Matrix4d(*matrix)
    camera.fov_y = fov_y
    camera.aspect = aspect
    camera.near = near
    camera.far = far
    return camera


class RendererSetting:
    """One of a render delegate's own settings, as ``renderer_settings_info()`` lists them.

    - ``key``: what the delegate takes it by ("ty:maxBounces")
    - ``label``: what it calls it ("Max Bounces")
    - ``type``: "bool", "int", "float" or "string"
    - ``default``: the delegate's own default
    - ``settings_key``: its app-wide default's key in ``usdcc.settings``
      ("Renderers/HdEmbreeRendererPlugin/ty:maxBounces")
    """

    __slots__ = ("key", "label", "type", "default", "settings_key")

    def __repr__(self):
        return f"RendererSetting({self.key!r}, {self.type}, default={self.default!r})"


def renderers():
    """Every Hydra renderer, as {id: display name}: {"HdStormRendererPlugin": "GL", ...}."""
    return dict(_app().renderers())


def renderer_settings_info(renderer):
    """The settings of a renderer (an id from ``renderers()``), as ``RendererSetting``s in its
    order. Their app-wide defaults are on Settings > Renderers; after this call
    ``usdcc.settings`` can read and change them by ``settings_key``. Raises ValueError for an
    unknown renderer."""
    result = []
    for key, label, type, default, settings_key in _app().renderer_settings_info(renderer):
        setting = RendererSetting()
        setting.key, setting.label, setting.type, setting.default, setting.settings_key = key, label, type, default, settings_key
        result.append(setting)
    return result


class Viewport:
    """An open viewport (``viewports()``, ``active_viewport()``). Once it's closed, every call
    but ``is_open()`` raises ValueError.

    Its renderer's own settings (docs/VIEWPORT_DISPLAY.md section 7): a viewport uses the
    app-wide defaults on Settings > Renderers, except for what it overrides, per renderer, as
    its Renderer Settings dialog does. Calls about settings take ``renderer=None``, the
    viewport's own renderer, or a renderer's id to work on one it isn't using (its overrides
    apply once it switches to it). A viewport has a renderer once it has drawn, or one has
    been set; until then, name one.
    """

    __slots__ = ("_id",)

    def __init__(self, id):
        self._id = id

    def __eq__(self, other):
        return isinstance(other, Viewport) and other._id == self._id

    def __hash__(self):
        return hash(self._id)

    def __repr__(self):
        if not self.is_open():
            return f"Viewport({self._id}, closed)"
        return f"Viewport({self._id}, {self.title()!r}, renderer={self.renderer()!r})"

    @property
    def id(self):
        """A number for this viewport, never given to another while the app runs."""
        return self._id

    def is_open(self):
        return _app().viewport_exists(self._id)

    def title(self):
        """Its panel's title ("Viewport")."""
        return _app().viewport_title(self._id)

    def stage(self):
        """The Usd.Stage it shows, or None."""
        from usdcc import usd

        return usd._stage(_app().viewport_stage(self._id))

    def renderer(self):
        """Its renderer's id ("HdStormRendererPlugin"), or "" before it has one."""
        return _app().viewport_renderer(self._id)

    def renderers(self):
        """The renderers it can switch to: once it has drawn, those that can render here;
        before, every one Hydra knows."""
        return list(_app().viewport_renderers(self._id))

    def set_renderer(self, renderer):
        """Switches it to ``renderer`` (an id), as its Renderer menu does. Before it has
        drawn, the renderer it starts with. Returns whether it uses it now; raises
        ValueError for an unknown renderer."""
        return _app().set_viewport_renderer(self._id, renderer)

    def renderer_settings(self, renderer=None):
        """What the renderer gets here, as {key: value}: the defaults with this viewport's
        overrides applied."""
        return dict(_app().viewport_renderer_settings(self._id, renderer or ""))

    def renderer_setting(self, key, renderer=None):
        """One of ``renderer_settings()``; KeyError if the renderer hasn't it."""
        values = self.renderer_settings(renderer)
        if key not in values:
            raise KeyError(f"{renderer or self.renderer()} has no setting {key!r}")
        return values[key]

    def renderer_setting_overrides(self, renderer=None):
        """Only what this viewport overrides, as {key: value}."""
        return dict(_app().viewport_renderer_overrides(self._id, renderer or ""))

    def set_renderer_setting(self, key, value, renderer=None):
        """Overrides one setting in this viewport, as its Renderer Settings dialog does. The
        value is a bool, int, float or str as the setting's type takes it (an int does for a
        float); KeyError for a setting the renderer hasn't, TypeError for a value that
        doesn't fit."""
        _app().set_viewport_renderer_setting(self._id, renderer or "", key, value)

    def reset_renderer_setting(self, key=None, renderer=None):
        """Drops this viewport's override of ``key``, so it follows Settings > Renderers again;
        with no key, all of the renderer's."""
        _app().reset_viewport_renderer_setting(self._id, renderer or "", key or "")

    def renderer_commands(self):
        """Its renderer's commands, as {name: description}; none before it has drawn."""
        return dict(_app().viewport_renderer_commands(self._id))

    def run_renderer_command(self, name):
        """Runs one of ``renderer_commands()`` with its arguments' defaults, as the Renderer
        menu does. Returns whether the renderer ran it."""
        return _app().run_viewport_renderer_command(self._id, name)


def viewports():
    """Every open viewport, oldest first, as ``Viewport``s."""
    return [Viewport(id) for id in _app().viewports()]


def active_viewport():
    """The viewport the user last worked in (the last one clicked, or else the oldest one
    open), as a ``Viewport``; None with no viewport open."""
    id = _app().active_viewport()
    return None if id is None else Viewport(id)


class Connection:
    """What ``connect()`` returns. Connections end by themselves when the app quits."""

    def __init__(self, id):
        self._id = id

    def disconnect(self):
        if self._id is not None:
            _app().disconnect_event(self._id)
            self._id = None


def connect(event, callback):
    """Calls ``callback`` when something happens in the app, until the returned
    ``Connection`` is disconnected. Exceptions are logged.

    - "selection_changed": ``callback(stage, paths)``, with Sdf.Paths
    - "current_stage_changed": ``callback(stage)``, with None when no stage is open
    - "stage_opened": ``callback(stage)``
    - "stage_closed": ``callback(identifier)``, its root layer's, as it's gone by then
    - "stage_edited": ``callback(stage, resynced_paths, changed_info_paths)`` after
      every change to any open stage, whoever made it (a panel, a tool, undo, a
      script), with the Sdf.Paths USD's change notice reports. It is called inside
      that notice: note what changed and act later (e.g. from a QTimer), don't edit
      or save the stage from it
    - "viewport_camera_changed": ``callback(camera)``, a ``ViewportCamera`` or None,
      when the active viewport's camera moves, it shows another stage, or another
      viewport becomes the active one (see ``active_viewport_camera()``)
    """
    from pxr import Sdf
    from usdcc import usd

    if event == "selection_changed":
        plain = lambda cache_id, paths: callback(usd._stage(cache_id), [Sdf.Path(path) for path in paths])
    elif event in ("current_stage_changed", "stage_opened"):
        plain = lambda cache_id: callback(usd._stage(cache_id))
    elif event == "stage_edited":
        plain = lambda cache_id, resynced, changed: callback(
            usd._stage(cache_id), [Sdf.Path(path) for path in resynced], [Sdf.Path(path) for path in changed]
        )
    elif event == "viewport_camera_changed":
        plain = lambda: callback(active_viewport_camera())
    else:
        plain = callback  # stage_closed; an unknown name raises below
    connection = Connection(_app().connect_event(event, plain))
    _plugins.track(connection.disconnect, callback)
    return connection


def reload_plugin(id):
    """Reloads a plugin, once what's running now has finished, so a plugin's own menu
    action can ask for it (docs/PLUGIN_DEV.md). The Plugins panel shows how it went."""
    _app().reload_plugin(id)


# --- Sessions (docs/SESSIONS.md) ----------------------------------------------


def sessions():
    """The saved sessions' names, newest first."""
    return list(_app().sessions())


def current_session():
    """The name of the session last loaded or saved, or None."""
    return _app().current_session() or None


def save_session(name):
    """Saves the app as it is now as the session ``name``, replacing one of that name.
    Unsaved edits to layers aren't in it."""
    _app().save_session(name)


def load_session(name, discard_unsaved=False):
    """Loads a session by name, or a session file by path, replacing the stages, panels
    and Script Editor tabs open now. Raises RuntimeError while layers have unsaved edits
    (unless ``discard_unsaved``), and when the session needs other plugins, which takes
    a restart: load it from File > Sessions for that."""
    _app().load_session(name, discard_unsaved)


def delete_session(name):
    """Moves the session's file to the recycle bin."""
    _app().delete_session(name)


def import_session(path, replace=False):
    """Copies a session file (one someone sent) into the sessions folder and returns
    its name. Raises if one of that name exists, unless ``replace``."""
    return _app().import_session(path, replace)


def export_session(name, path):
    """Writes a copy of the session's file to ``path``, to send to someone."""
    _app().export_session(name, path)


_session_savers = {}
_session_loaders = {}


def _session_key(callback, key):
    key = key or _plugins._owner_of(callback) or (_plugins.current().id if _plugins.current() else None)
    if not key:
        raise ValueError("give a key: the callback isn't from a plugin")
    return key


def on_session_save(callback, key=None):
    """Calls ``callback()`` when a session is saved; what it returns (anything that
    converts to JSON) is stored in the session under ``key``, by default the id of the
    plugin the callback is from."""
    key = _session_key(callback, key)
    _session_savers[key] = callback
    _plugins.track(lambda: _session_savers.pop(key, None), callback)


def on_session_load(callback, key=None):
    """Calls ``callback(data)`` when a session is loaded, with what ``on_session_save``'s
    callback returned for that key, or ``{}`` when the session has nothing for it."""
    key = _session_key(callback, key)
    _session_loaders[key] = callback
    _plugins.track(lambda: _session_loaders.pop(key, None), callback)


def _session_save_data():
    import json

    data = {}
    for key, callback in list(_session_savers.items()):
        try:
            data[key] = callback()
            json.dumps(data[key])
        except Exception as error:
            data.pop(key, None)
            from usdcc import log

            log.log_exception(type(error), error, error.__traceback__, "session", "session:" + key)
    return json.dumps(data)


def _session_load_data(encoded):
    import base64
    import json

    data = json.loads(base64.b64decode(encoded).decode("utf-8")) if encoded else {}
    for key, callback in list(_session_loaders.items()):
        try:
            callback(data.get(key, {}))
        except Exception as error:
            from usdcc import log

            log.log_exception(type(error), error, error.__traceback__, "session", "session:" + key)
    return True
