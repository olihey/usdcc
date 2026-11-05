"""V-Ray Render: a view panel that renders its stage with V-Ray, progressively, following
the viewport camera and edits (docs/VRAY_RENDER.md).

V-Ray runs in a helper process through the V-Ray App SDK, which has to be installed
(Edit > Settings... > V-Ray says where). Try it with

    usdcc --plugin-path src/plugins

and Stages > V-Ray Render.
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

    usdcc.app.register_view_panel(plugin.id + ".view", "V-Ray Render", panel.VRayRenderPanel)


def deactivate(plugin):
    client.shutdown_all()
