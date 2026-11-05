"""Runs V-Ray for usdcc's V-Ray Render panel, in a process of its own (docs/VRAY_RENDER.md).

    python -I -u vray_helper.py [--fake]

usdcc starts this with its own python.exe and a clean environment: V-Ray reads
USD through its own copy of OpenUSD, whose DLLs have the same names as usdcc's,
so the two can't share a process. It reads commands from stdin and writes
events to stdout (protocol.py); stdin closing means usdcc is gone, and it quits.

--fake renders a gradient instead of importing V-Ray: the tests use it, and it
needs no App SDK or license.
"""

import os
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

    def image(self, width, height, pixels, change):
        with self._image_ready:
            self._image = (width, height, pixels, change)
            self._image_ready.notify()

    def log(self, level, message):
        self.event("log", level=level, msg=message)

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
                width, height, pixels, change = self._image
                self._image = None
            self._write(protocol.event("image", pixels, w=width, h=height, change=change))


class VRayBackend:
    """Renders with the V-Ray App SDK, interactively: camera and size changes
    restart sampling without reloading the scene."""

    MODES = {"cpu": "interactive", "cuda": "interactiveCuda", "rtx": "interactiveOptix"}

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
        if self.renderer is not None:
            self.renderer.stop()

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
        pixels = bytes(image.toBitmapData(True, True, False))  # RGBA, top row first
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
            rows = [bytes((tint, (y * 255) // max(1, height - 1), (sample * 80) % 256, 255)) * width for y in range(height)]
            self.writer.image(width, height, b"".join(rows), change)
        self.writer.event("state", state="done")


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
