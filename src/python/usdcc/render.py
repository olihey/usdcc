"""usdcc.render: final-frame renders (docs/FINAL_RENDER.md).

    import usdcc.render as render
    job = render.render_frame()                         # the current stage, the frame shown
    job = render.render_animation(stage, settings="/Render/Settings")
    for j in render.jobs():
        print(j.id, j.status, f"{j.progress:.0%}", j.files)

A render takes its settings from the stage's RenderSettings prim (the one named by the
stage's renderSettingsPrimPath metadata, else the first, else Settings > Final Render's
defaults), and is queued: the calls return at once, and the Render Queue shows it.

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
    ``frames``, ``files`` (written so far) and ``error``."""

    __slots__ = ("id", "title", "renderer", "status", "progress", "frames", "files", "error")

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
    """The Render Queue's jobs, oldest first."""
    result = []
    for id, title, renderer, status, progress, frames, files, error in _app().render_jobs():
        job = Job()
        job.id, job.title, job.renderer, job.status = id, title, renderer, status
        job.progress, job.frames, job.files, job.error = progress, list(frames), list(files), error
        result.append(job)
    return result


def job(id):
    """The job with ``id``, or None."""
    return next((j for j in jobs() if j.id == id), None)


def cancel(id):
    """Stops a queued or running job; frames already written stay."""
    _app().cancel_render(id)
