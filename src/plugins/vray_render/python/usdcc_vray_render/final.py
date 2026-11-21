"""V-Ray for final renders (docs/VRAY_RENDER.md section 10, docs/FINAL_RENDER.md section 3.2): a
usdcc.render.FinalRenderer that renders each task's scene file in the V-Ray helper, in
production mode.

- **The scene** is the app's export of the task (task.export_scene()): the job's stage with its
  own RenderSettings prim, which V-Ray reads prims through, and the task's camera, size, tile
  and frame on it. V-Ray shaders' list counts are filled in, as for the panel.
- **The settings** this offers are applied by the helper to V-Ray's own settings plugins
  (helper/vray_helper.py, final_settings()), not read by V-Ray from the file.
- **One helper** for the renderer, started with its first task and kept for the next ones:
  starting V-Ray takes a second or two. The plugin's deactivate() ends it.
"""

import logging

import usdcc.render as render
from pxr import Usd

from . import client as clients
from . import config, shaders

LOG = logging.getLogger("plugin.org.usdcc.vray-render")  # activate() replaces it with plugin.log

ID = "org.usdcc.vray-render"


class VRayFinalRenderer(render.FinalRenderer):
    id = ID
    name = "V-Ray"
    namespaces = ("vray",)
    tiles = True  # a render region

    def __init__(self):
        self._client = None
        self._ready = False
        self._task = None
        self._pending = None  # the "final" command waiting for the helper to be ready

    def setup_problem(self):
        if config.FAKE_RENDERER:
            return None
        return clients.sdk_problem(config.get(config.SDK))

    def settings_schema(self):
        return [
            render.setting("vray:engine", "Engine", "choice", config.get(config.ENGINE), choices=config.ENGINES,
                           help="CPU, or an NVIDIA GPU with CUDA or RTX"),
            render.setting("vray:noiseThreshold", "Noise threshold", "float", 0.01, group="Image sampler",
                           minimum=0.0, maximum=1.0,
                           help="The progressive sampler stops refining a pixel below this noise"),
            render.setting("vray:minSubdivs", "Min subdivs", "int", 1, group="Image sampler", minimum=1, maximum=1000),
            render.setting("vray:maxSubdivs", "Max subdivs", "int", 100, group="Image sampler", minimum=1,
                           maximum=10000),
            render.setting("vray:gi", "Global illumination", "bool", True, group="Lighting"),
        ]

    def start(self, task):
        self._task = task
        try:
            scene = task.export_scene()
            _prepare(scene.file)
        except Exception as error:  # noqa: BLE001 - the task fails with it
            LOG.exception("Exporting the stage for V-Ray failed")
            task.failed("Exporting the stage for V-Ray failed: %s" % error)
            self._task = None
            return
        width, height = task.resolution
        x, y, w, h = task.region
        settings = task.settings
        command = {
            "path": scene.file,
            "width": width,
            "height": height,
            "region": None if (x, y, w, h) == (0, 0, width, height) else [x, y, w, h],
            "engine": str(settings.get("vray:engine", config.get(config.ENGINE))).lower(),
            "settings": {key: value for key, value in settings.items() if key != "vray:engine"},
            "time_limit": task.time_limit,
        }
        if self._client is not None and self._client.running() and self._ready:
            self._client.send("final", **command)
            task.progress(-1, "Loading the stage")
        else:
            self._pending = command
            if self._client is None or not self._client.running():
                self._start_helper()
            task.progress(-1, "Starting V-Ray")

    def cancel(self, task):
        if task is not self._task:
            return
        self._task = None
        self._pending = None
        if self._client is not None:
            self._client.send("stop")

    def shutdown(self):
        """Ends the helper (the plugin's deactivate())."""
        self._task = None
        self._pending = None
        if self._client is not None:
            client, self._client = self._client, None
            client.shutdown()
        self._ready = False

    # The helper.

    def _start_helper(self):
        python = config.get(config.PYTHON) or clients.default_python()
        sdk = config.get(config.SDK)
        self._ready = False
        self._client = clients.RenderClient(python, sdk, fake=config.FAKE_RENDERER)
        self._client.ready.connect(self._helper_ready)
        self._client.result.connect(self._result)
        self._client.event.connect(self._event)
        self._client.output.connect(lambda line: LOG.info("%s", line))
        self._client.finished.connect(self._helper_finished)
        self._client.start()
        self._client.send("init", sdk=sdk, engine=config.get(config.ENGINE).lower(),
                          update_ms=config.get(config.UPDATE_MS), license_servers=config.get(config.LICENSE_SERVERS))

    def _helper_ready(self, fields):
        self._ready = True
        LOG.info("V-Ray %s is ready for final renders", fields.get("version", "?"))
        if self._pending is not None and self._task is not None:
            command, self._pending = self._pending, None
            self._client.send("final", **command)
            self._task.progress(-1, "Loading the stage")

    def _helper_finished(self, why):
        self._client = None
        self._ready = False
        task, self._task = self._task, None
        self._pending = None
        if task is not None and task.active:
            task.failed(why or "The V-Ray helper stopped")

    def _result(self, image):
        task, self._task = self._task, None
        if task is not None and task.active:
            task.finished({"color": image})

    def _event(self, name, fields):
        task = self._task
        if name == "progress":
            if task is not None and task.active:
                task.progress(fields.get("fraction", -1.0), fields.get("msg") or "Rendering")
        elif name == "log":
            level = logging.ERROR if fields.get("level") == "error" else logging.WARNING
            LOG.log(level, "V-Ray: %s", fields.get("msg", ""))
        elif name == "error":
            message = fields.get("msg", "V-Ray failed")
            LOG.error("%s", message)
            self._task = None
            if task is not None and task.active:
                task.failed(message)
            if fields.get("fatal"):
                self.shutdown()


def _prepare(path):
    """What V-Ray needs in the file beyond the app's export: V-Ray shaders' list counts."""
    stage = Usd.Stage.Open(path)
    if stage is None:
        raise RuntimeError("couldn't open %s" % path)
    shaders.fill_list_counts(stage)
    stage.GetRootLayer().Save()


RENDERER = None


def register():
    global RENDERER
    RENDERER = VRayFinalRenderer()
    render.register_renderer(RENDERER)


def unregister():
    global RENDERER
    if RENDERER is not None:
        RENDERER.shutdown()
        RENDERER = None
    try:
        render.unregister_renderer(ID)
    except RuntimeError:
        pass  # the app is going
