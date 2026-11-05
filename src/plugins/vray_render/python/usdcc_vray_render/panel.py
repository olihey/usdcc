"""The V-Ray Render view panel (docs/VRAY_RENDER.md section 5): renders its stage
progressively in a V-Ray helper process, following the viewport camera and edits."""

import logging

import shiboken6
import usdcc.app
import usdcc.settings
import usdcc.toast
import usdcc.ui
from pxr import Gf
from PySide6 import QtCore, QtGui, QtWidgets

from . import client as clients
from . import config, scene

LOG = logging.getLogger("plugin.org.usdcc.vray-render")  # activate() replaces it with plugin.log

VIEWPORT = "viewport"
# Label, then a scale of the panel's size or a fixed (width, height).
RESOLUTIONS = [
    ("Fit Panel", 1.0),
    ("Fit Panel ½", 0.5),
    ("Fit Panel ¼", 0.25),
    ("1280 × 720", (1280, 720)),
    ("1920 × 1080", (1920, 1080)),
]
STATES = {"preparing": "Preparing", "rendering": "Rendering", "done": "Done", "paused": "Paused",
          "stopped": "Stopped", "error": "Error"}
# Camera moves come in as fast as the mouse; V-Ray restarts at most this often (ms).
CAMERA_INTERVAL = 30
# The longest side of an image, in pixels.
MAX_SIZE = 8192


class ImageView(QtWidgets.QWidget):
    """Shows the rendered image fit to the panel, or a message."""

    resized = QtCore.Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.image = None
        self.message = ""
        self.setMinimumSize(64, 64)
        self.setSizePolicy(QtWidgets.QSizePolicy.Policy.Expanding, QtWidgets.QSizePolicy.Policy.Expanding)

    def set_image(self, image):
        self.image = image
        self.update()

    def set_message(self, text):
        self.message = text
        self.update()

    def paintEvent(self, event):
        painter = QtGui.QPainter(self)
        painter.fillRect(self.rect(), QtGui.QColor(24, 24, 26))
        if self.image is not None and not self.image.isNull():
            size = self.image.size().scaled(self.size(), QtCore.Qt.AspectRatioMode.KeepAspectRatio)
            target = QtCore.QRect(QtCore.QPoint(0, 0), size)
            target.moveCenter(self.rect().center())
            painter.setRenderHint(QtGui.QPainter.RenderHint.SmoothPixmapTransform)
            painter.drawImage(target, self.image)
        if self.message:
            painter.setPen(QtGui.QColor(205, 205, 210))
            painter.drawText(self.rect().adjusted(24, 24, -24, -24),
                             QtCore.Qt.AlignmentFlag.AlignCenter | QtCore.Qt.TextFlag.TextWordWrap, self.message)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.resized.emit()


class VRayRenderPanel(usdcc.ui.ViewPanel):
    """Renders the panel's stage with V-Ray. Render starts a helper process,
    which stays until the panel closes; Stop only stops sampling."""

    def create_widget(self):
        self.client = None
        self.ready = False
        self.files = None
        self.camera = None  # the last scene.Camera sent
        self.size = None
        self.state = ""
        self.clock = QtCore.QElapsedTimer()

        root = QtWidgets.QWidget()
        self.root = root
        layout = QtWidgets.QVBoxLayout(root)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        bar = QtWidgets.QWidget(root)
        row = QtWidgets.QHBoxLayout(bar)
        row.setContentsMargins(6, 4, 6, 4)
        self.render_button = QtWidgets.QToolButton(text="Render", checkable=True, toolTip="Start or stop rendering")
        self.render_button.toggled.connect(self._render_toggled)
        self.refresh_button = QtWidgets.QToolButton(text="Refresh", toolTip="Send the stage to V-Ray again")
        self.refresh_button.clicked.connect(self._reload)
        self.camera_combo = QtWidgets.QComboBox(toolTip="What the image is seen through")
        self.camera_combo.addItem("Viewport Camera", VIEWPORT)
        self.camera_combo.currentIndexChanged.connect(self._camera_source_changed)
        self.resolution_combo = QtWidgets.QComboBox(toolTip="The image's size")
        for label, value in RESOLUTIONS:
            self.resolution_combo.addItem(label, value)
        self.resolution_combo.currentIndexChanged.connect(self._send_size)
        self.engine_combo = QtWidgets.QComboBox(toolTip="Render on the CPU, or on an NVIDIA GPU with CUDA or RTX")
        self.engine_combo.addItems(config.ENGINES)
        self.engine_combo.setCurrentText(config.get(config.ENGINE))
        self.engine_combo.currentTextChanged.connect(self._engine_changed)
        self.live_check = QtWidgets.QCheckBox("Update on Edits", checked=True,
                                              toolTip="Send the stage to V-Ray again after every edit")
        self.save_button = QtWidgets.QToolButton(text="Save Image…")
        self.save_button.clicked.connect(self._save_image)
        self.settings_button = QtWidgets.QToolButton(text="Settings…", toolTip="The V-Ray settings")
        self.settings_button.clicked.connect(lambda: usdcc.settings.show(config.CATEGORY))
        for widget in (self.render_button, self.refresh_button, self.camera_combo, self.resolution_combo,
                       self.engine_combo, self.live_check):
            row.addWidget(widget)
        row.addStretch(1)
        row.addWidget(self.save_button)
        row.addWidget(self.settings_button)

        self.view = ImageView(root)
        self.view.resized.connect(self._view_resized)
        self.status = QtWidgets.QLabel(root)
        self.status.setContentsMargins(8, 3, 8, 3)

        layout.addWidget(bar)
        layout.addWidget(self.view, 1)
        layout.addWidget(self.status)

        self.edit_timer = self._timer(config.get(config.EDIT_DELAY_MS), self._reload)
        self.size_timer = self._timer(150, self._send_size)
        self.camera_timer = self._timer(CAMERA_INTERVAL, self._send_camera)

        self.connections = [
            usdcc.app.connect("stage_edited", self._stage_edited),
            usdcc.app.connect("viewport_camera_changed", self._viewport_camera_changed),
        ]
        root.destroyed.connect(self._closed)
        problem = self._setup_problem()
        self.view.set_message(problem + "\n\nSet it in Settings… > V-Ray." if problem else
                              "Press Render to render the stage with V-Ray.")
        return root

    # The view panel's own calls.

    def stage_changed(self, stage):
        self._fill_cameras()
        if stage is None:
            self.render_button.setChecked(False)
            self.view.set_message("No stage to render.")
        elif self.ready:
            self._reload()

    def populate_options_menu(self, menu):
        menu.addAction("V-Ray Settings…", lambda: usdcc.settings.show(config.CATEGORY))

    # Starting and stopping.

    def _setup_problem(self):
        if config.FAKE_RENDERER:
            return None
        return clients.sdk_problem(config.get(config.SDK))

    def _render_toggled(self, on):
        self.render_button.setText("Stop" if on else "Render")
        if not on:
            if self.client is not None:
                self.client.send("stop")
            return
        problem = self._setup_problem() or (None if self.stage is not None else "No stage to render.")
        if problem:
            self.view.set_message(problem)
            self._set_status(problem)
            self.render_button.setChecked(False)
            return
        if self.client is None:
            self._start_helper()  # renders once it is ready
        elif self.ready:
            self.client.send("start")

    def _start_helper(self):
        python = config.get(config.PYTHON) or clients.default_python()
        sdk = config.get(config.SDK)
        self.client = clients.RenderClient(python, sdk, fake=config.FAKE_RENDERER, parent=self.root)
        self.client.ready.connect(self._helper_ready)
        self.client.image.connect(self._image)
        self.client.event.connect(self._event)
        self.client.output.connect(lambda line: LOG.info("%s", line))
        self.client.finished.connect(self._helper_finished)
        self.client.start()
        self.client.send("init", sdk=sdk, engine=self.engine_combo.currentText().lower(),
                         update_ms=config.get(config.UPDATE_MS), license_servers=config.get(config.LICENSE_SERVERS))
        self.view.set_message("Starting V-Ray…")
        self._set_status("Starting V-Ray…")

    def _helper_ready(self, fields):
        self.ready = True
        LOG.info("V-Ray %s is ready", fields.get("version", "?"))
        self._reload()
        if self.render_button.isChecked():
            self.client.send("start")

    def _helper_finished(self, why):
        self.client = None
        self.ready = False
        if not shiboken6.isValid(self.render_button):
            return
        self.render_button.setChecked(False)
        if why:
            LOG.error("%s", why)
            self.view.set_message(why + "\n\nThe Log panel has more.")
            self._set_status(why)
            usdcc.toast.show("V-Ray Render", why + " The Log panel has more.", kind="error", sticky=True)

    def _closed(self, *args):
        for connection in self.connections:
            connection.disconnect()
        self.connections = []
        if self.client is not None:
            self.client.shutdown()
            self.client = None
        if self.files is not None:
            self.files.remove()
            self.files = None

    # What V-Ray is given.

    def _reload(self):
        """Exports the stage as it is now and has V-Ray load it."""
        if not self.ready or self.stage is None:
            return
        if self.files is None:
            self.files = scene.SceneFiles()
        self.size = self._render_size()
        self.camera = self._current_camera()
        try:
            path = self.files.export(self.stage, self.camera, self.size)
        except Exception as error:  # shown; V-Ray keeps rendering the last scene
            LOG.exception("Exporting the stage for V-Ray failed")
            self._set_status("Exporting the stage failed: %s" % error)
            return
        self.client.send("size", width=self.size[0], height=self.size[1])
        self.client.send("camera", **self.camera.command_fields())
        self.client.send("load", path=path)
        self.clock.start()
        self._set_status("Loading the stage…")

    def _render_size(self):
        value = self.resolution_combo.currentData()
        if isinstance(value, float):
            ratio = self.view.devicePixelRatioF()
            width, height = self.view.width() * ratio * value, self.view.height() * ratio * value
        else:
            width, height = value
        # A squashed panel, mid-layout, can be very thin: keep the image a size V-Ray renders.
        shrink = min(1.0, MAX_SIZE / max(width, height, 1))
        return (max(16, int(width * shrink)), max(16, int(height * shrink)))

    def _aspect(self):
        width, height = self.size or self._render_size()
        return width / max(1, height)

    def _current_camera(self):
        source = self.camera_combo.currentData()
        aspect = self._aspect()
        if source != VIEWPORT:
            prim = self.stage.GetPrimAtPath(source) if self.stage is not None else None
            if prim:
                return scene.from_prim(prim, aspect)
        viewport = usdcc.app.active_viewport_camera()
        if viewport is not None and viewport.stage == self.stage:
            return scene.from_viewport(viewport, aspect)
        if self.camera is not None:
            return self.camera
        # No viewport on this stage: back from the origin, as a new viewport starts.
        return scene.Camera(Gf.Matrix4d(1.0).SetTranslate(Gf.Vec3d(0.0, 0.0, 10.0)), scene.horizontal_fov(45.0, aspect))

    def _send_camera(self):
        if not self.ready:
            return
        self.camera = self._current_camera()
        self.client.send("camera", **self.camera.command_fields())
        self.clock.start()

    def _send_size(self, *args):
        if not self.ready:
            return
        size = self._render_size()
        if size == self.size:
            return
        self.size = size
        self.client.send("size", width=size[0], height=size[1])
        self._send_camera()  # the field of view follows the aspect ratio

    # What happens in the app.

    def _stage_edited(self, stage, resynced, changed):
        # Inside USD's change notice: only note it, the timers act afterwards.
        if not shiboken6.isValid(self.render_button):
            return
        if stage != self.stage:
            return
        if resynced:
            QtCore.QTimer.singleShot(0, self._fill_cameras)
        if self.ready and self.live_check.isChecked():
            self.edit_timer.start()

    def _viewport_camera_changed(self, camera):
        if not shiboken6.isValid(self.render_button):
            return
        follows = self.ready and self.camera_combo.currentData() == VIEWPORT
        if follows and camera is not None and camera.stage == self.stage and not self.camera_timer.isActive():
            self.camera_timer.start()

    def _view_resized(self):
        if self.ready and isinstance(self.resolution_combo.currentData(), float):
            self.size_timer.start()

    def _camera_source_changed(self, *args):
        self._send_camera()

    def _engine_changed(self, text):
        if self.ready:
            self.client.send("engine", engine=text.lower())

    def _fill_cameras(self):
        if not shiboken6.isValid(self.camera_combo):
            return
        current = self.camera_combo.currentData()
        self.camera_combo.blockSignals(True)
        self.camera_combo.clear()
        self.camera_combo.addItem("Viewport Camera", VIEWPORT)
        for path in scene.camera_prims(self.stage):
            self.camera_combo.addItem(path.name, path.pathString)
        index = self.camera_combo.findData(current)
        self.camera_combo.setCurrentIndex(max(0, index))
        self.camera_combo.blockSignals(False)
        if index < 0 and current != VIEWPORT:
            self._send_camera()  # its camera went away

    # What comes back.

    def _image(self, image, change):
        self.view.set_message("")
        self.view.set_image(image)
        self._show_progress()

    def _event(self, name, fields):
        if name == "loaded":
            LOG.debug("V-Ray loaded %s in %.2f s", fields.get("path"), fields.get("seconds", 0.0))
        elif name == "state":
            self.state = fields.get("state", "")
            self._show_progress()
        elif name == "log":
            level = logging.ERROR if fields.get("level") == "error" else logging.WARNING
            LOG.log(level, "V-Ray: %s", fields.get("msg", ""))
        elif name == "error":
            message = fields.get("msg", "V-Ray failed")
            LOG.error("%s", message)
            self._set_status(message)
            if fields.get("fatal"):
                self.render_button.setChecked(False)
                usdcc.toast.show("V-Ray Render", message, kind="error", sticky=True)

    def _show_progress(self):
        parts = [STATES.get(self.state, "Rendering")]
        if self.clock.isValid():
            parts.append("%.1f s" % (self.clock.elapsed() / 1000.0))
        if self.view.image is not None:
            parts.append("%d × %d" % (self.view.image.width(), self.view.image.height()))
        self._set_status(" · ".join(parts))

    def _set_status(self, text):
        if shiboken6.isValid(self.status):
            self.status.setText(text)

    def _save_image(self):
        if self.view.image is None:
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(self.root, "Save Image", "render.png",
                                                        "PNG (*.png);;JPEG (*.jpg);;TIFF (*.tif)")
        if path and not self.view.image.save(path):
            usdcc.toast.show("V-Ray Render", "Could not save %s." % path, kind="error")

    def _timer(self, interval, slot):
        timer = QtCore.QTimer(self.root)
        timer.setSingleShot(True)
        timer.setInterval(int(interval))
        timer.timeout.connect(slot)
        return timer
