"""usdcc.debug: lets a debugger attach to the Python in usdcc (docs/VSCODE.md section 5).

    import usdcc.debug
    usdcc.debug.listen()    # the port, such as 5678
    breakpoint()            # stops in the attached debugger; does nothing without one

Attach with VS Code's Python Debugger to 127.0.0.1 and that port. Breakpoints then stop in
scripts, whether run from VS Code or the Script Editor, and in plugins. While one is paused
the app is frozen, as scripts run on its main thread.

Only once asked for, as any process on this machine can connect to the port, and a debugger
can run code. It can't be taken back until usdcc quits.

Needs the running app.
"""

from __future__ import annotations

import contextlib
import os
import socket
import sys

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    _native = None

_HOST = "127.0.0.1"


def _app():
    if _native is None:
        raise RuntimeError("usdcc.debug needs the running usdcc app")
    return _native


def is_listening() -> bool:
    """Whether a debugger can attach."""
    return port() is not None


def port() -> int | None:
    """The port a debugger can attach to, or None."""
    return _app().debug_port() or None


def is_attached() -> bool:
    """Whether a debugger is attached."""
    debugpy = sys.modules.get("debugpy")
    return is_listening() and debugpy is not None and debugpy.is_client_connected()


def listen(port: int | None = None) -> int:
    """Lets a debugger attach from now on, and returns the port.

    `port` is the one to use, by default the setting Python/debugPort; a free one if that is
    taken, as by another usdcc. Listening already, it returns the port it has.
    """
    app = _app()
    if app.debug_port():
        return app.debug_port()
    folder, executable, wanted = app.debug_info()
    wanted = _usable(wanted if port is None else port)
    with _environment():
        debugpy = _import(folder)
        # Its helper process would be started with sys.executable, which is usdcc itself.
        # Processes that scripts and plugins start aren't debugged: they're left as they are.
        debugpy.configure(python=executable, subProcess=False)
        _, listening = debugpy.listen((_HOST, wanted))
    app.set_debug_port(listening)
    return listening


def _usable(wanted):
    with contextlib.closing(socket.socket()) as probe:
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        try:
            probe.bind((_HOST, wanted))
        except OSError:
            return 0  # any
    return wanted


def _import(folder):
    if folder and folder not in sys.path:
        sys.path.append(folder)  # last, so nothing gets it instead of its own
    try:
        import debugpy
    except ImportError as error:
        raise RuntimeError("debugpy isn't there: this usdcc was built without it, and it isn't among the Python packages") from error
    return debugpy


@contextlib.contextmanager
def _environment():
    # For the helper process: a deploy's python.exe finds its DLL beside usdcc.exe.
    # For pydevd, as it's loaded: not to warn about frozen modules, the standard library's.
    old = {name: os.environ.get(name) for name in ("PATH", "PYDEVD_DISABLE_FILE_VALIDATION")}
    os.environ["PATH"] = os.pathsep.join(filter(None, (os.path.dirname(sys.executable), old["PATH"])))
    os.environ["PYDEVD_DISABLE_FILE_VALIDATION"] = "1"
    try:
        yield
    finally:
        for name, value in old.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
