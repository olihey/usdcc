"""usdcc.tools: the viewport tools (docs/SCRIPTING.md section 4.1, docs/PLUGINS.md).

    import usdcc.tools
    usdcc.tools.set_active_tool("move")   # the tool rail follows

A tool made in Python subclasses ``Tool`` and is added with ``register_tool()``.
Needs the running app.
"""

from usdcc import _plugins

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    _native = None


def _app():
    if _native is None:
        raise RuntimeError("usdcc.tools needs the running usdcc app")
    return _native


def tool_names():
    """The tools' ids, in the tool rail's order: select, move, rotate, scale,
    then any others."""
    return _app().tool_names()


def active_tool():
    """The active tool's id."""
    return _app().active_tool()


def set_active_tool(name):
    """Activates the tool with the id `name` (see tool_names())."""
    _app().set_active_tool(name)


# --- Tools made in Python ----------------------------------------------------


class ToolResult:
    """What a mouse event can answer, instead of None (nothing happened).

    ``handled``: the viewport doesn't orbit or pan for this event. ``new_selection``:
    replaces the selection (paths). ``starts_edit``: from mouse_press(), a drag has
    begun that will edit the stage, so it becomes one undo step, ending at
    mouse_release(). A press saying this must not have edited anything itself yet.
    """

    def __init__(self, handled=True, new_selection=None, starts_edit=False):
        self.handled = handled
        self.new_selection = new_selection
        self.starts_edit = starts_edit


class Gizmo:
    """World-space lines and triangles for the viewport to draw on top."""

    def __init__(self):
        self.lines = []
        self.triangles = []

    def add_line(self, a, b, color=(1, 1, 1, 1)):
        self.lines.append((*a, *b, *_rgba(color)))

    def add_triangle(self, a, b, c, color=(1, 1, 1, 1)):
        self.triangles.append((*a, *b, *c, *_rgba(color)))


def _rgba(color):
    return tuple(color) if len(color) == 4 else (*color, 1.0)


class ToolContext:
    """The viewport's state for this one call: don't keep it.

    ``stage`` (a Usd.Stage), ``selection`` (Sdf.Paths), ``view_matrix``,
    ``projection_matrix`` (Gf.Matrix4d), ``camera_position`` (Gf.Vec3d) and
    ``target_op`` (the chosen transform op's name, or ''). ``pick(ndc)`` is the
    closest prim's Sdf.Path under normalized device coordinates, or an empty path.
    """

    __slots__ = ("stage", "selection", "view_matrix", "projection_matrix", "camera_position", "target_op", "_pick")

    def pick(self, ndc):
        from pxr import Sdf

        return Sdf.Path(self._pick(ndc[0], ndc[1]))


class Tool:
    """Base class for viewport tools made in Python.

    Subclass it and add it with ``register_tool()``. One instance is shared by every
    viewport. Like the built-in tools it holds only the state of a drag. Exceptions
    are logged, and the tool then does nothing until it's registered again.

    - ``options()`` lists the options bar's settings: ``toggle()``, ``number()``
      and ``choice()``, with their current values.
    - ``set_option(id, value)`` applies a new one.
    - ``build_gizmo(ctx)`` returns a ``Gizmo`` or None. Called on every frame, so
      keep it quick.
    - ``mouse_press(ctx, ndc, ray)`` and ``mouse_move(ctx, ndc, ray)`` get the
      position (Gf.Vec2d, -1 to 1) and a Gf.Ray, and return a ``ToolResult`` or None.
    - ``mouse_release()`` ends a drag.
    """

    def options(self):
        return []

    def set_option(self, id, value):
        pass

    def build_gizmo(self, ctx):
        return None

    def mouse_press(self, ctx, ndc, ray):
        return None

    def mouse_move(self, ctx, ndc, ray):
        return None

    def mouse_release(self):
        pass


def toggle(id, label, value):
    """An on/off option, for Tool.options()."""
    return {"type": "toggle", "id": id, "label": label, "value": bool(value)}


def number(id, label, value, min=0.0, max=1.0, step=0.1, decimals=2, suffix="", enabled_by=""):
    """A number option. `enabled_by` is a toggle's id that must be on for it."""
    return {"type": "number", "id": id, "label": label, "value": float(value), "min": float(min), "max": float(max),
            "step": float(step), "decimals": int(decimals), "suffix": suffix, "enabled_by": enabled_by}


def choice(id, label, value, choices):
    """One of `choices`, a list of (id, label) pairs. `value` is the chosen id."""
    return {"type": "choice", "id": id, "label": label, "value": value, "choices": list(choices)}


def register_tool(id, label, cls, icon=""):
    """Adds a tool to the tool rail: an instance of ``cls``, a ``Tool`` subclass,
    with the stable ``id`` and the tooltip ``label``.

    ``icon`` is the path of an image file; without one, the rail shows the label's
    first letter. Registering an id again replaces that tool. The built-in tools
    can't be replaced.
    """
    if not (isinstance(cls, type) and issubclass(cls, Tool)):
        raise TypeError("register_tool() needs a usdcc.tools.Tool subclass, not %r" % (cls,))
    _app().register_tool(id, label, cls(), str(icon))
    _plugins.note("Tool", label)


def unregister_tool(id):
    """Removes a tool made in Python. True if there was one."""
    return _app().unregister_tool(id)


# The C++ side of a Python tool (ScriptedTool) calls these with plain values.


def _context(cache_id, selection, view, projection, camera, target_op, pick):
    from pxr import Gf, Sdf
    from usdcc import usd

    ctx = ToolContext()
    ctx.stage = usd._stage(cache_id)
    ctx.selection = [Sdf.Path(path) for path in selection]
    ctx.view_matrix = Gf.Matrix4d(*view)
    ctx.projection_matrix = Gf.Matrix4d(*projection)
    ctx.camera_position = Gf.Vec3d(*camera)
    ctx.target_op = target_op
    ctx._pick = pick
    return ctx


def _gizmo(tool, context):
    gizmo = tool.build_gizmo(_context(*context))
    return (gizmo.lines, gizmo.triangles) if gizmo is not None else ([], [])


def _mouse(tool, method, context, ndc, origin, direction):
    from pxr import Gf

    result = getattr(tool, method)(_context(*context), Gf.Vec2d(*ndc), Gf.Ray(Gf.Vec3d(*origin), Gf.Vec3d(*direction)))
    if result is None:
        return (False, False, None)
    selection = None if result.new_selection is None else [str(path) for path in result.new_selection]
    return (bool(result.handled), bool(result.starts_edit), selection)
