"""Gives the Material Editor V-Ray's shader nodes when the plugin starts (shaders.py,
docs/VRAY_RENDER.md section 8).

From the cache when this V-Ray build's definitions have been made; else plugininfo makes
them in a thread, about half a minute, and the nodes appear in the palettes when it's
done. USDCC_VRAY_SHADER_NODES=0 turns this off, as the tests do.
"""

import logging
import os
import threading

from PySide6 import QtCore

from . import client as clients
from . import config, shaders

LOG = logging.getLogger("plugin.org.usdcc.vray-render")  # activate() replaces it with plugin.log
ENVIRONMENT = "USDCC_VRAY_SHADER_NODES"


class _Finished(QtCore.QObject):
    """Brings the thread's result to the UI thread, where Sdr and the palettes are."""

    made = QtCore.Signal(str, list, str)  # the file, its identifiers, why it failed


class Loader:
    """One load: from the cache now, or made in a thread. ``state`` says where it is:
    "off", "unavailable", "making", "registered" or "failed"."""

    def __init__(self, sdk, folder):
        self.sdk = sdk
        self.cache = shaders.ShaderCache(folder)
        self.state = "idle"
        self.message = ""
        self.path = None
        self._finished = None
        self._thread = None

    def start(self):
        problem = clients.sdk_problem(self.sdk)
        if problem is None and shaders.plugininfo(self.sdk) is None:
            problem = "%s has no bin/plugininfo.exe." % self.sdk
        if problem:
            self.state, self.message = "unavailable", problem
            LOG.info("No V-Ray shader nodes for the Material Editor: %s", problem)
            return
        path = self.cache.cached(self.sdk)
        if path:
            self._register(path, self.cache.identifiers(path))
            return
        self.state = "making"
        LOG.info("Making the V-Ray shader node definitions for the Material Editor with plugininfo "
                 "(about half a minute, once per V-Ray build)")
        self._finished = _Finished()
        self._finished.made.connect(self._made)
        self._thread = threading.Thread(target=self._make, name="usdcc-vray-shader-nodes", daemon=True)
        self._thread.start()

    def cancel(self):
        """The plugin is going: what the thread makes is still cached, but not registered."""
        if self._finished is not None:
            self._finished.made.disconnect(self._made)
            self._finished = None

    def _make(self):
        finished = self._finished
        try:
            path = self.cache.make(self.sdk)
            result = (path, self.cache.identifiers(path), "")
        except Exception as error:  # noqa: BLE001 - reported on the UI thread
            result = ("", [], str(error) or type(error).__name__)
        if finished is not None:
            finished.made.emit(*result)

    def _made(self, path, identifiers, error):
        self._finished = None
        if error:
            self.state, self.message = "failed", error
            LOG.warning("Couldn't make the V-Ray shader node definitions: %s", error)
            return
        self._register(path, identifiers)

    def _register(self, path, identifiers):
        import usdcc.shading

        added = shaders.register(path, identifiers)
        shaders.register_profile()
        usdcc.shading.nodes_changed()
        self.state, self.path = "registered", path
        self.message = "%d V-Ray shader nodes from %s" % (len(identifiers), path)
        LOG.info("%s%s", self.message, "" if added else " (registered before)")


CURRENT = None


def start():
    """Loads the nodes, unless USDCC_VRAY_SHADER_NODES=0 or the renderer is the tests'
    fake. Returns the Loader, or None."""
    global CURRENT
    if os.environ.get(ENVIRONMENT, "1") == "0" or config.FAKE_RENDERER:
        return None
    import usdcc.app

    CURRENT = Loader(config.get(config.SDK), os.path.join(usdcc.app.user_data_folder(), "vray", "shader_nodes"))
    CURRENT.start()
    return CURRENT


def stop():
    """The plugin is going. Its profile goes; the nodes stay in Sdr, which can't
    forget them, and come back when it starts again."""
    global CURRENT
    if CURRENT is None:
        return
    CURRENT.cancel()
    if CURRENT.state == "registered":
        import usdcc.shading

        # Registered after activate(), so not taken back for the plugin by itself.
        usdcc.shading.remove_renderer_profile(shaders.SYSTEM)
    CURRENT = None
