"""usdcc.usd: the app's stages, selection and undo history (docs/SCRIPTING.md section 4).

Inside usdcc, this works on the running app: stages come back as pxr
``Usd.Stage`` objects, so a script uses OpenUSD's own Python API on them, and
the Outliner, Viewport and Edit > Undo see every change::

    from pxr import Sdf
    import usdcc.usd

    stage = usdcc.usd.open_stage("C:/shots/a.usda")   # like File > Open Stage
                                                      # (or current_stage())
    cube = stage.DefinePrim("/World/Cube", "Cube")
    usdcc.usd.select([cube])

    with usdcc.usd.edit("Resize cube"):               # one undo step
        cube.GetAttribute("size").Set(2.0)

A script run from the app is one undo step already; ``edit()`` is for code
that isn't (startup scripts, plugins) or wants a named step of its own.

In a plain python there is no app: ``StageManager`` is a stage manager of your
own (from usdcc/_usd.pyd), and the module-level functions below raise
RuntimeError.
"""

from contextlib import contextmanager

from pxr import Usd, UsdUtils

try:
    import _usdcc_app as _native  # compiled into the app

    EMBEDDED = True
except ImportError:
    EMBEDDED = False
    # _usd.pyd needs OpenUSD's DLLs, which Python 3.8+ only finds in folders
    # added with os.add_dll_directory; pxr's own import helper adds the PATH
    # folders for the duration of the import.
    from pxr import Tf as _Tf

    _wrapper = getattr(_Tf, "WindowsImportWrapper", None)
    if _wrapper is not None:
        with _wrapper():
            from usdcc import _usd as _native
    else:
        from usdcc import _usd as _native

StageManager = _native.StageManager
if EMBEDDED:
    UndoManager = _native.UndoManager


def _app():
    if not EMBEDDED:
        raise RuntimeError("this needs the running usdcc app; in a plain python, create a usdcc.usd.StageManager()")
    return _native


def stage_manager():
    """The app's StageManager."""
    return _app().stage_manager()


def undo_manager():
    """The app's UndoManager."""
    return _app().undo_manager()


def _stage(cache_id):
    if cache_id < 0:
        return None
    return UsdUtils.StageCache.Get().Find(Usd.StageCache.Id.FromLongInt(cache_id))


def stage_id(stage):
    """The id the app knows `stage` by (its UsdUtils.StageCache id)."""
    cache_id = UsdUtils.StageCache.Get().GetId(stage)
    if not cache_id.IsValid():
        raise ValueError("this stage isn't open in usdcc (it isn't in UsdUtils.StageCache)")
    return cache_id.ToLongInt()


def stages():
    """Every open stage, in the order they were opened."""
    return [_stage(cache_id) for cache_id in stage_manager().stage_cache_ids()]


def current_stage():
    """The current stage, or None if no stage is open: the stage of the view
    panel last focused (or the one set_current_stage() set since)."""
    return _stage(stage_manager().current_stage_cache_id())


def set_current_stage(stage):
    stage_manager().set_current_stage(stage_id(stage))


def open_stage(path):
    """Opens `path` in the app, like File > Open Stage. As there, a file that
    is already open is opened again, as a second stage on the same layers;
    use stages() or current_stage() to work on an open one."""
    cache_id = stage_manager().open_stage(str(path))
    if cache_id < 0:
        raise RuntimeError(f"could not open {path} (the Log panel says why)")
    return _stage(cache_id)


def close_stage(stage):
    """Closes `stage` in the app, with every panel showing it."""
    stage_manager().close_stage(stage_id(stage))


def _resolve(stage):
    stage = stage if stage is not None else current_stage()
    if stage is None:
        raise RuntimeError("no stage is open")
    return stage


def _path_string(item):
    get_path = getattr(item, "GetPath", None)  # a prim or property
    return str(get_path() if get_path else item)


def selected_paths(stage=None):
    """The selected prims' paths (Sdf.Path) on `stage` (default: the current stage)."""
    from pxr import Sdf

    stage = _resolve(stage)
    return [Sdf.Path(path) for path in stage_manager().selected_paths(stage_id(stage))]


def select(items, stage=None):
    """Selects prims, given as prims, Sdf.Paths or path strings, on `stage`
    (default: the current stage). The Outliner and the Viewport follow."""
    stage = _resolve(stage)
    stage_manager().set_selected_paths(stage_id(stage), [_path_string(item) for item in items])


def edit_target(stage=None):
    """The layer (Sdf.Layer) edits to `stage` (default: the current stage) go to:
    its edit target, which the Layers panel shows and sets."""
    return _resolve(stage).GetEditTarget().GetLayer()


def set_edit_target(layer, stage=None):
    """Makes `layer` (an Sdf.Layer or its identifier) the edit target of `stage`
    (default: the current stage), as the Layers panel does. It must be in the
    stage's layer stack, session layers included, and not muted; ValueError
    otherwise. Not an undo step."""
    from pxr import Sdf

    stage = _resolve(stage)
    found = layer if isinstance(layer, Sdf.Layer) else Sdf.Layer.Find(str(layer))
    if not found or not stage.HasLocalLayer(found):
        raise ValueError("not a layer of the stage's layer stack: %r" % (layer,))
    if stage.IsLayerMuted(found.identifier):
        raise ValueError("the layer is muted: %s" % found.identifier)
    stage.SetEditTarget(Usd.EditTarget(found))


def set_relationship_targets(relationship, types=(), schemas=(), severity="block", single=False, reason=""):
    """Says what a relationship expects to target (docs/RELATIONSHIPS.md), for the
    Attribute Editor's target dialog. USD doesn't record it, so this is how a plugin
    adds its own. `relationship` is a name, with at most one * for any text; a rule
    for the same one is replaced, so a script can run again, and a built-in rule can
    be overridden. A target is fine if it's one of `types` (or derives from one) or
    has one of `schemas`. With severity "block" others are greyed out, with "warn"
    they can be picked but are marked. `single`: it holds one target. `reason` is the
    tooltip. Takes effect the next time the dialog opens."""
    _native.set_relationship_targets(relationship, list(types), list(schemas), severity, single, reason)


def remove_relationship_targets(relationship):
    """Removes that rule; False if there was none."""
    return _native.remove_relationship_targets(relationship)


def relationship_targets(relationship):
    """The rule (a dict) that applies to a relationship's name, or None."""
    return _native.relationship_targets(relationship)


def relationship_target_rules():
    """Every rule (dicts), the built-in ones included."""
    return _native.relationship_target_rules()


def set_create_entry(type, category="Plugins", group=0, order=1000):
    """Says where the Create menu lists a prim type (docs/CREATE_MENU.md), which a type
    with no entry finds in Plugins. An entry for the same type is replaced, so a script
    can run again, and a built-in one can be overridden. A new `category` becomes a
    submenu; `group`s in it are separated by a line, `order` sorts within one, then
    the name. Either spelling of a versioned type names its family. A type USD doesn't
    know is left out until its plugin has loaded. Takes effect the next time the menu
    opens."""
    _native.set_create_entry(type, category, group, order)


def hide_create_entry(type):
    """Leaves a prim type out of the Create menu."""
    _native.hide_create_entry(type)


def remove_create_entry(type):
    """Forgets that entry, so the type goes to Plugins; False if there was none."""
    return _native.remove_create_entry(type)


def create_entries():
    """What the Create menu lists now (dicts), in its order, leaving out Prim (no type)."""
    return _native.create_entries()


@contextmanager
def edit(description, stage=None):
    """Makes everything done to `stage` (default: the current stage) inside the
    block one undo step named `description`. Nests: inside a script run, or
    another edit(), it becomes part of the outer step. Changes made before an
    exception stay, as the step."""
    stage = _resolve(stage)
    cache_id = stage_id(stage)
    undo_manager().begin_edit(cache_id, description)
    try:
        yield stage
    finally:
        undo_manager().end_edit(cache_id)


def undo(stage=None):
    """Edit > Undo on `stage` (default: the current stage)."""
    undo_manager().undo(stage_id(_resolve(stage)))


def redo(stage=None):
    """Edit > Redo on `stage` (default: the current stage)."""
    undo_manager().redo(stage_id(_resolve(stage)))


def can_undo(stage=None):
    return undo_manager().can_undo(stage_id(_resolve(stage)))


def can_redo(stage=None):
    return undo_manager().can_redo(stage_id(_resolve(stage)))


def undo_text(stage=None):
    """What Undo would revert on `stage`, e.g. "Run shot_setup.py" ('' if nothing)."""
    return undo_manager().undo_text(stage_id(_resolve(stage)))
