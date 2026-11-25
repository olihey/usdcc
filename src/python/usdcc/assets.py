"""usdcc.assets: USD assets on servers (docs/REMOTE_ASSETS.md section 10).

    import usdcc.assets
    from pxr import Usd

    for entry in usdcc.assets.list("webdav://studio/chars/"):
        print(entry.name, entry.origin)

    stage = Usd.Stage.Open("webdav://studio/shot.usd", usdcc.assets.context("test"))

In the app and in a plain python. Every call here that asks a server waits for its answer, with
the GIL released, so other threads go on; on the app's main thread the app waits too. Nothing
here gives out a password or a token.

To open a stage in the app, ``usdcc.usd.open_stage()`` chooses the environment as File > Open
Stage does.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, NoReturn

from usdcc.usd import _native

if TYPE_CHECKING:
    import builtins

    from pxr import Ar


@dataclass(frozen=True)
class Entry:
    """A file or a folder on a server."""

    name: str
    is_folder: bool
    size: int
    modified: float  # seconds since the epoch; 0 when the server doesn't say
    etag: str  # what tells its versions apart; may be empty
    origin: str  # the server it comes from: an endpoint of the stack behind the name


def _fail(status: str, message: str, path: str) -> NoReturn:
    kind = {"not allowed": PermissionError, "unreachable": ConnectionError, "missing": FileNotFoundError}.get(
        status, OSError
    )
    raise kind(f"{path}: {message or status}")


def environments() -> builtins.list[str]:
    """The environments the configuration has."""
    return _native.asset_environments()


def default_environment() -> str:
    """The one a stage opens in when none is asked for; empty when there's none."""
    return _native.asset_default_environment()


def context(environment: str | None = None) -> Ar.ResolverContext:
    """The resolver context of `environment` (None: the default one), for ``Usd.Stage.Open()``,
    ``Ar.ResolverContext()`` and ``Ar.ResolverContextBinder()``. A ``Context``: its
    ``environment``, and ``names(scheme)``, the servers' names. ValueError when there's no
    such environment."""
    return _native.asset_context(environment or "")


def list(path: str, environment: str | None = None) -> builtins.list[Entry]:
    """What's in the folder `path` names ("webdav://studio/chars/"), sorted by name: all the
    stack's servers together, the upper one's file where several have it. FileNotFoundError,
    PermissionError, ConnectionError, or OSError for anything else that went wrong (a
    protocol that can't list, such as plain http)."""
    status, result = _native.asset_list(path, environment or "")
    if status:
        _fail(status, result, path)
    return [Entry(*entry) for entry in result]


def stat(path: str, environment: str | None = None) -> Entry | None:
    """The file `path` names, from the first server of the stack that has it. None when the
    servers answered and none has it; a server that didn't answer is a ConnectionError, as it
    is for a stage: the next server's file isn't taken instead."""
    status, result = _native.asset_stat(path, environment or "")
    if status == "missing":
        return None
    if status:
        _fail(status, result, path)
    return Entry(*result)
