"""The RTX Render view panel (docs/RTX_RENDER.md section 2): the app's render view panel
(usdcc.ui.RenderViewPanel, docs/RENDER_VIEW_PANEL.md) with NVIDIA's ovrtx in a helper
process behind it. Camera moves, sizes and transform edits are written into the
helper's stage in place; anything else sends the stage again."""

import logging

import usdcc.ui
from PySide6 import QtWidgets

from . import client as clients
from . import config, scene

LOG = logging.getLogger("plugin.org.usdcc.rtx-render")  # activate() replaces it with plugin.log

STATES = {"starting": "Starting RTX…", "rendering": "Rendering", "done": "Done", "stopped": "Stopped"}


class RtxRenderPanel(usdcc.ui.RenderViewPanel):
    """Renders the panel's stage with ovrtx. Render starts a helper process, which
    stays until the panel is deleted; Stop only stops rendering frames."""

    renderer_name = "RTX"
    settings_category = config.CATEGORY

    def __init__(self):
        self.client = None
        self.ready = False
        self.files = None
        self.orthographic = False  # the loaded camera's projection

    # The panel's hooks.

    def setup_problem(self):
        if config.FAKE_RENDERER:
            return None
        return clients.setup_problem(config.get(config.PYTHON))

    def create_toolbar(self, layout):
        self.mode_combo = QtWidgets.QComboBox(toolTip="Real-time path tracing, path tracing, or minimal rasterizing")
        self.mode_combo.addItems(config.MODES)
        self.mode_combo.setCurrentText(config.get(config.MODE))
        self.mode_combo.currentTextChanged.connect(lambda _: self.reload())
        layout.addWidget(self.mode_combo)
        self.set_edit_delay(config.get(config.EDIT_DELAY_MS))

    def start(self):
        if self.client is None:
            self._start_helper()  # renders once it is ready
        elif self.ready:
            self.client.send("start")

    def stop(self):
        if self.client is not None:
            self.client.send("stop")

    def reload(self):
        """Exports the stage as it is now and has the helper load it."""
        if not self.ready or self.stage is None:
            return
        if self.files is None:
            self.files = scene.SceneFiles()
        size = self.render_size()
        camera = self.camera()
        try:
            path = self.files.export(self.stage, camera, size, self.mode_combo.currentText())
        except Exception as error:  # shown; the helper keeps rendering the last scene
            LOG.exception("Exporting the stage for RTX failed")
            self.report_error("Exporting the stage failed: %s" % error)
            return
        self.orthographic = camera.orthographic
        self.client.send("load", path=path, width=size[0], height=size[1], mode=self.mode_combo.currentText())
        self.set_state("Loading the stage…")

    def camera_changed(self, camera):
        if not self.ready:
            return
        if camera.orthographic != self.orthographic:
            self.reload()  # the projection is in the export
        else:
            self.client.send("camera", **scene.camera_fields(camera))

    def size_changed(self, width, height):
        if self.ready:
            self.client.send("size", width=width, height=height)

    def stage_edited(self, resynced, changed):
        if not self.ready or self.stage is None:
            return
        items = scene.in_place_edits(self.stage, resynced, changed)
        if items is None:
            self.reload()
            return
        if items:
            self.client.send("xforms", items=items)
        # A camera prim the panel looks through may be what moved.
        self.camera_changed(self.camera())

    def close(self):
        if self.client is not None:
            self.client.shutdown()
            self.client = None
        self.ready = False
        if self.files is not None:
            self.files.remove()
            self.files = None

    # The helper.

    def _start_helper(self):
        # Without a parent: close() ends it, and the panel's widgets are the app's.
        self.client = clients.RenderClient("" if config.FAKE_RENDERER else config.get(config.PYTHON),
                                           fake=config.FAKE_RENDERER)
        self.client.ready.connect(self._helper_ready)
        self.client.image.connect(self._image)
        self.client.event.connect(self._event)
        self.client.output.connect(lambda line: LOG.info("%s", line))
        self.client.finished.connect(self._helper_finished)
        self.client.start()
        self.client.send("init", max_frames=config.get(config.MAX_FRAMES), image_ms=config.get(config.IMAGE_MS))
        self.show_message("Starting RTX…\n\nThe first start compiles shaders, which takes a minute or two.")
        self.set_state("Starting RTX…")

    def _helper_ready(self, fields):
        self.ready = True
        LOG.info("ovrtx %s is ready (%.1f s)", fields.get("version", "?"), fields.get("seconds", 0.0))
        if not self.is_open():
            return
        if self.image_size() is None:
            self.show_message("Loading the stage…")
        self.reload()
        if self.is_rendering():
            self.client.send("start")

    def _helper_finished(self, why):
        self.client = None
        self.ready = False
        if not self.is_open():
            return
        self.set_rendering(False)
        if why:
            LOG.error("%s", why)
            self.report_error(why, fatal=True)

    # What comes back.

    def _slow(self, seconds, frame):
        """ovrtx has been working on one frame for `seconds`: say so, as nothing else
        changes meanwhile. A scene's first frame can take minutes when it has many
        complex (MDL) materials; the helper can't stop it before it's done."""
        which = "the first frame" if frame == 1 else "frame %d" % frame
        self.set_state("Working on %s, %d s" % (which, seconds))
        if self.image_size() is None:
            self.show_message("ovrtx has been working on %s for %d s.\n\nA scene with many complex materials can take "
                              "minutes for its first frame. The GPU is busy meanwhile." % (which, seconds))
        if seconds < 15 or seconds % 60 < 5:  # not every few seconds
            LOG.info("RTX: working on %s for %d s", which, seconds)

    def _image(self, image, frame):
        if self.is_open():
            self.show_image(image)

    def _event(self, name, fields):
        if not self.is_open():
            return
        if name == "loaded":
            LOG.info("RTX loaded the stage in %.2f s", fields.get("seconds", 0.0))
            if self.image_size() is None:
                self.show_message("Rendering the first frame…")
        elif name == "slow":
            self._slow(fields.get("seconds", 0), fields.get("frame", 1))
        elif name == "state":
            state = fields.get("state", "")
            self.set_state(STATES.get(state, state.capitalize()))
        elif name == "log":
            level = {"error": logging.ERROR, "warning": logging.WARNING}.get(fields.get("level"), logging.INFO)
            LOG.log(level, "RTX: %s", fields.get("msg", ""))
        elif name == "stale":
            self.reload()  # an edit couldn't be written in place
        elif name == "error":
            message = fields.get("msg", "RTX failed")
            LOG.error("%s", message)
            self.report_error(message, fatal=bool(fields.get("fatal")))
