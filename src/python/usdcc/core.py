"""usdcc.core: the usdcc log from Python (docs/LOGGING.md, docs/SCRIPTING.md section 4.1).

Python's own logging module already reaches the log inside the app (see
usdcc.log); this is for posting with an explicit category and level, and for
changing which levels are kept, as USDCC_LOG does at start-up::

    import usdcc.core
    usdcc.core.log(usdcc.core.WARNING, "my.tool", "Something looks odd")
    usdcc.core.set_levels("info,my.tool=debug")
    usdcc.core.log(usdcc.core.ERROR, "my.tool", "Export failed", toast="sticky")
"""

from usdcc.log import DEBUG, ERROR, FATAL, INFO, TRACE, WARNING, post

try:
    import _usdcc_log as _hub
except ImportError:  # a plain python, not the app
    _hub = None

__all__ = ["TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL", "log", "set_levels"]


def log(level, category, message, details="", toast=False, title="", kind=None):
    """Posts a record (source "python") to the log. No-op outside the app.

    `toast` True (or "sticky", to stay until dismissed) also shows it as a
    toast notification (docs/NOTIFICATIONS.md), whatever levels the log keeps:
    titled `title` (else the level's name), of `kind` ("info", "success",
    "warning", "error"; else the level's), the message (and details) as its
    body.
    """
    post(level, category, message, details, toast=toast, toast_title=title, toast_kind=kind or "")


def set_levels(spec):
    """Sets which levels the log keeps, in USDCC_LOG's format ("warning",
    "info,usdcc.viewport=debug,usd=error"). Returns False if part of `spec`
    couldn't be parsed. Raises RuntimeError outside the app."""
    if _hub is None:
        raise RuntimeError("usdcc.core.set_levels needs the running usdcc app")
    return _hub.configure_levels(spec)
