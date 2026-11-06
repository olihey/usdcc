"""usdcc.shading: which shader nodes exist, for the Material Editor (docs/MATERIAL_EDITOR.md).

``catalog()`` lists them as ``SdrRegistry`` has them, without lights and what's marked
hidden. To add a renderer, register its definitions with ``pxr.Sdr``, and describe it
with ``register_renderer_profile()``::

    import usdcc.shading as shading
    [node["identifier"] for node in shading.catalog() if node["group"] == "pbr"]
"""

from usdcc import _plugins

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


def catalog():
    """Each node's default version, as dicts: identifier, label, shading_system, context,
    group, function, variant and help."""
    return _app().shader_nodes()


def register_renderer_profile(shading_system, display_name, terminal_prefix="", default_surface_node="",
                              terminals=("surface",), hidden_contexts=(), icon=""):
    """Says how the editor treats a shading system: its name in the palette, its Material
    terminals (``outputs:<prefix>:surface``), what "New Material" creates and connects,
    and any contexts to leave out. Registering one again replaces it. A plugin's goes
    when it reloads."""
    _app().set_renderer_profile(shading_system, display_name, str(icon), terminal_prefix, default_surface_node,
                                list(terminals), list(hidden_contexts))
    _plugins.note("Renderer profile", display_name)
    _plugins.track(lambda: remove_renderer_profile(shading_system))


def remove_renderer_profile(shading_system):
    """Removes a profile. True if there was one."""
    return _app().remove_renderer_profile(shading_system)
