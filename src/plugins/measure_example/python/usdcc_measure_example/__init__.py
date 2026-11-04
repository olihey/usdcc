"""An example plugin (docs/PLUGINS.md): each thing a Python plugin can add.

Run the app with this plugin's parent folder to try it:

    usdcc --plugin-path src/plugins
"""

import shiboken6
from pxr import Gf
from PySide6 import QtWidgets

import usdcc.app
import usdcc.tools
import usdcc.ui
import usdcc.usd

NOTE_KEY = "usdccNote"


class MeasureTool(usdcc.tools.Tool):
    """Drag in the viewport to measure a distance on the ground plane. It edits
    nothing, so it doesn't start an undo step."""

    def __init__(self):
        self.start = None
        self.end = None
        self.snap = False

    def options(self):
        return [usdcc.tools.toggle("snap", "Snap to Whole Units", self.snap)]

    def set_option(self, id, value):
        if id == "snap":
            self.snap = value

    def _on_ground(self, ray):
        hit, distance = ray.Intersect(Gf.Plane(Gf.Vec3d(0, 1, 0), 0.0))[:2]
        if not hit:
            return None
        point = ray.GetPoint(distance)
        return Gf.Vec3d(*(round(value) for value in point)) if self.snap else point

    def mouse_press(self, ctx, ndc, ray):
        self.start = self.end = self._on_ground(ray)
        return usdcc.tools.ToolResult(handled=self.start is not None)

    def mouse_move(self, ctx, ndc, ray):
        if self.start is None:
            return None
        self.end = self._on_ground(ray) or self.end
        return usdcc.tools.ToolResult()

    def mouse_release(self):
        if self.start is not None and self.end is not None:
            print("Measured %.3f" % (self.end - self.start).GetLength())
        self.start = self.end = None

    def build_gizmo(self, ctx):
        if self.start is None or self.end is None:
            return None
        gizmo = usdcc.tools.Gizmo()
        gizmo.add_line(self.start, self.end, (1.0, 0.8, 0.2))
        return gizmo


class NotesPanel(usdcc.ui.ViewPanel):
    """Shows the note on the selected prim, following the selection."""

    def create_widget(self):
        self.label = QtWidgets.QLabel("Select a prim.")
        self.label.setWordWrap(True)
        self.connection = usdcc.app.connect("selection_changed", self._selection_changed)
        return self.label

    def _selection_changed(self, stage, paths):
        if not shiboken6.isValid(self.label):  # the panel was closed
            self.connection.disconnect()
            return
        if stage != self.stage:
            return
        prim = stage.GetPrimAtPath(paths[0]) if paths else None
        note = prim.GetCustomDataByKey(NOTE_KEY) if prim else None
        self.label.setText(note or "No note on this prim.")


def add_note():
    stage = usdcc.usd.current_stage()
    if stage is None:
        return
    count = PLUGIN.settings.value("notes_added", 0) + 1
    PLUGIN.settings.set_value("notes_added", count)
    for path in usdcc.usd.selected_paths(stage):
        stage.GetPrimAtPath(path).SetCustomDataByKey(NOTE_KEY, "Note %d" % count)


def clear_note(stage, paths, clicked):
    for path in paths:
        stage.GetPrimAtPath(path).ClearCustomDataByKey(NOTE_KEY)


def has_note(stage, paths, clicked):
    return stage.GetPrimAtPath(clicked).HasCustomDataKey(NOTE_KEY)


PLUGIN = None


def activate(plugin):
    global PLUGIN
    PLUGIN = plugin
    usdcc.tools.register_tool(plugin.id + ".measure", "Measure", MeasureTool)
    usdcc.app.register_view_panel(plugin.id + ".notes", "Notes", NotesPanel)
    usdcc.app.add_menu_action("Tools/Add Note to Selection", add_note)
    usdcc.app.add_prim_menu_action("Clear Note", clear_note, applies=has_note, id=plugin.id + ".clear")
    plugin.log.info("Ready")
