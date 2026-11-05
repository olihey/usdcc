"""Routes Python's output into the usdcc log (docs/LOGGING.md section 3.4).

The app's embedded interpreter calls install() before any other Python runs
(usdcc::scripting::PythonRuntime). From then on these all become records in the
Log panel and the log file, with source "python":

- the logging module: a handler on the root logger; the category is the
  logger's name ("my.tool"), the level maps across (CRITICAL becomes Error);
- warnings.warn(): through logging.captureWarnings, category "py.warnings";
- print() and sys.stdout: one Info record per line, category "python.stdout";
- sys.stderr: one Warning record per line, category "python.stderr";
- uncaught exceptions, also in threads: an Error with the traceback in the
  record's details, category "python.exception" / "python.thread".

A logging record with ``extra={"usdcc_toast": True}`` (or ``"sticky"``) is also
shown as a toast notification (docs/NOTIFICATIONS.md), like
usdcc.core.log(..., toast=True); ``extra={"usdcc_toast_title": ...}`` titles it.

Outside the app there is no _usdcc_log module: install() does nothing and the
user's own logging setup applies.
"""

import io
import logging
import sys
import threading
import traceback

try:
    import _usdcc_log as _hub
except ImportError:  # a plain python, not the app
    _hub = None

# usdcc::core::LogLevel
TRACE, DEBUG, INFO, WARNING, ERROR, FATAL = range(6)
SOURCE = "python"


def _level(levelno):
    if levelno >= logging.ERROR:
        return ERROR
    if levelno >= logging.WARNING:
        return WARNING
    if levelno >= logging.INFO:
        return INFO
    if levelno >= logging.DEBUG:
        return DEBUG
    return TRACE


def _thread_name():
    # The main thread already has its name ("main") on the C++ side.
    name = threading.current_thread().name
    return "" if name == "MainThread" else name


def _toast_mode(toast):
    """True/"timed" -> "timed", "sticky" -> "sticky", False/None -> ""."""
    if toast is None or toast is False or toast == "":
        return ""
    if toast is True or toast == "timed":
        return "timed"
    if toast == "sticky":
        return "sticky"
    raise ValueError("toast must be True, False, 'timed' or 'sticky'")


def post(level, category, message, details="", file="", line=0, function="", source=SOURCE, toast=False,
         toast_title="", toast_kind=""):
    """Posts one record to the log (no-op outside the app). `toast` True or
    "sticky" also shows it as a toast notification."""
    mode = _toast_mode(toast)
    if _hub is not None:
        _hub.post(level, source, category, message, details, file, line or 0, function, _thread_name(),
                  mode, toast_title or "", toast_kind or "")


def log_exception(exc_type, exc_value, exc_traceback, category="python.exception", source=SOURCE):
    """Logs an exception as an Error: its last line as the message, the full
    traceback as details, and the innermost frame as the location."""
    if exc_type is None:
        return
    message = traceback.format_exception_only(exc_type, exc_value)[-1].strip()
    details = "".join(traceback.format_exception(exc_type, exc_value, exc_traceback)).rstrip()
    file, line, function = "", 0, ""
    frames = traceback.extract_tb(exc_traceback)
    if frames:
        file, line, function = frames[-1].filename, frames[-1].lineno, frames[-1].name
    elif isinstance(exc_value, SyntaxError):
        # Raised while compiling, before any frame ran: it carries its own location.
        file, line = exc_value.filename or "", exc_value.lineno or 0
    post(ERROR, category, message, details, file, line, function, source)


class _HubHandler(logging.Handler):
    def emit(self, record):
        try:
            level = _level(record.levelno)
            toast = getattr(record, "usdcc_toast", False)
            # A plugin's logger, plugin.log, is "plugin.<id>" (docs/PLUGINS.md).
            source = "plugin:" + record.name[7:] if record.name.startswith("plugin.") else SOURCE
            if not toast and not _hub.is_enabled(level, source, record.name):
                return
            details = ""
            if record.exc_info and record.exc_info[0] is not None:
                details = "".join(traceback.format_exception(*record.exc_info)).rstrip()
            elif record.stack_info:
                details = record.stack_info
            post(level, record.name, record.getMessage(), details, record.pathname, record.lineno, record.funcName, source,
                 toast=toast, toast_title=getattr(record, "usdcc_toast_title", ""))
        except Exception:
            self.handleError(record)


class _HubStream(io.TextIOBase):
    """A text stream that posts each complete line it is given."""

    encoding = "utf-8"
    errors = "replace"

    def __init__(self, category, level, original):
        super().__init__()
        self._category = category
        self._level = level
        self._original = original
        self._pending = ""
        self._lock = threading.Lock()

    def writable(self):
        return True

    def isatty(self):
        return False

    def fileno(self):
        # The C-level stream underneath (captured into the log as "stdout" /
        # "stderr" by usdcc), for code that writes to the descriptor itself.
        if self._original is None:
            raise io.UnsupportedOperation("fileno")
        return self._original.fileno()

    def write(self, text):
        if not isinstance(text, str):
            raise TypeError(f"write() argument must be str, not {type(text).__name__}")
        with self._lock:
            *lines, self._pending = (self._pending + text).split("\n")
        for line in lines:
            line = line.rstrip("\r")
            if line:
                post(self._level, self._category, line)
        return len(text)

    def flush(self):
        with self._lock:
            line, self._pending = self._pending, ""
        line = line.rstrip("\r")
        if line:
            post(self._level, self._category, line)


def install():
    """Routes logging, warnings, stdout, stderr and uncaught exceptions to the
    usdcc log. Returns False outside the app, where it does nothing."""
    if _hub is None:
        return False
    root = logging.getLogger()
    if not any(isinstance(handler, _HubHandler) for handler in root.handlers):
        root.addHandler(_HubHandler())
    # Let every record through to the handler: the hub's own levels (USDCC_LOG)
    # decide what is kept, as for every other source.
    root.setLevel(logging.NOTSET)
    logging.captureWarnings(True)
    if not isinstance(sys.stdout, _HubStream):
        sys.stdout = _HubStream("python.stdout", INFO, sys.__stdout__)
    if not isinstance(sys.stderr, _HubStream):
        sys.stderr = _HubStream("python.stderr", WARNING, sys.__stderr__)
    sys.excepthook = log_exception
    threading.excepthook = lambda args: log_exception(
        args.exc_type, args.exc_value, args.exc_traceback, "python.thread")
    return True
