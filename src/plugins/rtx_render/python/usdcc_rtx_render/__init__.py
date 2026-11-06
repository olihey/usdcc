"""RTX Render: a view panel that renders its stage with NVIDIA's ovrtx (Omniverse RTX),
following the viewport camera and edits (docs/RTX_RENDER.md).

ovrtx runs in a helper process, in a Python of its own with ovrtx installed
(Edit > Settings... > RTX says which). Try it with

    usdcc --plugin-path src/plugins

and Stages > RTX Render.
"""

from . import client, config, panel

PLUGIN = None


def activate(plugin):
    """Registers the panel type and the settings; nothing starts until a panel renders."""
    global PLUGIN
    PLUGIN = plugin
    panel.LOG = plugin.log
    config.define()
    import usdcc.app

    usdcc.app.register_view_panel(plugin.id + ".view", "RTX Render", panel.RtxRenderPanel)


def deactivate(plugin):
    client.shutdown_all()
