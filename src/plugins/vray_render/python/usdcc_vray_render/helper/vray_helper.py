"""Runs V-Ray for usdcc's V-Ray Render panel, in a process of its own (docs/VRAY_RENDER.md).

    python -I -u vray_helper.py [--fake]

usdcc starts this with its own python.exe and a clean environment: V-Ray reads
USD through its own copy of OpenUSD, whose DLLs have the same names as usdcc's,
so the two can't share a process. It reads commands from stdin and writes
events to stdout (protocol.py); stdin closing means usdcc is gone, and it quits.

--fake renders a gradient instead of importing V-Ray: the tests use it, and it
needs no App SDK or license.

Besides the panel's interactive rendering, "final" renders a file once, in production mode,
for usdcc's final renders (docs/FINAL_RENDER.md section 3.2): the file's own camera and render
settings, a region for a tile, and the settings usdcc applies (final_settings()). The image
comes back as one "result" event.
"""

import os
import struct
import sys
import threading
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder out

import protocol

# The RenderView the helper makes and moves (VRayBackend._view()).
CAMERA_NAME = "usdccCamera"


class Writer:
    """Sends events to usdcc, from any thread. Images are coalesced: only the
    newest one waiting is sent, so a slow reader never falls behind."""

    def __init__(self, stream):
        self._stream = stream
        self._lock = threading.Lock()
        self._image = None
        self._image_ready = threading.Condition()
        self._closed = False
        self._thread = threading.Thread(target=self._send_images, name="usdcc-vray-images", daemon=True)
        self._thread.start()

    def event(self, name, **fields):
        self._write(protocol.event(name, **fields))

    def image(self, width, height, pixels, change, format="rgba32f"):
        with self._image_ready:
            self._image = (width, height, pixels, change, format)
            self._image_ready.notify()

    def log(self, level, message):
        self.event("log", level=level, msg=message)

    def result(self, width, height, pixels, format="rgba32f"):
        """A final render's image: sent as it is, never coalesced."""
        self._write(protocol.event("result", pixels, w=width, h=height, format=format))

    def close(self):
        with self._image_ready:
            self._closed = True
            self._image_ready.notify()
        self._thread.join(2.0)

    def _write(self, data):
        with self._lock:
            try:
                self._stream.write(data)
                self._stream.flush()
            except OSError:
                pass  # usdcc is gone; stdin ends next and the helper quits

    def _send_images(self):
        while True:
            with self._image_ready:
                while self._image is None and not self._closed:
                    self._image_ready.wait()
                if self._closed:
                    return
                width, height, pixels, change, format = self._image
                self._image = None
            self._write(protocol.event("image", pixels, w=width, h=height, change=change, format=format))


class VRayBackend:
    """Renders with the V-Ray App SDK, interactively: camera and size changes
    restart sampling without reloading the scene."""

    MODES = {"cpu": "interactive", "cuda": "interactiveCuda", "rtx": "interactiveOptix"}
    PRODUCTION = {"cpu": "production", "cuda": "productionCuda", "rtx": "productionOptix"}

    def __init__(self, writer):
        self.writer = writer
        self.vray = None
        self.renderer = None
        self.view = None  # the RenderView _view() made
        self.loaded = False
        self.rendering = False  # asked to render
        self.camera = None
        self.size = None
        self.mode = "interactive"
        self.final = None  # the final render running: (width, height), or None
        self._final_serial = 0

    def cmd_init(self, sdk, engine="cpu", update_ms=200, license_servers=""):
        sys.path.insert(0, os.path.join(sdk, "python"))
        import vray

        vray.setSDKLibraryPath(os.path.join(sdk, "bin"))
        self.vray = vray
        if license_servers:
            vray.setLicenseServers(license_servers)
        # No frame buffer window of V-Ray's own: the image goes to the panel.
        renderer = vray.VRayRenderer(enableFrameBuffer=False, showFrameBuffer=False, useVfbLog=False)
        renderer.setOnLogMessage(self._on_log)
        renderer.setOnLicenseError(self._on_license_error)
        renderer.setOnStateChanged(self._on_state)
        renderer.setOnProgressiveImageUpdated(self._on_image)
        renderer.progressiveImageUpdatedHasBuffer = True
        renderer.setProgressiveImageUpdateTimeout(int(update_ms))
        renderer.keepInteractiveRunning = True
        self.renderer = renderer
        self.mode = self.MODES.get(engine, "interactive")
        self.writer.event("ready", version=vray.getVRayVersion(), api=vray.getAPIVersion())

    def cmd_load(self, path):
        renderer = self.renderer
        started = time.perf_counter()
        if self.loaded:
            renderer.stop()
            renderer.clearScene()
        renderer.load(path)
        self.view = None
        self.loaded = True
        self._apply_camera()
        self._apply_size()
        self.writer.event("loaded", path=path, seconds=round(time.perf_counter() - started, 3))
        if self.rendering:
            self._start()

    def cmd_camera(self, transform, fov, ortho=False, ortho_width=1.0):
        self.camera = (transform, fov, ortho, ortho_width)
        if self.loaded:
            self._apply_camera()

    def cmd_size(self, width, height):
        self.size = (int(width), int(height))
        if self.loaded:
            self._apply_size()

    def cmd_engine(self, engine):
        self.mode = self.MODES.get(engine, "interactive")
        if self.loaded and self.rendering:
            self.renderer.stop()
            self._start()

    def cmd_start(self):
        self.rendering = True
        if self.loaded:
            self._start()

    def cmd_stop(self):
        self.rendering = False
        self.final = None
        self._final_serial += 1
        if self.renderer is not None:
            self.renderer.stop()

    def cmd_final(self, path, width, height, region=None, engine="cpu", settings=None, time_limit=0.0):
        """Renders `path` once, in production mode, through the file's own camera and render
        settings, and sends the image as a "result" event (the whole frame; a region, when
        given, is all V-Ray renders of it)."""
        renderer = self.renderer
        self.rendering = False
        if self.loaded:
            renderer.stop()
            renderer.clearScene()
        renderer.load(path)
        self.loaded = True
        self.view = None
        renderer.size = (int(width), int(height))
        if region:
            _try(self.writer, "render region", lambda: renderer.setRenderRegion(*[int(v) for v in region]))
        final_settings(self.writer, renderer, settings or {}, time_limit)
        renderer.keepInteractiveRunning = False
        renderer.renderMode = self.PRODUCTION.get(engine, "production")
        self._final_serial += 1
        serial = self._final_serial
        self.final = (int(width), int(height))
        _try(self.writer, "progress reports", lambda: renderer.setOnProgress(self._on_progress))
        renderer.startSync()
        threading.Thread(target=self._finish_final, args=(serial,), name="usdcc-vray-final", daemon=True).start()

    def _finish_final(self, serial):
        renderer = self.renderer
        renderer.waitForRenderEnd()
        if serial != self._final_serial or self.final is None or self.renderer is None:
            return  # stopped, or another one started
        self.final = None
        image = renderer.getImage()
        if image is None:
            self.writer.event("error", msg="V-Ray gave no image", fatal=False)
            return
        pixels = bytes(memoryview(image.toIntList("rgba32")))
        self.writer.result(image.width, image.height, pixels)

    def _on_progress(self, renderer, message, progress, total, instant):
        if self.final is not None and total:
            self.writer.event("progress", fraction=float(progress) / float(total), msg=str(message).strip())

    def close(self):
        if self.renderer is not None:
            self.renderer.stop()
            self.renderer.close()
            self.renderer = None

    def _start(self):
        self.renderer.renderMode = self.mode
        self.renderer.startSync()

    def _view(self):
        """The RenderView the image is seen through: one of our own, made the
        current camera by name. The views V-Ray makes from the USD file's render
        camera ignore changes while interactive rendering runs."""
        if self.view is None:  # made again after each load: clearScene() removes it
            self.view = self.renderer.classes.RenderView(CAMERA_NAME)
            self.view.scene_name = [CAMERA_NAME]
            self.renderer.cameraName = CAMERA_NAME
        return self.view

    def _apply_camera(self):
        if self.camera is None:
            return
        transform, fov, ortho, ortho_width = self.camera
        vray = self.vray
        view = self._view()
        view.transform = vray.Transform(
            vray.Matrix(vray.Vector(*transform[0:3]), vray.Vector(*transform[3:6]), vray.Vector(*transform[6:9])),
            vray.Vector(*transform[9:12]),
        )
        view.fov = float(fov)
        view.orthographic = bool(ortho)
        view.orthographicWidth = float(ortho_width)

    def _apply_size(self):
        if self.size is not None:
            self.renderer.size = self.size

    def _on_log(self, renderer, message, level, instant):
        if level <= self.vray.LOGLEVEL_WARNING:
            self.writer.log("error" if level == self.vray.LOGLEVEL_ERROR else "warning", message.strip())

    def _on_license_error(self, renderer, error, instant):
        self.writer.event("error", msg="V-Ray license: %s" % error, fatal=True)

    def _on_state(self, renderer, old, new, instant):
        names = {
            self.vray.RENDERER_STATE_PREPARING: "preparing",
            self.vray.RENDERER_STATE_RENDERING: "rendering",
            self.vray.RENDERER_STATE_RENDERING_AWAITING_CHANGES: "done",
            self.vray.RENDERER_STATE_RENDERING_PAUSED: "paused",
            self.vray.RENDERER_STATE_IDLE_DONE: "done",
            self.vray.RENDERER_STATE_IDLE_STOPPED: "stopped",
            self.vray.RENDERER_STATE_IDLE_ERROR: "error",
            self.vray.RENDERER_STATE_RENDERING_FATAL_ERROR: "error",
        }
        if new in names:
            self.writer.event("state", state=names[new])

    def _on_image(self, renderer, image, change, render_pass, instant):
        if render_pass != self.vray.RENDER_PASS_FINAL:
            return  # light cache passes are a preview of the lighting, not the image
        # Linear float RGBA, top row first, with no sRGB or gamma: usdcc applies the
        # view transform (docs/COLOR_MANAGEMENT.md section 6).
        pixels = bytes(memoryview(image.toIntList("rgba32")))
        self.writer.image(image.width, image.height, pixels, change)


class FakeBackend:
    """Stands in for V-Ray: answers like VRayBackend does and renders a gradient
    in a few passes, tinted by the camera, so tests can follow it."""

    def __init__(self, writer):
        self.writer = writer
        self.camera = None
        self.size = (64, 36)
        self.path = None
        self.rendering = False
        self.change = 0
        self._stop = threading.Event()
        self._thread = None

    def cmd_init(self, sdk, engine="cpu", update_ms=200, license_servers=""):
        self.writer.event("ready", version="fake", api=0)

    def cmd_final(self, path, width, height, region=None, engine="cpu", settings=None, time_limit=0.0):
        """The whole frame: R = x / width, G = y / height (top row 0), B = the setting
        vray:noiseThreshold (0.5 without), A = 1; outside a region, 0. The tests check where tiles go."""
        self._halt()
        self.path = path
        self.writer.event("loaded", path=path, seconds=0.0)
        width, height = int(width), int(height)
        x0, y0, w, h = region or (0, 0, width, height)
        blue = float((settings or {}).get("vray:noiseThreshold", 0.5))
        self.writer.event("progress", fraction=0.5, msg="Rendering")
        rows = []
        empty = struct.pack("<4f", 0.0, 0.0, 0.0, 0.0)
        for y in range(height):
            inside_row = y0 <= y < y0 + h
            rows.append(b"".join(
                struct.pack("<4f", x / width, y / height, blue, 1.0) if inside_row and x0 <= x < x0 + w else empty
                for x in range(width)))
        self.writer.result(width, height, b"".join(rows))

    def cmd_load(self, path):
        self.path = path
        self.writer.event("loaded", path=path, seconds=0.0)
        self._restart()

    def cmd_camera(self, transform, fov, ortho=False, ortho_width=1.0):
        self.camera = (transform, fov)
        self._restart()

    def cmd_size(self, width, height):
        self.size = (int(width), int(height))
        self._restart()

    def cmd_engine(self, engine):
        self._restart()

    def cmd_start(self):
        self.rendering = True
        self._restart()

    def cmd_stop(self):
        self.rendering = False
        self._halt()
        self.writer.event("state", state="stopped")

    def close(self):
        self._halt()

    def _halt(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join()
            self._thread = None

    def _restart(self):
        self._halt()
        self.change += 1
        if not self.rendering or self.path is None:
            return
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._render, args=(self._stop, self.change), daemon=True)
        self._thread.start()

    def _render(self, stop, change):
        self.writer.event("state", state="rendering")
        width, height = self.size
        tint = int(abs(self.camera[0][9]) * 40) % 256 if self.camera else 128
        for sample in range(1, 4):
            if stop.wait(0.03):
                return
            rows = [
                struct.pack("<4f", tint / 255.0, y / max(1, height - 1), ((sample * 80) % 256) / 255.0, 1.0) * width
                for y in range(height)
            ]
            self.writer.image(width, height, b"".join(rows), change)
        self.writer.event("state", state="done")


def _try(writer, what, call):
    """Calls `call`, and reports a V-Ray App SDK that hasn't got it as a warning."""
    try:
        call()
    except Exception as error:  # noqa: BLE001 - an older SDK; the render goes on without it
        writer.log("warning", "V-Ray couldn't set the %s: %s" % (what, error))


def final_settings(writer, renderer, settings, time_limit):
    """The settings usdcc's Render Settings panel offers for V-Ray (final.py's schema), on
    V-Ray's own settings plugins: the progressive image sampler's noise threshold, maximum
    and minimum subdivisions and time limit, and global illumination on or off."""
    classes = renderer.classes

    def sampler():
        plugin = classes.SettingsImageSampler.getInstanceOrCreate()
        plugin.type = 3  # progressive
        if "vray:noiseThreshold" in settings:
            plugin.progressive_threshold = float(settings["vray:noiseThreshold"])
        if "vray:maxSubdivs" in settings:
            plugin.progressive_maxSubdivs = int(settings["vray:maxSubdivs"])
        if "vray:minSubdivs" in settings:
            plugin.progressive_minSubdivs = int(settings["vray:minSubdivs"])
        if time_limit:
            plugin.progressive_maxTime = float(time_limit) / 60.0  # minutes

    _try(writer, "image sampler", sampler)
    if "vray:gi" in settings:
        _try(writer, "global illumination",
             lambda: setattr(classes.SettingsGI.getInstanceOrCreate(), "on", bool(settings["vray:gi"])))


def main():
    # Events go to a private copy of stdout, and stdout itself to stderr: what
    # V-Ray's native code prints can then never break into the event stream.
    events = os.fdopen(os.dup(1), "wb", buffering=0)
    os.dup2(2, 1)
    sys.stdout = sys.stderr

    writer = Writer(events)
    backend = FakeBackend(writer) if "--fake" in sys.argv[1:] else VRayBackend(writer)
    try:
        for line in sys.stdin.buffer:  # ends when usdcc closes the pipe or goes away
            if not line.strip():
                continue
            fields = protocol.json.loads(line)
            name = fields.pop("cmd")
            if name == "quit":
                break
            try:
                getattr(backend, "cmd_" + name)(**fields)
            except Exception as error:  # noqa: BLE001 - reported to usdcc, which decides
                traceback.print_exc()
                writer.event("error", msg="%s: %s" % (name, error), fatal=name == "init")
    finally:
        try:
            backend.close()
        finally:
            writer.close()


if __name__ == "__main__":
    main()
