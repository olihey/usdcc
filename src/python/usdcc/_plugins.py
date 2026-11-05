"""The Python side of plugins (docs/PLUGINS.md): usdcc::scripting::PluginManager
calls check_modules(), activate() and deactivate(). Plugins see the Plugin
object handed to their activate(plugin).

Each plugin's package loads by its own file location rather than through
sys.path, so nothing else in its python/ folder can shadow another plugin's
modules. Third-party modules in its deps/ folder are found by a finder at the
end of sys.meta_path, after the app's folders, so they can't replace anything
the app uses.
"""

import functools
import importlib
import importlib.abc
import importlib.machinery
import importlib.metadata
import importlib.util
import json
import logging
import os
import re
import shutil
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
# The app won't install these, even for a package that pulls one in: a second Qt
# or USD in the process takes it down (docs/PYTHON_PACKAGES.md).
_NEVER_INSTALLED = _PROVIDED_BY_APP | {"pyside2", "pyqt5", "pyqt5-qt5", "pyqt6", "pyqt6-qt6"}

_active = {}
_current = None
# What reloading needs (docs/PLUGIN_DEV.md).
_undo = {}  # plugin id: what to call to take back what it registered, oldest first
_owners = {}  # top-level module name: plugin id
_panel_types = {}  # plugin id: the view panel type ids it has ever registered
_panel_classes = {}  # view panel type id: its class


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


def track(undo, callback=None):
    """Records how to take back something just registered, for when the plugin
    reloads: for the plugin being activated or, as a panel can connect to an event
    long after that, the one `callback` belongs to. Outside any plugin, nothing."""
    owner = _owner_of(callback) if callback is not None else None
    if owner is None and _current is not None:
        owner = _current.id
    if owner is not None:
        _undo.setdefault(owner, []).append(undo)


def _owner_of(callback):
    if isinstance(callback, functools.partial):
        callback = callback.func
    module = getattr(getattr(callback, "__func__", callback), "__module__", None)
    return _owners.get(module.split(".")[0]) if module else None


def register_panel_class(id, cls):
    """Remembers a view panel type made in Python, for panels to rebuild from."""
    _panel_classes[id] = cls
    if _current is not None:
        _panel_types.setdefault(_current.id, set()).add(id)


def forget_panel_class(id):
    _panel_classes.pop(id, None)


def panel_class(id):
    """The class of a view panel type, or None if nothing registers it now."""
    return _panel_classes.get(id)


def panel_types(id):
    """The view panel types a plugin has registered, now or at any earlier reload."""
    return sorted(_panel_types.get(id, ()))


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
    _owners[module] = id
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


# --- Reloading (docs/PLUGIN_DEV.md) -------------------------------------------


def has_deactivate(id):
    """Whether a running plugin has a deactivate(), which is the only place to clean
    up what it set up directly, such as a QTimer."""
    plugin = _active.get(id)
    return plugin is not None and getattr(plugin.module, "deactivate", None) is not None


def _is_inside(path, folder):
    try:
        path = os.path.normcase(os.path.abspath(path))
    except (TypeError, ValueError):
        return False
    return path.startswith(os.path.normcase(os.path.abspath(folder)) + os.sep)


def _forget_modules(module, folder):
    """Makes the next import read the plugin's files again. Only its own: a compiled
    package from deps/ or the shared folder can't be loaded twice."""
    python = os.path.join(folder, "python")
    for name, loaded in list(sys.modules.items()):
        if name == module or name.startswith(module + ".") or _is_inside(getattr(loaded, "__file__", None), python):
            del sys.modules[name]
    # A .pyc is matched to its source by size and a time in whole seconds, so a quick
    # edit of the same length would go on running the old code.
    for parent, folders, _files in os.walk(python):
        if "__pycache__" in folders:
            shutil.rmtree(os.path.join(parent, "__pycache__"), ignore_errors=True)
            folders.remove("__pycache__")
    importlib.invalidate_caches()


def unload(id, module, folder):
    """Takes a plugin out as far as can be done: its deactivate(), everything it
    registered, and its modules. What it did directly, such as connecting a Qt signal,
    only its deactivate() can end."""
    deactivate(id)
    for undo in reversed(_undo.pop(id, [])):
        try:
            undo()
        except Exception as error:
            _log.log_exception(type(error), error, error.__traceback__, "plugin", "plugin:" + id)
    _owners.pop(module, None)
    _forget_modules(module, folder)


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


def _manifest_requirements(texts):
    """The requirements in a manifest that apply on this machine. Only names,
    versions and markers: a URL would let a manifest pick where code comes from.
    Raises InvalidRequirement."""
    from usdcc._vendor.packaging.requirements import InvalidRequirement, Requirement

    requirements = []
    for text in texts:
        requirement = Requirement(text)
        if requirement.url:
            raise InvalidRequirement("%s names a URL, and only names and versions are allowed" % text)
        if requirement.marker is None or requirement.marker.evaluate({"extra": ""}):
            requirements.append(requirement)
    return requirements


def _is_in_folder(dist, folder):
    if not folder:
        return False
    location = os.path.abspath(str(dist.locate_file("")))
    return os.path.normcase(location) == os.path.normcase(os.path.abspath(folder))


def check_modules(plugins, shared_folder=""):
    """Checks what `plugins` (dicts of id, name, module, folder and requires, in
    activation order) need from Python, running none of their code. Returns
    {id: (why it's refused, whether installing packages could fix it)}, and makes
    the deps/ folders of the rest importable.

    A package needed by several plugins gets one copy: an installed one if there
    is one, as it can't be replaced, otherwise the newest bundled copy that
    satisfies them all. An installed copy refuses the plugins it doesn't suit.
    When no bundled copy suits them all, the last plugin needing it is refused
    and the rest are looked at again. `shared_folder` is the environment the app
    installs into, whose versions can change (docs/PYTHON_PACKAGES.md).
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
            refused[id] = ("Its module %s is also the module of %s." % (module, owners[module]), False)
            continue
        # A plugin that's running, here for what it needs when another reloads.
        if not plugin.get("loaded") and _module_taken(module):
            refused[id] = ("Python already has a module called %s." % module, False)
            continue

        info = {"name": plugin["name"], "requirements": [], "dists": {}, "folder": None}
        deps = os.path.join(plugin["folder"], "deps")
        if os.path.isdir(deps):
            protected = _protected_in(deps)
            if protected:
                refused[id] = ("It bundles %s, which the app provides itself." % protected, False)
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
            info["requirements"].extend(_manifest_requirements(plugin.get("requires", [])))
        except InvalidRequirement as error:
            refused[id] = ("Its manifest has an invalid requirement: %s" % error, False)
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

        # All of what's missing at once, so a plugin can be told everything it
        # needs, not one package per start.
        missing = {}
        for package, requirers in needs.items():
            if package not in installed and package not in copies:
                for id, requirement in requirers:
                    if requirement is not None:
                        missing.setdefault(id, []).append(str(requirement))
        if missing:
            for id, texts in missing.items():
                verb = "isn't" if len(texts) == 1 else "aren't"
                refused[id] = ("It needs %s, which %s installed or bundled." % (", ".join(texts), verb), True)
                del infos[id]
            continue

        chosen = {}
        problem = None
        for package, requirers in needs.items():
            requirements = [(id, r) for id, r in requirers if r is not None]
            system = installed.get(package)
            if system is not None:
                candidates = [(system, None)]
            else:
                candidates = sorted(copies.get(package, []), key=lambda copy: version_of(copy[0]), reverse=True)
            for copy in candidates:
                version = version_of(copy[0])
                if all(r.specifier.contains(version, prereleases=True) for _, r in requirements):
                    chosen[package] = copy
                    break
            else:
                if system is not None:
                    # The copy is fixed, so the fault is with those it doesn't suit.
                    version = version_of(system)
                    id, requirement = [(i, r) for i, r in requirements if not r.specifier.contains(version, prereleases=True)][-1]
                    why = "the installed %s is %s" % (system.name, system.version)
                    problem = (id, "It needs %s, but %s." % (requirement, why), _is_in_folder(system, shared_folder))
                    break
                id, requirement = requirements[-1]
                others = [(i, r) for i, r in requirements if i != id]
                if others:
                    why = "%s needs %s" % (infos[others[-1][0]]["name"], others[-1][1])
                else:
                    why = "the bundled copy is %s" % candidates[0][0].version
                problem = (id, "It needs %s, but %s." % (requirement, why), False)
                break
        if problem is None:
            break
        refused[problem[0]] = problem[1:]
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


# --- Installing packages (docs/PYTHON_PACKAGES.md) ----------------------------


def package_requests(plugins, user_packages, extra_folders):
    """What should be installed: (requests, blocked names).

    Each request is a dict of id, name, requirements and problem, in the order
    they're resolved: what the user asked for (id "") first, then `plugins`
    (dicts as for check_modules()). Requirements are normalized, so nothing but a
    requirement reaches the installer. Left out is what the app provides, what a
    plugin's own deps/ meets, and what `extra_folders` already meet.
    """
    from usdcc._vendor.packaging.requirements import InvalidRequirement
    from usdcc._vendor.packaging.version import InvalidVersion, Version

    def meets(dist, requirement):
        try:
            return requirement.specifier.contains(Version(dist.version), prereleases=True)
        except InvalidVersion:
            return False

    provided = {}
    for dist in importlib.metadata.distributions(path=list(extra_folders)):
        provided.setdefault(_canonical(dist.name), dist)

    def request(id, name, texts, deps=None):
        bundled = {}
        if deps and os.path.isdir(deps):
            for dist in importlib.metadata.distributions(path=[deps]):
                bundled[_canonical(dist.name)] = dist
        entry = {"id": id, "name": name, "requirements": [], "problem": ""}
        try:
            for requirement in _manifest_requirements(texts):
                package = _canonical(requirement.name)
                if package in _PROVIDED_BY_APP:
                    continue
                copy = provided.get(package) or bundled.get(package)
                if copy is None or not meets(copy, requirement):
                    entry["requirements"].append(str(requirement))
        except InvalidRequirement as error:
            entry["problem"] = "An invalid requirement: %s" % error
        return entry

    requests = [request("", "My packages", user_packages.split())]
    for plugin in plugins:
        requests.append(request(plugin["id"], plugin["name"], plugin.get("requires", []), os.path.join(plugin["folder"], "deps")))
    return [r for r in requests if r["requirements"] or r["problem"]], sorted(_NEVER_INSTALLED)
