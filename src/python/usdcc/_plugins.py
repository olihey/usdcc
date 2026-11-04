"""The Python side of plugins (docs/PLUGINS.md): usdcc::scripting::PluginManager
calls check_modules(), activate() and deactivate(). Plugins see the Plugin
object handed to their activate(plugin).

Each plugin's package loads by its own file location rather than through
sys.path, so nothing else in its python/ folder can shadow another plugin's
modules. Third-party modules in its deps/ folder are found by a finder at the
end of sys.meta_path, after the app's folders, so they can't replace anything
the app uses.
"""

import importlib.abc
import importlib.machinery
import importlib.metadata
import importlib.util
import json
import logging
import os
import re
import sys

from usdcc import log as _log

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    _native = None

# What the app itself brings, or would break it next to its own Qt
# (docs/PLAN.md section 6 item 11). A plugin can't ship these.
_PROTECTED_MODULES = {"PySide6", "shiboken6", "pxr", "usdcc", "PySide2", "PyQt5", "PyQt6"}
# A requirement on these is met by the app, whatever its metadata says: pxr and
# PySide6 come from builds without dist-info.
_PROVIDED_BY_APP = {"pyside6", "pyside6-essentials", "pyside6-addons", "shiboken6", "usd-core", "pxr", "usdcc"}

_active = {}
_current = None


class Settings:
    """A plugin's own settings, kept across sessions in the app's usdcc.ini.
    Values are anything JSON can hold."""

    def __init__(self, plugin_id):
        self._id = plugin_id

    def value(self, key, default=None):
        text = _native.plugin_setting(self._id, key)
        return default if text is None else json.loads(text)

    def set_value(self, key, value):
        _native.set_plugin_setting(self._id, key, json.dumps(value))

    def remove(self, key):
        _native.remove_plugin_setting(self._id, key)


class Plugin:
    """What a plugin's activate() is given.

    ``id``, ``name``, ``version`` and ``folder`` come from its manifest.
    ``settings`` keeps values across sessions, and ``log`` is a logger whose
    records have the source ``plugin:<id>`` in the Log panel.
    """

    def __init__(self, id, name, version, folder):
        self.id = id
        self.name = name
        self.version = version
        self.folder = folder
        self.settings = Settings(id)
        self.log = logging.getLogger("plugin." + id)
        self.module = None

    def __repr__(self):
        return "<Plugin %s %s>" % (self.id, self.version)


def current():
    """The plugin whose activate() is running, or None."""
    return _current


def note(kind, name):
    """Records that the plugin being activated added something, which the
    Plugin Manager lists."""
    if _current is not None and _native is not None:
        _native.plugin_note(_current.id, kind, name)


# --- Loading a plugin --------------------------------------------------------


def _load_module(name, folder):
    python = os.path.join(folder, "python")
    package = os.path.join(python, name, "__init__.py")
    if os.path.isfile(package):
        spec = importlib.util.spec_from_file_location(name, package, submodule_search_locations=[os.path.dirname(package)])
    elif os.path.isfile(os.path.join(python, name + ".py")):
        spec = importlib.util.spec_from_file_location(name, os.path.join(python, name + ".py"))
    else:
        raise ImportError("No package or module called %s in %s" % (name, python))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(name, None)
        raise
    return module


def activate(id, name, version, module, folder):
    """Imports the plugin and calls its activate(plugin). Returns '' or, when it
    failed (logged, with the traceback), the reason."""
    global _current
    plugin = Plugin(id, name, version, folder)
    _current = plugin
    try:
        plugin.module = _load_module(module, folder)
        function = getattr(plugin.module, "activate", None)
        if function is not None:
            function(plugin)
        _active[id] = plugin
        return ""
    except (Exception, SystemExit) as error:
        _log.log_exception(type(error), error, error.__traceback__, "plugin", "plugin:" + id)
        return "%s: %s" % (type(error).__name__, error)
    finally:
        _current = None


def deactivate(id):
    """Calls the plugin's deactivate(plugin), if it has one, at quit."""
    plugin = _active.pop(id, None)
    function = getattr(plugin.module, "deactivate", None) if plugin else None
    if function is None:
        return
    try:
        function(plugin)
    except Exception as error:
        _log.log_exception(type(error), error, error.__traceback__, "plugin", "plugin:" + id)


# --- Third-party modules (docs/PLUGINS.md section 3.10) ----------------------


def _canonical(name):
    return re.sub(r"[-_.]+", "-", name).lower()


def _top_level_names(dist):
    names = set()
    for file in dist.files or []:
        first = file.parts[0]
        if first == ".." or first.endswith((".dist-info", ".data")) or first == "__pycache__":
            continue
        # numpy/..., six.py, _cffi.cp312-win_amd64.pyd; not numpy.libs/...
        name = first.split(".")[0] if len(file.parts) == 1 else first
        if name.isidentifier():
            names.add(name)
    if not names:
        names.update((dist.read_text("top_level.txt") or "").split())
    return names


class _DepsFinder(importlib.abc.MetaPathFinder):
    """Finds a plugin's third-party modules: the one chosen copy of each known
    package, then whatever else its deps/ folder holds without metadata."""

    def __init__(self):
        self.chosen = {}
        self.folders = []

    def find_spec(self, fullname, path=None, target=None):
        if path is not None or "." in fullname:
            return None  # a submodule: its package knows where it is
        folder = self.chosen.get(fullname)
        for candidate in [folder] if folder else self.folders:
            spec = importlib.machinery.PathFinder.find_spec(fullname, [candidate])
            if spec is not None:
                return spec
        return None


_finder = _DepsFinder()


def _protected_in(deps):
    for entry in os.listdir(deps):
        if entry.split(".")[0] in _PROTECTED_MODULES:
            return entry.split(".")[0]
    return None


def _module_taken(module):
    if module in _PROTECTED_MODULES or module in sys.modules:
        return True
    try:
        return importlib.util.find_spec(module) is not None
    except (ImportError, ValueError):
        return True


def _specifier_text(requirement):
    return str(requirement.specifier) or "any version"


def check_modules(plugins):
    """Checks what `plugins` (dicts of id, name, module, folder and requires, in
    activation order) need from Python, running none of their code. Returns
    {id: why it's refused}, and makes the deps/ folders of the rest importable.

    A package needed by several plugins gets one copy: an installed one if there
    is one, as it can't be replaced, otherwise the newest bundled copy that
    satisfies them all. When none does, the last plugin needing it is refused
    and the rest are looked at again.
    """
    from usdcc._vendor.packaging.requirements import InvalidRequirement, Requirement
    from usdcc._vendor.packaging.version import InvalidVersion, Version

    refused = {}
    infos = {}
    owners = {}
    for plugin in plugins:
        id = plugin["id"]
        module = plugin["module"]
        if module in owners:
            refused[id] = "Its module %s is also the module of %s." % (module, owners[module])
            continue
        if _module_taken(module):
            refused[id] = "Python already has a module called %s." % module
            continue

        info = {"name": plugin["name"], "requirements": [], "dists": {}, "folder": None}
        deps = os.path.join(plugin["folder"], "deps")
        if os.path.isdir(deps):
            protected = _protected_in(deps)
            if protected:
                refused[id] = "It bundles %s, which the app provides itself." % protected
                continue
            info["folder"] = deps
            for dist in importlib.metadata.distributions(path=[deps]):
                info["dists"][_canonical(dist.name)] = dist
                for text in dist.requires or []:
                    try:
                        requirement = Requirement(text)
                    except InvalidRequirement:
                        continue  # the package's own metadata: not this plugin's fault
                    if requirement.marker is None or requirement.marker.evaluate({"extra": ""}):
                        info["requirements"].append(requirement)
        try:
            info["requirements"].extend(Requirement(text) for text in plugin.get("requires", []))
        except InvalidRequirement as error:
            refused[id] = "Its manifest has an invalid requirement: %s" % error
            continue
        owners[module] = plugin["name"]
        infos[id] = info

    installed = {}
    for dist in importlib.metadata.distributions():
        installed.setdefault(_canonical(dist.name), dist)

    def version_of(dist):
        try:
            return Version(dist.version)
        except InvalidVersion:
            return Version("0")

    chosen = {}
    while True:
        needs = {}  # package: [(plugin id, requirement or None)]
        copies = {}  # package: [(dist, deps folder)]
        for id, info in infos.items():
            for requirement in info["requirements"]:
                package = _canonical(requirement.name)
                if package not in _PROVIDED_BY_APP:
                    needs.setdefault(package, []).append((id, requirement))
            for package, dist in info["dists"].items():
                needs.setdefault(package, []).append((id, None))
                copies.setdefault(package, []).append((dist, info["folder"]))

        chosen = {}
        problem = None
        for package, requirers in needs.items():
            requirements = [(id, r) for id, r in requirers if r is not None]
            system = installed.get(package)
            if system is not None:
                candidates = [(system, None)]
            else:
                candidates = sorted(copies.get(package, []), key=lambda copy: version_of(copy[0]), reverse=True)
            if not candidates:
                id, requirement = requirements[-1]
                problem = (id, "It needs %s, which isn't installed or bundled." % requirement)
                break
            for copy in candidates:
                version = version_of(copy[0])
                if all(r.specifier.contains(version, prereleases=True) for _, r in requirements):
                    chosen[package] = copy
                    break
            else:
                id, requirement = requirements[-1]
                others = [(i, r) for i, r in requirements if i != id]
                if system is not None:
                    why = "the installed %s is %s" % (system.name, system.version)
                elif others:
                    why = "%s needs %s" % (infos[others[-1][0]]["name"], others[-1][1])
                else:
                    why = "the bundled copy is %s" % candidates[0][0].version
                problem = (id, "It needs %s, but %s." % (requirement, why))
                break
        if problem is None:
            break
        refused[problem[0]] = problem[1]
        del infos[problem[0]]

    for dist, folder in chosen.values():
        if folder:
            for name in _top_level_names(dist):
                _finder.chosen.setdefault(name, folder)
    for info in infos.values():
        if info["folder"] and info["folder"] not in _finder.folders:
            _finder.folders.append(info["folder"])
    if _finder.folders and _finder not in sys.meta_path:
        sys.meta_path.append(_finder)
    return refused
