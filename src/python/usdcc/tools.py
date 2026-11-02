"""usdcc.tools: the viewport tools (docs/SCRIPTING.md section 4.1).

    import usdcc.tools
    usdcc.tools.set_active_tool("move")   # the tool rail follows

Needs the running app.
"""

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    _native = None


def _app():
    if _native is None:
        raise RuntimeError("usdcc.tools needs the running usdcc app")
    return _native


def tool_names():
    """The tools, in the tool rail's order: select, move, rotate, scale."""
    return _app().tool_names()


def active_tool():
    """The active tool's name."""
    return _app().active_tool()


def set_active_tool(name):
    """Activates the tool called `name` (see tool_names())."""
    _app().set_active_tool(name)
