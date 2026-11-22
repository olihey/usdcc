"""usdcc.render: final-frame renders (docs/FINAL_RENDER.md).

    import usdcc.render as render
    job = render.render_frame()                         # the current stage, the frame shown
    job = render.render_animation(stage, settings="/Render/Settings")
    for j in render.jobs():
        print(j.id, j.status, f"{j.progress:.0%}", j.files)

A render takes its settings from the stage's RenderSettings prim (the one named by the
stage's renderSettingsPrimPath metadata, else the first, else Settings > Final Render's
defaults), and is queued: the calls return at once, and the Render Queue shows it.

A plugin adds a renderer of its own by subclassing ``FinalRenderer`` and calling
``register_renderer()`` (docs/FINAL_RENDER.md section 6.1):

    class MyRenderer(render.FinalRenderer):
        id = "com.example.my-renderer"
        name = "My Renderer"
        namespaces = ("my",)

        def settings_schema(self):
            return [render.setting("my:samples", "Samples", "int", 64, minimum=1, maximum=4096)]

        def start(self, task):
            scene = task.export_scene()      # the task's USD file, with its RenderSettings
            ...                              # render it; later:
            task.finished({"color": qimage})  # a float QImage per AOV

    render.register_renderer(MyRenderer())

Needs the running app.
"""

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    _native = None


def _app():
    if _native is None:
        raise RuntimeError("usdcc.render needs the running usdcc app")
    return _native


class Renderer:
    """A final renderer: ``id``, ``name``, ``problem`` (why it can't render here, or "") and
    ``tiles`` (whether it renders a tile on its own)."""

    __slots__ = ("id", "name", "problem", "tiles")

    def __repr__(self):
        return f"Renderer({self.id!r}, {self.name!r})"


class Job:
    """A render job, as it was when ``jobs()`` was called: ``id``, ``title``, ``renderer``,
    ``status`` ("queued", "running", "done", "failed" or "cancelled"), ``progress`` (0 to 1),
    ``frames``, ``files`` (written so far) and ``error``; ``queue``, the render queue server's
    name for a job sent to one (Render on: Queue), else "". A queue's job is "queued" until the
    queue starts it, "failed" when the queue refused it, and its ``files`` are those fetched
    here: none when the queue writes them to a shared path."""

    __slots__ = ("id", "title", "renderer", "status", "progress", "frames", "files", "error", "queue")

    def __repr__(self):
        return f"Job({self.id!r}, {self.status!r}, {self.progress:.0%})"


def renderers():
    """The final renderers there are: every Hydra delegate, and those plugins add."""
    result = []
    for id, name, problem, tiles in _app().final_renderers():
        renderer = Renderer()
        renderer.id, renderer.name, renderer.problem, renderer.tiles = id, name, problem, tiles
        result.append(renderer)
    return result


def _submit(stage, settings, animation, renderer):
    from usdcc import usd

    if stage is None:
        stage = usd.current_stage()
        if stage is None:
            raise RuntimeError("no stage is open to render")
    return _app().submit_render(usd.stage_id(stage), str(settings or ""), animation, renderer or "")


def render_frame(stage=None, settings=None, renderer=None):
    """Queues the frame ``stage`` (a ``Usd.Stage`` open in the app; the current one by default)
    shows, with its RenderSettings prim ``settings`` (a path; its default by default), and
    returns the job's id. ``renderer`` renders with another renderer than the prim's. Raises
    RuntimeError, saying why, when it can't (no camera, the renderer can't render here)."""
    return _submit(stage, settings, False, renderer)


def render_animation(stage=None, settings=None, renderer=None):
    """As ``render_frame()``, for the render settings' frame range (the stage's without one)."""
    return _submit(stage, settings, True, renderer)


def jobs():
    """The Render Queue's jobs, oldest first, then those sent to render queue servers."""
    result = []
    for id, title, renderer, status, progress, frames, files, error, queue in _app().render_jobs():
        job = Job()
        job.id, job.title, job.renderer, job.status = id, title, renderer, status
        job.progress, job.frames, job.files, job.error = progress, list(frames), list(files), error
        job.queue = queue
        result.append(job)
    return result


def job(id):
    """The job with ``id``, or None."""
    return next((j for j in jobs() if j.id == id), None)


def cancel(id):
    """Stops a queued or running job; frames already written stay. A job sent to a queue
    server is cancelled there, once the queue has queued it."""
    _app().cancel_render(id)


# --- Renderers from Python (docs/FINAL_RENDER.md section 6.1).


def setting(attribute, label, type="float", default=None, group="", help="", minimum=None, maximum=None,
            choices=None, key=None):
    """One entry of ``FinalRenderer.settings_schema()``: the attribute on the RenderSettings prim
    ("my:samples"), its label, its type ("bool", "int", "float", "string", "choice" or "color", an
    (r, g, b) tuple), its default, and what ``task.settings`` has it by (``key``, the attribute by
    default)."""
    return {"attribute": attribute, "label": label, "type": type, "default": default, "group": group,
            "help": help, "minimum": minimum, "maximum": maximum, "choices": choices, "key": key}


class Scene:
    """What ``Task.export_scene()`` wrote: ``file``, and the paths of its ``settings``,
    ``product`` and ``camera`` prims."""

    __slots__ = ("file", "settings", "product", "camera")

    def __repr__(self):
        return f"Scene({self.file!r})"


class Task:
    """One task: a frame of a job, or a tile of one.

    - ``frame``, ``resolution`` (width, height) and ``pixel_aspect``;
    - ``region``: (x, y, width, height) of the image to render, in pixels from the top left; the
      whole image unless the job has tiles;
    - ``camera`` and ``render_settings``: prim paths;
    - ``aovs``: (name, source name, USD type) of each RenderVar;
    - ``settings``: the renderer's settings that are authored, by key;
    - ``time_limit``: seconds, 0 for until it's done.

    Report with ``progress()``, then ``finished()`` or ``failed()``, once, on the UI thread.
    ``active`` is False once it has ended or was cancelled."""

    def __init__(self, native):
        self._native = native

    def __getattr__(self, name):
        return getattr(self._native, name)

    def export_scene(self):
        """Writes the task's scene, a .usdc in a folder that goes with the task: the job's stage
        with its RenderSettings prim, and the task's camera, resolution, tile (dataWindowNDC) and
        frame (the start and end time code) on it. Returns a ``Scene``."""
        scene = Scene()
        scene.file, scene.settings, scene.product, scene.camera = self._native.export_scene()
        return scene

    def progress(self, fraction, state=""):
        """How far it is, 0 to 1 (negative when it can't tell), and what it's doing."""
        self._native.progress(float(fraction), str(state))

    def finished(self, images):
        """Done: ``images`` maps each AOV's name to a float QImage (Format_RGBA32FPx4 or
        RGBA16FPx4, linear), of the region's size or of the whole image."""
        import shiboken6

        self._native.finished({name: shiboken6.getCppPointer(image)[0] for name, image in images.items()})

    def failed(self, error):
        self._native.failed(str(error))


class FinalRenderer:
    """A renderer for final renders. Subclass it, set ``id``, ``name`` and ``namespaces`` (where
    its settings are on a RenderSettings prim), and ``tiles`` if it can render a region, and
    implement ``start()``."""

    id = ""
    name = ""
    namespaces = ()
    tiles = False

    def setup_problem(self):
        """Why it can't render here, or None."""
        return None

    def settings_schema(self):
        """Its settings, as ``setting()`` makes them, for the Render Settings panel."""
        return []

    def start(self, task):
        """Renders ``task`` (a ``Task``), reporting on it as it goes. Return at once."""
        raise NotImplementedError

    def cancel(self, task):
        """Stops ``task``. Nothing it reports afterwards is used."""


class _Adapter:
    """What the app holds: forwards to the renderer with ``Task``s."""

    def __init__(self, renderer):
        self._renderer = renderer
        self._tasks = {}
        self.id = renderer.id
        self.name = renderer.name or renderer.id
        self.namespaces = tuple(renderer.namespaces)
        self.tiles = bool(renderer.tiles)

    def setup_problem(self):
        return self._renderer.setup_problem()

    def settings_schema(self):
        return list(self._renderer.settings_schema())

    def start(self, native):
        task = Task(native)
        self._tasks[id(native)] = task
        self._renderer.start(task)

    def cancel(self, native):
        task = self._tasks.pop(id(native), None)
        if task is not None:
            self._renderer.cancel(task)


def register_renderer(renderer):
    """Adds ``renderer`` (a ``FinalRenderer``) to the app's final renderers, replacing one with
    its id. Remove it in the plugin's ``deactivate()``."""
    if not isinstance(renderer, FinalRenderer):
        raise TypeError("register_renderer() needs a FinalRenderer, not %s" % type(renderer).__name__)
    return _app().register_final_renderer(_Adapter(renderer))


def unregister_renderer(id):
    """Removes the final renderer with ``id``."""
    _app().unregister_final_renderer(id)
