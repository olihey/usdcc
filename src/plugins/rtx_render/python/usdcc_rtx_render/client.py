"""Starts and talks to the RTX helper process (helper/rtx_helper.py, docs/RTX_RENDER.md
section 4).

The helper runs in usdcc's own python.exe, with ovrtx, ovstage and numpy from the
folders the app installed the plugin's packages into (python.requires), and an
environment built from scratch. ovrtx brings its own OpenUSD as one DLL with a name of
its own, so usdcc's OpenUSD on PATH doesn't get in its way; the helper takes usdcc's
pxr off its sys.path all the same. Settings > RTX can name another python.exe with
ovrtx installed instead.
"""

import ctypes
import importlib.util
import os
import sys
import weakref

from PySide6 import QtCore, QtGui

from .helper import protocol

HELPER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "helper", "rtx_helper.py")

# Every client still running, for deactivate().
_running = weakref.WeakSet()

# Inherited variables that would point the helper at usdcc's Python or OpenUSD.
_DROPPED = ("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "VIRTUAL_ENV", "PXR_PLUGINPATH_NAME", "USDCC_PYTHONHOME",
            "USDCC_PYTHON_PATH")


def usdcc_python():
    """usdcc's own python.exe, which the helper runs in."""
    return os.path.join(sys.base_prefix, "python.exe" if os.name == "nt" else "bin/python3")


def _python_dll_folder():
    """Where the running Python's DLL is: a deploy's python.exe finds it only on PATH
    (it is in bin, beside usdcc.exe). None if it can't be told."""
    handle = getattr(sys, "dllhandle", None)
    if not handle:
        return None
    buffer = ctypes.create_unicode_buffer(32768)
    if not ctypes.windll.kernel32.GetModuleFileNameW(ctypes.c_void_p(handle), buffer, len(buffer)):
        return None
    return os.path.dirname(buffer.value)


# What the helper imports from the app's packages folders.
PACKAGES = ("ovrtx", "ovstage", "numpy")


def package_folders():
    """The folders on usdcc's sys.path that PACKAGES are installed in, in sys.path
    order, or None if one is missing. Found without importing them: ovrtx must never
    load into usdcc itself."""
    folders = []
    for name in PACKAGES:
        spec = importlib.util.find_spec(name)
        if spec is None or not spec.submodule_search_locations:
            return None
        folder = os.path.dirname(os.path.normpath(next(iter(spec.submodule_search_locations))))
        if folder not in folders:
            folders.append(folder)
    return folders


def setup_problem(other_python):
    """Why the helper can't run, or None: `other_python` is Settings > RTX's other
    Python, empty for usdcc's own with the installed packages."""
    if other_python:
        return None if os.path.isfile(other_python) else "%s doesn't exist." % other_python
    if package_folders() is None:
        return ("ovrtx isn't installed. View > Plugins > RTX Render > Install Packages installs it "
                "(about 2.5 GB), for the next start.")
    return None


def helper_environment(python, own=False):
    """The helper's environment: usdcc's, without what would lead it to usdcc's Python
    or OpenUSD. PATH is the helper Python's folder and the system folders; for usdcc's
    own Python (`own`), its DLL's folder too, which in a deploy is bin, beside usdcc's
    OpenUSD. ovrtx's OpenUSD is one DLL named its own way (ov_<version>usd_ms.dll), so
    the two don't meet. Omniverse's Hub, which ovrtx would otherwise try to start and
    wait for, is turned off."""
    environment = QtCore.QProcessEnvironment.systemEnvironment()
    for name in _DROPPED:
        environment.remove(name)
    folders = [os.path.dirname(python)] if python else []
    if own:
        dll_folder = _python_dll_folder()
        if dll_folder:
            folders.append(dll_folder)
    system_root = os.environ.get("SystemRoot", r"C:\Windows")
    folders += [os.path.join(system_root, "System32"), system_root]
    environment.insert("PATH", os.pathsep.join(folders))
    environment.insert("PYTHONDONTWRITEBYTECODE", "1")
    environment.insert("PYTHONIOENCODING", "utf-8")
    environment.insert("OMNICLIENT_HUB_MODE", "disabled")
    return environment


class RenderClient(QtCore.QObject):
    """One helper process. Commands go out with ``send()``; what comes back arrives
    as signals on the UI thread."""

    ready = QtCore.Signal(dict)  # the "ready" event's fields: version, seconds
    image = QtCore.Signal(QtGui.QImage, int)  # the image and its frame since the last change
    event = QtCore.Signal(str, dict)  # any other event: "loaded", "state", "log", "error", "stale"
    output = QtCore.Signal(str)  # a line the helper wrote to stderr
    finished = QtCore.Signal(str)  # the helper ended; why, "" when asked to

    def __init__(self, other_python="", fake=False, parent=None):
        """Runs the helper in `other_python`, or in usdcc's own Python with the installed
        packages when that's empty; `fake` renders a gradient instead of importing ovrtx."""
        super().__init__(parent)
        self._reader = protocol.EventReader()
        self._stopping = False
        self._partial = ""
        python = other_python or usdcc_python()
        arguments = ["-I", "-u", HELPER]
        if fake:
            arguments.append("--fake")
        elif not other_python:
            arguments += ["--packages", os.pathsep.join(package_folders() or [])]
        self.process = QtCore.QProcess(self)
        self.process.setProgram(python)
        self.process.setArguments(arguments)
        self.process.setProcessEnvironment(helper_environment(python, own=not other_python))
        self.process.readyReadStandardOutput.connect(self._read_events)
        self.process.readyReadStandardError.connect(self._read_errors)
        self.process.finished.connect(self._ended)
        self.process.errorOccurred.connect(self._failed)

    def start(self):
        self._stopping = False
        self.process.start()
        _running.add(self)

    def running(self):
        return self.process.state() != QtCore.QProcess.ProcessState.NotRunning

    def send(self, name, **fields):
        if self.running():
            self.process.write(protocol.command(name, **fields))

    def shutdown(self, wait_ms=3000):
        """Asks the helper to quit, then makes sure it does."""
        _running.discard(self)
        if not self.running():
            return
        self._stopping = True
        self.send("quit")
        self.process.closeWriteChannel()
        if not self.process.waitForFinished(wait_ms):
            self.process.kill()
            self.process.waitForFinished(1000)

    def _read_events(self):
        for header, payload in self._reader.feed(bytes(self.process.readAllStandardOutput())):
            name = header.pop("ev", "")
            if name == "image":
                width, height = header["w"], header["h"]
                picture = QtGui.QImage(payload, width, height, width * 4, QtGui.QImage.Format.Format_RGBA8888).copy()
                self.image.emit(picture, header.get("frame", 0))
            elif name == "ready":
                self.ready.emit(header)
            else:
                self.event.emit(name, header)

    def _read_errors(self):
        self._partial += bytes(self.process.readAllStandardError()).decode("utf-8", "replace")
        *lines, self._partial = self._partial.split("\n")
        for line in lines:
            if line.strip():
                self.output.emit(line.rstrip("\r"))

    def _ended(self, code, status):
        _running.discard(self)
        if self._stopping:
            self.finished.emit("")
        elif status == QtCore.QProcess.ExitStatus.CrashExit:
            self.finished.emit("The RTX helper crashed.")
        else:
            self.finished.emit("The RTX helper stopped (exit code %d)." % code)

    def _failed(self, error):
        if error == QtCore.QProcess.ProcessError.FailedToStart:
            _running.discard(self)
            self.finished.emit("Could not start %s." % self.process.program())


def shutdown_all():
    """Stops every helper still running (the plugin's deactivate())."""
    for client in list(_running):
        client.shutdown()
