"""usdcc.validation: rules of your own for the Validation panel (docs/VALIDATION.md section 5).

    import usdcc.validation as v

    @v.rule("studio.mesh_names", "Mesh names", types=["Mesh"])
    def mesh_names(prim, report, prefix="geo_"):
        \"""Meshes start with the studio's prefix.\"""
        if not prim.GetName().startswith(prefix):
            report.warn(prim, "The name should start with " + prefix)

A file like this in the rules folder (the Validation panel's New Rule makes one) is
loaded when the app starts and again when it's saved. A plugin decorates its rules, or
imports the module that does, in its activate().

Rules are checked together with OpenUSD's, on many threads at once. So a rule only
reads: no edits, no Qt, nothing of usdcc.app. Names set at a file's top level are shared
by those threads. ``main_thread=True`` runs a rule on the main thread instead, slower, for
what must be called there.

A rule that raises doesn't end the run. Each failure is a finding, and after
``MAX_FAILURES`` of them the rule isn't called again in that run.

In a plain python, rules run through OpenUSD's own ``UsdValidation.ValidationContext``.
"""

import hashlib
import inspect
import os
import sys
import threading
import types as _types

from pxr import Sdf, Tf, Usd
from pxr import UsdValidation as _UsdValidation

from usdcc import _plugins
from usdcc import log as _log
from usdcc.usd import _native

MAX_FAILURES = 10

_rules = {}  # id: Rule, those there now
_slots = {}  # id: every _Slot ever registered with OpenUSD for it, which can't be taken back
_files = {}  # rules file: _File
_loading = None  # the rules file being run
_rules_lock = threading.Lock()


class Report:
    """What a rule tells its findings to. ``site`` is what's wrong: a prim, a property,
    the stage, a layer, a spec in one, or a ``(layer, path)`` pair. ``name`` tells one of
    the rule's findings from another, such as "TooLong"; it's the rule's own by default."""

    def __init__(self, name):
        self._name = name
        self._errors = []

    def error(self, site, message, name=None):
        self._add(_UsdValidation.ValidationErrorType.Error, site, message, name)

    def warn(self, site, message, name=None):
        self._add(_UsdValidation.ValidationErrorType.Warn, site, message, name)

    def info(self, site, message, name=None):
        self._add(_UsdValidation.ValidationErrorType.Info, site, message, name)

    def _add(self, kind, site, message, name):
        self._errors.append(_UsdValidation.ValidationError(name or self._name, kind, [_site(site)], str(message)))


def _site(site):
    if isinstance(site, Usd.Stage):
        return _UsdValidation.ValidationErrorSite(site, Sdf.Path.absoluteRootPath)
    if isinstance(site, Usd.SchemaBase):
        site = site.GetPrim()
    if isinstance(site, (Usd.Prim, Usd.Property)):
        return _UsdValidation.ValidationErrorSite(site.GetStage(), site.GetPath())
    if isinstance(site, Sdf.Layer):
        return _UsdValidation.ValidationErrorSite(site, Sdf.Path.absoluteRootPath)
    if isinstance(site, Sdf.Spec):
        return _UsdValidation.ValidationErrorSite(site.layer, site.path)
    if isinstance(site, tuple) and len(site) == 2:
        return _UsdValidation.ValidationErrorSite(site[0], Sdf.Path(site[1]))
    raise TypeError("a finding is at a prim, property, stage, layer, spec or (layer, path), not a %s" % type(site).__name__)


class Rule:
    """A rule, which its decorator returns in place of the function. Calling it calls
    the function. ``parameters`` are the function's keyword arguments' defaults."""

    def __init__(self, kind, function, id, label, schema_types, keywords, time_dependent, main_thread):
        self.kind = kind
        self.function = function
        self.id = id
        self.label = label
        self.doc = inspect.cleandoc(function.__doc__ or "") or label
        self.schema_types = schema_types
        self.keywords = keywords
        self.time_dependent = time_dependent
        self.main_thread = main_thread
        self.file = _loading or _source_file(function)
        self.parameters = _parameters(function, time_dependent)
        self._failures = 0
        self._failed_run = None
        self._lock = threading.Lock()
        self.__name__ = getattr(function, "__name__", id)
        self.__doc__ = function.__doc__
        self.__module__ = getattr(function, "__module__", None)

    def __call__(self, *args, **kwargs):
        return self.function(*args, **kwargs)

    def __repr__(self):
        return "<usdcc.validation.Rule %s>" % self.id

    def _run(self, subject, times):
        if self._failed_run is not None and self._gave_up():
            return []
        report = Report(self.id.rsplit(".", 1)[-1])
        arguments = dict(self.parameters)
        if self.time_dependent:
            arguments["times"] = times
        try:
            self.function(subject, report, **arguments)
        except (Exception, SystemExit) as error:
            # What it reported before it raised counts.
            return report._errors + self._failed(subject, error)
        return report._errors

    def _gave_up(self):
        with self._lock:
            return self._failed_run == _native.validation_run() and self._failures >= MAX_FAILURES

    def _failed(self, subject, error):
        run = _native.validation_run()
        with self._lock:
            if self._failed_run != run:
                self._failed_run, self._failures = run, 0
            self._failures += 1
            count = self._failures
        if count > MAX_FAILURES:
            return []  # on other threads when the last one counted
        if count == 1:
            _log.log_exception(type(error), error, error.__traceback__, "usdcc.validation")
        message = "The rule failed: %s: %s" % (type(error).__name__, error)
        if count == MAX_FAILURES:
            message += " (it isn't run any further)"
        return [
            _UsdValidation.ValidationError(
                "RuleFailed", _UsdValidation.ValidationErrorType.Error, [_site(subject)], message
            )
        ]


def _source_file(function):
    code = getattr(inspect.unwrap(function), "__code__", None)
    return os.path.abspath(code.co_filename) if code and os.path.isfile(code.co_filename) else ""


def _parameters(function, time_dependent):
    """The defaults of what comes after the subject and the report."""
    parameters = list(inspect.signature(function).parameters.values())[2:]
    result = {}
    for parameter in parameters:
        if parameter.kind in (parameter.VAR_POSITIONAL, parameter.VAR_KEYWORD):
            continue
        if time_dependent and parameter.name == "times":
            continue
        if parameter.default is parameter.empty:
            raise TypeError("the rule's argument %s needs a default, as nothing would give it a value" % parameter.name)
        result[parameter.name] = parameter.default
    return result


def _type_name(type):
    """The TfType's name for "Mesh", "UsdGeomMesh" or UsdGeom.Mesh."""
    if isinstance(type, str):
        found = Usd.SchemaRegistry.GetTypeFromName(type)
        if not found:
            found = Tf.Type.FindByName(type)
    else:
        found = Tf.Type.Find(type)
    if not found or not Usd.SchemaRegistry().GetSchemaTypeName(found):
        raise ValueError("USD has no schema %s" % (type,))
    return found.typeName


# --- Registering ---------------------------------------------------------------


class _Slot:
    """What OpenUSD has under one name, forever: it runs the rule that's in it, if any."""

    def __init__(self, name, signature):
        self.name = name
        self.signature = signature
        self.rule = None

    def run(self, subject, times=None):
        rule = self.rule
        return rule._run(subject, times) if rule is not None else []


def _signature(rule):
    return (rule.kind, tuple(rule.schema_types), tuple(rule.keywords), rule.time_dependent)


def _slot_for(rule):
    slots = _slots.setdefault(rule.id, [])
    signature = _signature(rule)
    for slot in slots:
        if slot.signature == signature:
            return slot
    # A name's metadata can't be changed, so a changed one needs another name.
    slot = _Slot("usdcc:%s#%d" % (rule.id, len(slots) + 1), signature)
    metadata = _UsdValidation.ValidatorMetadata(
        name=slot.name,
        doc=rule.doc,
        keywords=list(rule.keywords),
        schemaTypes=list(rule.schema_types),
        isTimeDependent=rule.time_dependent,
    )
    registry = _UsdValidation.ValidationRegistry()
    if rule.kind == "prim":
        registry.RegisterPrimValidator(metadata, slot.run)
    elif rule.kind == "stage":
        registry.RegisterStageValidator(metadata, slot.run)
    else:
        registry.RegisterLayerValidator(metadata, slot.run)
    slots.append(slot)
    return slot


def _add(rule):
    with _rules_lock:
        old = _rules.get(rule.id)
        if old is not None and _normal(old.file) != _normal(rule.file):
            _log.post(
                _log.WARNING,
                "usdcc.validation",
                "The rule %s in %s replaces the one in %s" % (rule.id, rule.file or "?", old.file or "?"),
            )
        slot = _slot_for(rule)
        for other in _slots[rule.id]:
            other.rule = None
        slot.rule = rule
        _rules[rule.id] = rule
    _native.validation_set_rule(
        rule.id,
        slot.name,
        rule.label,
        rule.doc,
        list(rule.keywords),
        list(rule.schema_types),
        rule.file,
        rule.main_thread,
    )
    if _loading is not None:
        _files[_loading].rules.append(rule)
    elif _plugins.current() is not None:
        _plugins.note("Validation rule", rule.label)
        _plugins.track(lambda: _remove(rule))
    return rule


def _remove(rule):
    """Takes the rule away, unless another has its id by now."""
    with _rules_lock:
        if _rules.get(rule.id) is not rule:
            return False
        del _rules[rule.id]
        for slot in _slots.get(rule.id, []):
            slot.rule = None
    _native.validation_remove_rule(rule.id)
    return True


def _decorator(kind, id, label, types, keywords, time_dependent, main_thread):
    if not isinstance(id, str) or not id or not all(c.isalnum() or c in "._-" for c in id):
        raise ValueError("a rule's id is letters, digits, '.', '_' and '-', such as studio.mesh_names")
    if label is not None and not isinstance(label, str):
        raise TypeError("a rule's label is text: @rule(\"studio.mesh_names\", \"Mesh names\")")
    if isinstance(types, str) or isinstance(keywords, str):
        raise TypeError("types and keywords are lists")
    schema_types = [_type_name(type) for type in types]

    def decorate(function):
        return _add(
            Rule(kind, function, id, label or id, schema_types, list(keywords), bool(time_dependent), bool(main_thread))
        )

    return decorate


def rule(id, label=None, types=(), keywords=(), time_dependent=False, main_thread=False):
    """Makes a function a rule on prims: ``function(prim, report)``, or on those of
    `types` (schemas, by name or class: "Mesh", "MaterialBindingAPI", UsdGeom.Mesh; none
    means every prim). Its other arguments need defaults and are its parameters. A rule
    with the same `id` is replaced. The docstring says what the rule is for.
    ``time_dependent``: the function also gets ``times``, OpenUSD's ValidationTimeRange."""
    return _decorator("prim", id, label, types, keywords, time_dependent, main_thread)


def stage_rule(id, label=None, keywords=(), time_dependent=False, main_thread=False):
    """Makes a function a rule on the stage: ``function(stage, report)``."""
    return _decorator("stage", id, label, (), keywords, time_dependent, main_thread)


def layer_rule(id, label=None, keywords=(), main_thread=False):
    """Makes a function a rule on each layer the stage uses: ``function(layer, report)``."""
    return _decorator("layer", id, label, (), keywords, False, main_thread)


def rules():
    """The rules made with this module that are there now, by id."""
    with _rules_lock:
        return dict(sorted(_rules.items()))


def remove_rule(id):
    """Removes a rule made with this module; False if there's none."""
    found = rules().get(id)
    return found is not None and _remove(found)


# --- Rules files ---------------------------------------------------------------


class _File:
    def __init__(self, digest):
        self.digest = digest
        self.rules = []
        self.problem = ""
        self.module = ""


def _normal(path):
    return os.path.normcase(os.path.abspath(path)) if path else ""


def _read(path):
    try:
        with open(path, "rb") as file:
            return file.read()
    except OSError:
        return None  # gone, or being written: the next sync has it


def _unload(path):
    for old in reversed(_files[path].rules):
        _remove(old)
    sys.modules.pop(_files[path].module, None)
    del _files[path]


def _load(path, source, digest):
    global _loading
    file = _files[path] = _File(digest)
    file.module = "usdcc_rules_" + hashlib.sha1(_normal(path).encode()).hexdigest()[:12]
    module = _types.ModuleType(file.module)
    module.__file__ = path
    sys.modules[file.module] = module
    previous, _loading = _loading, path
    try:
        # Compiled here: an import would keep a .pyc, which a quick edit of the same
        # length doesn't replace.
        exec(compile(source, path, "exec"), module.__dict__)
    except (Exception, SystemExit) as error:
        _log.log_exception(type(error), error, error.__traceback__, "usdcc.validation")
        file.problem = "%s: %s" % (type(error).__name__, error)
        # All of a file's rules or none.
        for old in reversed(file.rules):
            _remove(old)
        file.rules = []
    finally:
        _loading = previous


def sync_files(folders):
    """Loads the rules files (``*.py``, but for those starting with ``_``) in `folders`:
    the new and the changed ones, taking back the rules of those that are gone. The app
    calls it at start and when a file changes. Returns whether anything changed."""
    found = {}
    for folder in folders:
        try:
            names = sorted(os.listdir(folder), key=str.lower)
        except OSError:
            continue
        for name in names:
            path = os.path.join(os.path.abspath(folder), name)
            if name.endswith(".py") and not name.startswith("_") and os.path.isfile(path):
                found.setdefault(_normal(path), path)
    paths = set(found.values())
    changed = False
    for path in [path for path in _files if path not in paths]:
        _unload(path)
        changed = True
    for path in found.values():
        source = _read(path)
        if source is None:
            continue
        digest = hashlib.sha1(source).hexdigest()
        if path in _files:
            if _files[path].digest == digest:
                continue
            _unload(path)
        _load(path, source, digest)
        changed = True
    if changed:
        _native.validation_set_problems([(path, file.problem) for path, file in _files.items() if file.problem])
    return changed
