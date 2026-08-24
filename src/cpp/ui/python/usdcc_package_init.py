# usdcc namespace package.
#
# usdcc.ui's compiled classes land at usdcc.ui.usdcc.ui.{SidePanel,ViewPanel}
# rather than usdcc.ui.SidePanel: Shiboken6 needs the C++ namespace spelled
# out in the typesystem to resolve usdcc::ui::SidePanel at all (a bare
# <object-type name="SidePanel"/> doesn't match), which makes that namespace
# reappear as a nested Python object too. Marking it invisible
# (visible="no") to collapse that hits a Shiboken6 codegen bug (undeclared
# SBK_...IDX identifier in the generated namespace wrapper), so this
# re-exports the flat names by hand after import instead. See
# src/cpp/ui/python/typesystem.xml for the full explanation.
from . import ui as _ui

_ui.SidePanel = _ui.usdcc.ui.SidePanel
_ui.ViewPanel = _ui.usdcc.ui.ViewPanel

del _ui
