"""usdcc.app: the running usdcc application (docs/SCRIPTING.md section 4.1).

``is_embedded()`` tells whether this python runs inside usdcc. Inside it,
``stage_manager()`` and ``undo_manager()`` are the app's own objects; most
scripts use the friendlier usdcc.usd functions instead. With PySide6,
``main_window()`` is the window, ``add_side_panel()`` docks a side panel made
in Python, and ``register_view_panel()`` adds a view panel type
(docs/SCRIPTING.md section 8).
"""

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


def add_side_panel(panel, area="right"):
    """Docks ``panel``, a ``usdcc.ui.SidePanel``, at the window's ``area`` edge
    ("left", "right", "top" or "bottom") and lists it in the View menu.

    The window takes ownership of it. Its object name (the title it was made
    with) is how the saved layout knows it: a panel shown in an earlier session
    goes back where it was then, instead of to ``area``. Needs PySide6.
    """
    import shiboken6
    from usdcc import ui

    if not isinstance(panel, ui.SidePanel):
        raise TypeError("add_side_panel() needs a usdcc.ui.SidePanel, not %s" % type(panel).__name__)
    if area not in _AREAS:
        raise ValueError("no dock area named %r (%s)" % (area, ", ".join(_AREAS)))
    # Parenting it from Python is what hands it over: from here on PySide
    # leaves deleting it to the window.
    panel.setParent(main_window())
    _app().add_side_panel(shiboken6.getCppPointer(panel)[0], area)


def register_view_panel(id, name, cls):
    """Makes ``cls``, a ``usdcc.ui.ViewPanel`` subclass, a view panel type
    named ``name`` (as the Stages menu shows it) with the stable ``id``.

    Registering an id again replaces that type, so re-running the script that
    registers it picks up changes; panels already open keep their old class.
    The built-in types (``view_panel_types()``) can't be replaced. Needs PySide6.
    """
    from usdcc import ui

    if not (isinstance(cls, type) and issubclass(cls, ui.ViewPanel)):
        raise TypeError("register_view_panel() needs a usdcc.ui.ViewPanel subclass, not %r" % (cls,))
    _app().register_view_panel(id, name, cls)


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
