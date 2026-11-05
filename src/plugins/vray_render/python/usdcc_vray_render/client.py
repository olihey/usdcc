"""Starts and talks to the V-Ray helper process (helper/vray_helper.py, docs/VRAY_RENDER.md
section 4).

The helper runs in usdcc's python.exe, with an environment built from scratch:
V-Ray's folders first on PATH, nothing of usdcc's Python or OpenUSD, so V-Ray's
own USD DLLs are the ones that load.
"""

import ctypes
import os
import sys
import weakref

from PySide6 import QtCore, QtGui

from .helper import protocol

HELPER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "helper", "vray_helper.py")

# Every client still running, for deactivate().
_running = weakref.WeakSet()

# Inherited variables that would point the helper at usdcc's Python or OpenUSD,
# or at another V-Ray.
_DROPPED = ("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "VIRTUAL_ENV", "VRAY_PATH", "PXR_PLUGINPATH_NAME",
            "USDCC_PYTHONHOME", "USDCC_PYTHON_PATH")


def default_python():
    """The python.exe of usdcc's own Python: the one beside the standard library
    usdcc runs, which is the deploy's python folder or the Python it was built with."""
    return os.path.join(sys.base_prefix, "python.exe" if os.name == "nt" else "bin/python3")


def _python_dll_folder():
    """Where the running Python's DLL is: a deploy's python.exe finds it only on
    PATH (it is in bin, beside usdcc.exe). None if it can't be told."""
    handle = getattr(sys, "dllhandle", None)
    if not handle:
        return None
    buffer = ctypes.create_unicode_buffer(32768)
    if not ctypes.windll.kernel32.GetModuleFileNameW(ctypes.c_void_p(handle), buffer, len(buffer)):
        return None
    return os.path.dirname(buffer.value)


def sdk_problem(sdk):
    """Why `sdk` isn't a V-Ray App SDK folder, or None if it looks like one."""
    if not sdk:
        return "No V-Ray App SDK folder is set."
    if not os.path.isfile(os.path.join(sdk, "python", "vray.py")):
        return "%s has no python/vray.py: it isn't a V-Ray App SDK folder." % sdk
    if os.name == "nt" and not os.path.isfile(os.path.join(sdk, "bin", "VRaySDKLibrary.dll")):
        return "%s has no bin/VRaySDKLibrary.dll." % sdk
    return None


def helper_environment(sdk):
    """The helper's environment: usdcc's, without what would lead it to usdcc's
    Python or OpenUSD, and with a PATH of V-Ray's bin folder, the Python DLL's
    folder and the system folders only."""
    environment = QtCore.QProcessEnvironment.systemEnvironment()
    for name in _DROPPED:
        environment.remove(name)
    folders = []
    if sdk:
        folders.append(os.path.join(sdk, "bin"))
    dll_folder = _python_dll_folder()
    if dll_folder:
        folders.append(dll_folder)
    system_root = os.environ.get("SystemRoot", r"C:\Windows")
    folders += [os.path.join(system_root, "System32"), system_root]
    environment.insert("PATH", os.pathsep.join(folders))
    environment.insert("PYTHONDONTWRITEBYTECODE", "1")
    environment.insert("PYTHONIOENCODING", "utf-8")
    return environment


class RenderClient(QtCore.QObject):
    """One helper process. Commands go out with ``send()``; what comes back
    arrives as signals on the UI thread."""

    ready = QtCore.Signal(dict)  # the "ready" event's fields: version, api
    image = QtCore.Signal(QtGui.QImage, int)  # the image and V-Ray's change index
    event = QtCore.Signal(str, dict)  # any other event: "loaded", "state", "log", "error"
    output = QtCore.Signal(str)  # a line the helper wrote to stderr
    finished = QtCore.Signal(str)  # the helper ended; why, "" when asked to

    def __init__(self, python, sdk, fake=False, parent=None):
        super().__init__(parent)
        self._reader = protocol.EventReader()
        self._stopping = False
        self._partial = ""
        self.process = QtCore.QProcess(self)
        self.process.setProgram(python)
        self.process.setArguments(["-I", "-u", HELPER] + (["--fake"] if fake else []))
        self.process.setProcessEnvironment(helper_environment(sdk))
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
                self.image.emit(picture, header.get("change", 0))
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
            self.finished.emit("The V-Ray helper crashed.")
        else:
            self.finished.emit("The V-Ray helper stopped (exit code %d)." % code)

    def _failed(self, error):
        if error == QtCore.QProcess.ProcessError.FailedToStart:
            _running.discard(self)
            self.finished.emit("Could not start %s." % self.process.program())


def shutdown_all():
    """Stops every helper still running (the plugin's deactivate())."""
    for client in list(_running):
        client.shutdown()
