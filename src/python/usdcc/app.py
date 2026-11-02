"""usdcc.app: the running usdcc application (docs/SCRIPTING.md section 4.1).

``is_embedded()`` tells whether this python runs inside usdcc. Inside it,
``stage_manager()`` and ``undo_manager()`` are the app's own objects; most
scripts use the friendlier usdcc.usd functions instead.
"""

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    _native = None


def is_embedded():
    """True when running inside usdcc (and the app is up)."""
    return _native is not None and _native.is_available()


def _app():
    if _native is None:
        raise RuntimeError("not running inside usdcc")
    return _native


def stage_manager():
    """The app's StageManager (see usdcc.usd for working with its stages)."""
    return _app().stage_manager()


def undo_manager():
    """The app's UndoManager (see usdcc.usd.edit/undo/redo)."""
    return _app().undo_manager()
