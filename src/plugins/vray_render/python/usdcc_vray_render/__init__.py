"""V-Ray Render: a view panel that renders its stage with V-Ray, progressively, following
the viewport camera and edits (docs/VRAY_RENDER.md), and V-Ray's shader nodes for the
Material Editor (section 8).

V-Ray runs in a helper process through the V-Ray App SDK, which has to be installed
(Edit > Settings... > V-Ray says where). Try it with

    usdcc --plugin-path src/plugins

and Stages > V-Ray Render.
"""

from PySide6 import QtCore

from . import client, config, node_loader, panel

PLUGIN = None


def activate(plugin):
    """Registers the panel type and the settings; nothing starts until a panel renders.
    The shader nodes load once the app is back in its event loop."""
    global PLUGIN
    PLUGIN = plugin
    panel.LOG = plugin.log
    node_loader.LOG = plugin.log
    config.define()
    import usdcc.app

    usdcc.app.register_view_panel(plugin.id + ".view", "V-Ray Render", panel.VRayRenderPanel)
    QtCore.QTimer.singleShot(0, _load_shader_nodes)


def _load_shader_nodes():
    if PLUGIN is not None:
        node_loader.start()


def deactivate(plugin):
    global PLUGIN
    PLUGIN = None
    node_loader.stop()
    client.shutdown_all()
