"""The V-Ray Render view panel (docs/VRAY_RENDER.md section 2): the app's render view
panel (usdcc.ui.RenderViewPanel, docs/RENDER_VIEW_PANEL.md) with V-Ray in a helper
process behind it. The app's panel does the toolbar, the image and following the
camera, the size and edits; this starts the helper and talks to it."""

import logging

import usdcc.ui
from PySide6 import QtGui

from . import client as clients
from . import config, scene

LOG = logging.getLogger("plugin.org.usdcc.vray-render")  # activate() replaces it with plugin.log

STATES = {"preparing": "Preparing", "rendering": "Rendering", "done": "Done", "paused": "Paused",
          "stopped": "Stopped", "error": "Error"}


class VRayRenderPanel(usdcc.ui.RenderViewPanel):
    """Renders the panel's stage with V-Ray. Render starts a helper process,
    which stays until the panel closes; Stop only stops sampling."""

    renderer_name = "V-Ray"
    settings_category = config.CATEGORY

    def __init__(self):
        self.client = None
        self.ready = False
        self.files = None

    # The panel's hooks.

    def setup_problem(self):
        if config.FAKE_RENDERER:
            return None
        return clients.sdk_problem(config.get(config.SDK))

    def create_toolbar(self, layout):
        # Nothing on the toolbar, which holds the app's icons: the engine is in the options menu.
        self.engine = config.get(config.ENGINE)
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
        """Exports the stage as it is now and has V-Ray load it."""
        if not self.ready or self.stage is None:
            return
        if self.files is None:
            self.files = scene.SceneFiles()
        size = self.render_size()
        camera = scene.Camera.from_render(self.camera())
        try:
            path = self.files.export(self.stage, camera, size)
        except Exception as error:  # shown; V-Ray keeps rendering the last scene
            LOG.exception("Exporting the stage for V-Ray failed")
            self.report_error("Exporting the stage failed: %s" % error)
            return
        self.client.send("size", width=size[0], height=size[1])
        self.client.send("camera", **camera.command_fields())
        self.client.send("load", path=path)
        self.set_state("Loading the stage…")

    def camera_changed(self, camera):
        if self.ready:
            self.client.send("camera", **scene.Camera.from_render(camera).command_fields())

    def size_changed(self, width, height):
        if self.ready:
            self.client.send("size", width=width, height=height)

    def populate_options_menu(self, menu):
        """Engine: CPU, CUDA or RTX, after the app's Camera, Resolution and Update on Edits."""
        engines = menu.addMenu("Engine")
        group = QtGui.QActionGroup(engines)
        for engine in config.ENGINES:
            action = engines.addAction(engine)
            action.setCheckable(True)
            action.setChecked(engine == self.engine)
            action.setToolTip("Render on the CPU, or on an NVIDIA GPU with CUDA or RTX")
            group.addAction(action)
            action.triggered.connect(lambda _=False, engine=engine: self._engine_changed(engine))

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
        python = config.get(config.PYTHON) or clients.default_python()
        sdk = config.get(config.SDK)
        # Without a parent: close() ends it, and the panel's widgets are the app's.
        self.client = clients.RenderClient(python, sdk, fake=config.FAKE_RENDERER)
        self.client.ready.connect(self._helper_ready)
        self.client.image.connect(self._image)
        self.client.event.connect(self._event)
        self.client.output.connect(lambda line: LOG.info("%s", line))
        self.client.finished.connect(self._helper_finished)
        self.client.start()
        self.client.send("init", sdk=sdk, engine=self.engine.lower(),
                         update_ms=config.get(config.UPDATE_MS), license_servers=config.get(config.LICENSE_SERVERS))
        self.show_message("Starting V-Ray…")
        self.set_state("Starting V-Ray…")

    def _helper_ready(self, fields):
        self.ready = True
        LOG.info("V-Ray %s is ready", fields.get("version", "?"))
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

    def _engine_changed(self, text):
        self.engine = text
        self.options_changed()
        if self.ready:
            self.client.send("engine", engine=text.lower())

    # What comes back.

    def _image(self, image, change):
        if self.is_open():
            self.show_image(image)

    def _event(self, name, fields):
        if not self.is_open():
            return
        if name == "loaded":
            LOG.debug("V-Ray loaded %s in %.2f s", fields.get("path"), fields.get("seconds", 0.0))
        elif name == "state":
            state = fields.get("state", "")
            self.set_state(STATES.get(state, "Rendering"))
        elif name == "log":
            level = logging.ERROR if fields.get("level") == "error" else logging.WARNING
            LOG.log(level, "V-Ray: %s", fields.get("msg", ""))
        elif name == "error":
            message = fields.get("msg", "V-Ray failed")
            LOG.error("%s", message)
            self.report_error(message, fatal=bool(fields.get("fatal")))
