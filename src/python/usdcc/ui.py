"""usdcc.ui: base classes for UI made in Python (docs/SCRIPTING.md section 8).

``SidePanel`` is the base class for side panels; dock one with
``usdcc.app.add_side_panel()``. Needs PySide6 built for usdcc
(tools/build-pyside.ps1); without it, importing this raises ImportError.
"""

# PySide6 first: importing it adds its folder (Qt, shiboken6 and pyside6
# DLLs) to the DLL search path that _ui.pyd needs.
import PySide6.QtWidgets  # noqa: F401
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
