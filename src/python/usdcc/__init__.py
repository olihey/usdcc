# usdcc package (docs/SCRIPTING.md section 4).
#
# Pure-Python modules (usd, app, core, tools, log) sit next to the compiled
# ones they use: inside the app, the embedded modules _usdcc_app and
# _usdcc_log; in a plain python, _usd.pyd. usdcc.ui is an optional compiled
# extension (it needs a PySide6/Shiboken6 build), so this file must tolerate
# it being absent rather than hard-failing `import usdcc.*` for an unrelated
# module.
try:
    from . import ui as _ui
except ImportError:
    pass
else:
    # usdcc.ui's compiled classes land at usdcc.ui.usdcc.ui.SidePanel rather
    # than usdcc.ui.SidePanel: Shiboken6 needs the C++ namespace spelled out
    # in the typesystem to resolve usdcc::ui::SidePanel at all (a bare
    # <object-type name="SidePanel"/> doesn't match), which makes that namespace
    # reappear as a nested Python object too. Marking it invisible
    # (visible="no") to collapse that hits a Shiboken6 codegen bug (undeclared
    # SBK_...IDX identifier in the generated namespace wrapper), so this
    # re-exports the flat name by hand after import instead. See
    # src/cpp/ui/python/typesystem.xml for the full explanation. (ViewPanel is
    # not bound to Python at all — see bindings.h.)
    _ui.SidePanel = _ui.usdcc.ui.SidePanel
    del _ui
