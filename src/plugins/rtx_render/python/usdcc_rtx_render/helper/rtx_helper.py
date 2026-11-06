"""Runs NVIDIA's ovrtx for usdcc's RTX Render panel, in a process of its own
(docs/RTX_RENDER.md section 4).

    python -I -u rtx_helper.py [--packages folder;folder...] [--fake]

usdcc starts this with its own python.exe, and --packages names the folders the app
installed ovrtx, ovstage and numpy into (its shared packages folder). ovrtx brings its
own OpenUSD, which can't share a process with usdcc's. It reads commands from stdin
and writes events to stdout (protocol.py); stdin closing means usdcc is gone, and it
quits.

ovrtx renders a frame per step(), on this process's main thread. After a change
(the stage, the camera, the size, an edit) the helper steps until it has rendered
`max_frames` frames, sending an image every `image_ms`, then says "done" and waits.

--fake renders a gradient instead of importing ovrtx: the tests use it, and it
needs no GPU.
"""

import os
import queue
import sys
import threading
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder out

import protocol


def use_packages(argv):
    """Puts the --packages folders on sys.path, and takes usdcc's own pxr off it:
    usdcc's python.exe has usdcc's OpenUSD in its site-packages, and ovrtx's must be
    the only one here."""
    if "--packages" not in argv:
        return
    folders = [f for f in argv[argv.index("--packages") + 1].split(os.pathsep) if f]
    sys.path[:] = [p for p in sys.path if not os.path.isdir(os.path.join(p, "pxr"))]
    sys.path[1:1] = folders  # after the helper's own folder

# The prims the export adds (scene.py): the camera and the render product.
CAMERA = "/__usdccRtx/Camera"
PRODUCT = "/__usdccRtx/Product"
LDR_COLOR = PRODUCT + "/LdrColor"


class Writer:
    """Sends events to usdcc. Images are coalesced on a thread of their own: only
    the newest one waiting is sent, so a slow reader never holds up rendering."""

    def __init__(self, stream):
        self._stream = stream
        self._lock = threading.Lock()
        self._image = None
        self._image_ready = threading.Condition()
        self._closed = False
        self._thread = threading.Thread(target=self._send_images, name="usdcc-rtx-images", daemon=True)
        self._thread.start()

    def event(self, name, **fields):
        self._write(protocol.event(name, **fields))

    def image(self, width, height, pixels, frame):
        with self._image_ready:
            self._image = (width, height, pixels, frame)
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
                width, height, pixels, frame = self._image
                self._image = None
            self._write(protocol.event("image", pixels, w=width, h=height, frame=frame))


class Backend:
    """What the RTX and the fake renderers share: when to render and send."""

    def __init__(self, writer):
        self.writer = writer
        self.loaded = False
        self.rendering = False  # asked to render
        self.size = (64, 36)
        self.frames = 0  # since the last change
        self.max_frames = 128
        self.image_seconds = 0.033
        self._sent = 0.0
        self._state = ""

    def wants_frames(self):
        return self.rendering and self.loaded and self.frames < self.max_frames

    def changed(self):
        self.frames = 0
        if self.rendering and self.loaded:
            self._set_state("rendering")

    def cmd_start(self):
        self.rendering = True
        self.changed()

    def cmd_stop(self):
        self.rendering = False
        self._set_state("stopped")

    def render_frame(self):
        pixels = self.step()
        self.frames += 1
        now = time.perf_counter()
        last = self.frames >= self.max_frames
        if self.frames == 1 or last or now - self._sent >= self.image_seconds:
            self._sent = now
            self.writer.image(pixels.shape[1], pixels.shape[0], pixels.tobytes(), self.frames)
        if last:
            self._set_state("done")

    def _set_state(self, state):
        if state != self._state:
            self._state = state
            self.writer.event("state", state=state)


class RtxBackend(Backend):
    """Renders with ovrtx. Camera, size and transform changes are written into the
    loaded stage in place; only a load reads the file again."""

    def __init__(self, writer):
        super().__init__(writer)
        self.np = None
        self.ovrtx = None
        self.ovstage = None
        self.renderer = None
        self.stage = None
        self.paths = None
        self.ordinal = 0
        self._queries = {}  # paths: (path list, query)
        self._tokens = {}

    def cmd_init(self, max_frames=128, image_ms=33):
        self.max_frames = max(1, int(max_frames))
        self.image_seconds = max(0.0, image_ms / 1000.0)
        try:
            import numpy
            import ovrtx
            import ovstage
        except ImportError as error:
            raise RuntimeError("ovrtx can't be imported in %s (%s). Install the RTX Render plugin's packages "
                               "from View > Plugins." % (sys.executable, error)) from error
        self.np, self.ovrtx, self.ovstage = numpy, ovrtx, ovstage
        self._set_state("starting")
        self.writer.log("info", "Starting ovrtx %s; the first start compiles shaders, which takes a minute or two."
                        % getattr(ovrtx, "__version__", ""))
        started = time.perf_counter()
        self.renderer = ovrtx.Renderer()
        self.stage = ovstage.Stage("usdcc.rtx")
        self.renderer.attach_ovstage(self.stage)
        version = ".".join(str(v) for v in self.renderer.version)
        self.writer.event("ready", version=version, seconds=round(time.perf_counter() - started, 2))

    def cmd_load(self, path, width, height):
        started = time.perf_counter()
        self._forget_queries()
        self.ordinal += 1
        self.ovstage.population.open_usd(self.stage, path, ordinal=self.ordinal)
        self._commit()
        self.paths = self.ovstage.PathDictionary(self.stage)
        self.size = (int(width), int(height))
        self.loaded = True
        self.writer.event("loaded", path=path, seconds=round(time.perf_counter() - started, 3))
        self.changed()

    def cmd_camera(self, transform, h_aperture, v_aperture):
        np = self.np
        self.ordinal += 1
        self._write([CAMERA], "omni:xform", np.array(transform, dtype=np.float64).reshape(1, 4, 4),
                    self.ovstage.AttributeSemantic.MATRIX)
        self._write([CAMERA], "horizontalAperture", np.array([h_aperture], dtype=np.float32))
        self._write([CAMERA], "verticalAperture", np.array([v_aperture], dtype=np.float32))
        self._commit()
        self.changed()

    def cmd_size(self, width, height):
        np = self.np
        self.size = (int(width), int(height))
        self.ordinal += 1
        self._write([PRODUCT], "resolution", np.array([[self.size[0], self.size[1]]], dtype=np.int32))
        self._commit()
        self.changed()

    def cmd_xforms(self, items):
        """Local transforms of prims, [{path, matrix (16 numbers)}], written in place."""
        np = self.np
        paths = [item["path"] for item in items]
        matrices = np.array([item["matrix"] for item in items], dtype=np.float64).reshape(len(items), 4, 4)
        self.ordinal += 1
        self._write(paths, "omni:xform", matrices, self.ovstage.AttributeSemantic.MATRIX)
        self._commit()
        self.changed()

    def step(self):
        ovrtx, np = self.ovrtx, self.np
        products = self.renderer.step(render_products={PRODUCT}, delta_time=1.0 / 60, ordinal=self.ordinal)
        pixels = None
        for product in products.values():
            for frame in product.frames:
                var = frame.render_vars[LDR_COLOR].map(device=ovrtx.Device.CPU)
                view = np.from_dlpack(var)
                pixels = view.copy()
                del view
                var.unmap()
                del var
            del frame
        del products
        if pixels is None:
            raise RuntimeError("ovrtx rendered no LdrColor for %s" % PRODUCT)
        return pixels

    def _write(self, paths, name, tensor, semantic=0):
        key = tuple(paths)
        if key not in self._queries:
            path_list = self.paths.create_path_list_from_strings(list(paths))
            self._queries[key] = (path_list, self.stage.query_from_path_list(path_list))
        if name not in self._tokens:
            self._tokens[name] = self.paths.intern_token(name)
        query = self._queries[key][1]
        self.stage.write_attribute(query, self._tokens[name], ordinal=self.ordinal, tensors=tensor, is_array=False,
                                   semantic=semantic).wait()

    def _commit(self):
        self.stage.advance_write_floor(self.ordinal, self.ovstage.Scope.ALL).wait()

    def _forget_queries(self):
        for path_list, query in self._queries.values():
            query.release().wait()
            self.paths.destroy_path_list(path_list)
        self._queries.clear()
        self._tokens.clear()
        if self.paths is not None:
            self.paths.destroy()
            self.paths = None


class FakeBackend(Backend):
    """A gradient instead of ovrtx: red follows the camera's x, blue counts the
    transform edits written in place. The tests use it."""

    def __init__(self, writer):
        super().__init__(writer)
        self.camera_x = 0.0
        self.edits = 0

    def cmd_init(self, max_frames=128, image_ms=33):
        self.max_frames = min(3, max(1, int(max_frames)))
        self.writer.event("ready", version="fake", seconds=0.0)

    def cmd_load(self, path, width, height):
        self.size = (int(width), int(height))
        self.loaded = True
        self.writer.event("loaded", path=path, seconds=0.0)
        self.changed()

    def cmd_camera(self, transform, h_aperture, v_aperture):
        self.camera_x = transform[12]
        self.changed()

    def cmd_size(self, width, height):
        self.size = (int(width), int(height))
        self.changed()

    def cmd_xforms(self, items):
        self.edits += len(items)
        self.changed()

    def step(self):
        time.sleep(0.01)
        width, height = self.size
        red = int(abs(self.camera_x) * 40) % 256
        blue = (self.edits * 40) % 256
        rows = [bytes((red, (y * 255) // max(1, height - 1), blue, 255)) * width for y in range(height)]
        return _Pixels(b"".join(rows), width, height)


class _Pixels:
    """Raw RGBA bytes with the little of a numpy array that render_frame() uses."""

    def __init__(self, data, width, height):
        self._data = data
        self.shape = (height, width, 4)

    def tobytes(self):
        return self._data


def coalesced(commands):
    """The commands worth running: a load makes the loads, camera moves, sizes and
    edits queued before it pointless, as it reads all of that afresh."""
    last_load = max((i for i, (name, _) in enumerate(commands) if name == "load"), default=-1)
    skipped = {"load", "camera", "size", "xforms"}
    return [(name, fields) for i, (name, fields) in enumerate(commands) if i >= last_load or name not in skipped]


def main():
    use_packages(sys.argv)
    # Events go to a private copy of stdout, and stdout itself to stderr: what native
    # code prints can then never break into the event stream.
    events = os.fdopen(os.dup(1), "wb", buffering=0)
    os.dup2(2, 1)
    sys.stdout = sys.stderr
    # Commands are read on a thread from a copy of stdin, and stdin itself becomes
    # NUL. On Windows, while one thread waits in a read on a pipe, anything else
    # touching that pipe's handle waits too, and ovrtx's start-up does: it stalled
    # until the next command arrived.
    command_stream = os.fdopen(os.dup(0), "rb")
    null = os.open(os.devnull, os.O_RDONLY)
    os.dup2(null, 0)
    os.close(null)
    sys.stdin = open(os.devnull)  # noqa: SIM115 - for the process's life

    writer = Writer(events)
    backend = FakeBackend(writer) if "--fake" in sys.argv[1:] else RtxBackend(writer)

    commands = queue.Queue()

    def read():
        for line in command_stream:  # ends when usdcc closes the pipe or goes away
            if line.strip():
                commands.put(protocol.parse_command(line))
        commands.put(("quit", {}))

    threading.Thread(target=read, name="usdcc-rtx-commands", daemon=True).start()

    try:
        while True:
            pending = []
            try:
                pending.append(commands.get(timeout=0.25) if not backend.wants_frames() else commands.get_nowait())
            except queue.Empty:
                pass
            while True:
                try:
                    pending.append(commands.get_nowait())
                except queue.Empty:
                    break
            if any(name == "quit" for name, _ in pending):
                break
            for name, fields in coalesced(pending):
                try:
                    getattr(backend, "cmd_" + name)(**fields)
                except Exception as error:  # noqa: BLE001 - reported to usdcc, which decides
                    traceback.print_exc()
                    writer.event("error", msg="%s: %s" % (name, error), fatal=name == "init")
                    if name == "xforms":
                        writer.event("stale")  # usdcc sends the whole stage instead
            if backend.wants_frames():
                try:
                    backend.render_frame()
                except Exception as error:  # noqa: BLE001
                    traceback.print_exc()
                    backend.rendering = False
                    writer.event("error", msg="Rendering failed: %s" % error, fatal=True)
    finally:
        writer.close()
        sys.stderr.flush()
        # Not ovrtx's own shutdown, which takes seconds: the process ending frees it all.
        os._exit(0)


if __name__ == "__main__":
    main()
