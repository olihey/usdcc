"""usdcc.physics: a stage's rigid-body simulation (docs/PHYSICS.md).

The stage's UsdPhysics content (rigid bodies, colliders, a PhysicsScene) is solved
by the physics backend (Jolt, a bundled plugin) into a cache: an anonymous sublayer
of the session layer whose time samples the stage then plays. The user's layers
are never touched::

    import usdcc.physics

    usdcc.physics.simulate(1, 120)            # solve frames 1 to 120
    print(usdcc.physics.cached_range())       # (1.0, 120.0)
    for path, message in usdcc.physics.reports():
        print(path, message)                  # what wasn't simulated as authored

To keep a result, ``bake()`` writes its frames into the edit target as time samples
on the bodies' transforms; ``check()`` lists what OpenUSD's physics validators and a
simulation would say about the stage.

The timeline's Simulate does the same while it plays. Any edit to the stage makes
the cache stale (``is_stale()``): it stays, but is never extended; ``simulate()``
solves anew. The "physics_changed" event (``usdcc.app.connect()``) follows every
change to the cache.
"""

from usdcc import usd as _usd


def _manager_and_id(stage):
    return _usd.stage_manager(), _usd.stage_id(_usd._resolve(stage))


def simulate(start=None, end=None, stage=None):
    """Solves `stage` (default: the current stage) anew from the frame `start` to
    `end` (default: its playback range's first and last), into the cache. Returns
    False if it couldn't start: ``reports()`` says why."""
    manager, cache_id = _manager_and_id(stage)
    first, last = manager.playback_range(cache_id)
    start = first if start is None else float(start)
    end = last if end is None else float(end)
    return manager.physics_simulate(cache_id, start, end)


def reset(stage=None):
    """Clears the cache of `stage` (default: the current stage)."""
    manager, cache_id = _manager_and_id(stage)
    manager.physics_reset(cache_id)


def cached_range(stage=None):
    """``(first, last)``: the frames the cache holds, or None when it's empty."""
    manager, cache_id = _manager_and_id(stage)
    return manager.physics_cached_range(cache_id)


def is_stale(stage=None):
    """True when the stage changed after the cache was made."""
    manager, cache_id = _manager_and_id(stage)
    return manager.physics_is_stale(cache_id)


def reports(stage=None):
    """``[(path, message), ...]``: what the last simulation left out or changed,
    with the prim it's about."""
    manager, cache_id = _manager_and_id(stage)
    return manager.physics_reports(cache_id)


def bake(prims=None, mark_kinematic=False, stage=None):
    """Keeps the simulation of `stage` (default: the current stage): solves its playback
    range where the cache lacks frames, writes the frames of the rigid bodies at or under
    `prims` (default: all) into the edit target as time samples on their transforms, and
    clears the cache. With `mark_kinematic`, the baked bodies get
    physics:kinematicEnabled, so a later simulation moves them as baked. Returns the
    baked bodies' paths. A script run is one undo step already; elsewhere wrap it in
    ``usdcc.usd.edit()``."""
    manager, cache_id = _manager_and_id(stage)
    paths = [str(prim.GetPath()) if hasattr(prim, "GetPath") else str(prim) for prim in (prims or [])]
    return manager.physics_bake(cache_id, paths, bool(mark_kinematic))


def check(stage=None):
    """``[(path, message, source), ...]``: what OpenUSD's physics validators say about
    `stage` (default: the current stage), source "validator" or "validator error", and
    what a simulation of it would leave out or change, source "simulation"."""
    manager, cache_id = _manager_and_id(stage)
    return manager.physics_check(cache_id)
