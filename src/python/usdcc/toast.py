"""usdcc.toast: toast notifications in the main window (docs/NOTIFICATIONS.md).

    import usdcc.toast as toast
    toast.show("Exported", "3 prims written to\\nC:/out/scene.usda", kind="success")
    toast_id = toast.show("Baking", "This takes a while...", sticky=True)
    toast.dismiss(toast_id)

Kinds: "info", "success", "warning", "error". A toast disappears after the
kind's time (Settings > Notifications) or `duration` seconds, or stays until
dismissed when `sticky`. To log a message and show it as a toast at once, use
usdcc.core.log(..., toast=True) or logging's extra={"usdcc_toast": True}.

Needs the running app with its window.
"""

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    _native = None

KINDS = ("info", "success", "warning", "error")


def _app():
    if _native is None:
        raise RuntimeError("usdcc.toast needs the running usdcc app")
    return _native


def show(title, body="", kind="info", duration=None, sticky=False):
    """Shows a toast and returns its id. `body` may have several lines; the
    text sets the toast's size. `duration` is in seconds (default: the kind's,
    from the settings); `sticky` keeps it until dismissed. Showing the same
    toast while it is up counts it on that one ("x2") and returns its id."""
    if sticky:
        duration_ms = 0
    elif duration is None:
        duration_ms = -1
    else:
        if duration <= 0:
            raise ValueError("duration must be positive; use sticky=True to keep a toast")
        duration_ms = max(1, int(round(duration * 1000)))
    return _app().toast_show(str(title), str(body), kind, duration_ms)


def dismiss(toast_id):
    """Closes a toast (or drops one still waiting to be shown)."""
    _app().toast_dismiss(toast_id)


def dismiss_all():
    """Closes every toast."""
    _app().toast_dismiss_all()


def visible():
    """The ids of the toasts that are up, newest first."""
    return _app().toast_visible()
