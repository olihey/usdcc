"""usdcc.ui: base classes for UI made in Python (docs/SCRIPTING.md section 8).

``SidePanel`` is the base class for side panels; dock one with
``usdcc.app.add_side_panel()``. ``ViewPanel`` is the base class for view
panels, the tabs in the centre that show one stage each; register one with
``usdcc.app.register_view_panel()``. Needs PySide6 built for usdcc
(tools/build-pyside.ps1); without it, importing this raises ImportError.
"""

# PySide6 first: importing it adds its folder (Qt, shiboken6 and pyside6
# DLLs) to the DLL search path that _ui.pyd needs.
import shiboken6 as _shiboken
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
