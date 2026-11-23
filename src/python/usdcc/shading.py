"""usdcc.shading: which shader nodes exist, for the Material Editor (docs/MATERIAL_EDITOR.md).

``catalog()`` lists them as ``SdrRegistry`` has them, without lights and what's marked
hidden. To add a renderer, register its definitions with ``pxr.Sdr``, and describe it
with ``register_renderer_profile()``::

    import usdcc.shading as shading
    [node["identifier"] for node in shading.catalog() if node["group"] == "pbr"]
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from usdcc import _plugins

if TYPE_CHECKING:
    from collections.abc import Iterable

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    try:
        from usdcc import _usd as _native  # a plain Python
    except ImportError:
        _native = None


def _app():
    if _native is None:
        raise RuntimeError("usdcc.shading needs usdcc's USD module")
    return _native


def catalog() -> list[dict[str, Any]]:
    """Each node's default version, as dicts: identifier, label, shading_system, context,
    group, function, variant and help. The label is the one the palette shows, "Open PBR
    Surface"; function is the name as its renderer writes it, "open_pbr_surface"."""
    return _app().shader_nodes()


def register_renderer_profile(shading_system: str, display_name: str, terminal_prefix: str = "", default_surface_node: str = "",
                              terminals: Iterable[str] = ("surface",), hidden_contexts: Iterable[str] = (), icon: str = "", group_metadata: str = "",
                              hidden_groups: Iterable[str] = (), hidden_nodes: Iterable[str] = (), node_groups: dict[str, str] | None = None) -> None:
    """Says how the editor treats a shading system: its name in the palette, its Material
    terminals (``outputs:<prefix>:surface``), what "New Material" creates and connects,
    and any contexts to leave out. Registering one again replaces it. A plugin's goes
    when it reloads.

    The palette groups a system's nodes by their ``role`` metadata. For one that says
    it elsewhere, ``group_metadata`` names where (Arnold's is ``"category"``), and
    ``node_groups`` maps the identifiers of nodes that say nothing to a group.
    ``hidden_groups`` leaves whole groups out, and ``hidden_nodes`` identifiers, or
    with a ``*`` at the end, all that start so (``"arnold:imager_*"``)."""
    _app().set_renderer_profile(shading_system, display_name, str(icon), terminal_prefix, default_surface_node,
                                list(terminals), list(hidden_contexts), group_metadata, list(hidden_groups),
                                list(hidden_nodes), dict(node_groups or {}))
    _plugins.note("Renderer profile", display_name)
    _plugins.track(lambda: remove_renderer_profile(shading_system))


def remove_renderer_profile(shading_system: str) -> bool:
    """Removes a profile. True if there was one."""
    return _app().remove_renderer_profile(shading_system)


def nodes_changed() -> None:
    """Tells the palettes to read the catalog again. Call it after registering
    definitions with ``pxr.Sdr``, which can't say so itself; profiles say so
    when they're registered or removed."""
    _app().shader_nodes_changed()
