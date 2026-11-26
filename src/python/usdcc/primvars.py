"""usdcc.primvars: what a prim's primvars are, and whether they're valid (docs/PRIMVAR_VIEWER.md).

``info()`` lists them, the inherited ones and the bound material's included, with where each
comes from, how many elements it has and should have, which materials read it, and what's
wrong::

    import usdcc.primvars as primvars

    found = primvars.info("/World/Cup")           # on the current stage; or a Usd.Prim
    for problem in found.all_problems():
        print(problem.severity, problem.message)
    st = found.get("st")                          # None if it has none
    print(st.interpolation, st.element_count, "of", st.expected_count, st.expected_of)

Provisional: these calls may still change, until the Primvars panel has used them.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from pxr import Sdf, Usd, UsdGeom, UsdShade

from usdcc import _plugins
from usdcc import usd as _usd

if TYPE_CHECKING:
    from collections.abc import Iterable

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    from usdcc import _usd as _native  # a plain Python; usdcc.usd above found its DLLs


@dataclass(frozen=True)
class Problem:
    """Something wrong with one primvar or several."""

    severity: str  # "error", "warning" or "info"
    code: str  # "ElementCount", "IndexRange", ...: doesn't change with the message
    message: str
    primvars: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class Primvar:
    """One primvar of a prim. Its name is without ``primvars:``."""

    name: str
    type: str  # "texCoord2f[]"
    interpolation: str  # "constant" unless authored
    interpolation_authored: bool
    element_size: int
    has_value: bool  # False: only declared, or blocked
    indexed: bool
    unauthored_values_index: int
    time_varying: bool
    value_count: int  # as authored
    element_count: int  # the indices, or the values over the element size
    expected_count: int | None  # what the interpolation needs; None where USD doesn't say
    expected_of: str  # what that's the count of: "points"
    source: str  # "local", "inherited" or "material"
    source_prim: Sdf.Path
    values_layer: str  # the identifiers of the layers with the strongest opinions
    indices_layer: str
    interpolation_layer: str
    id_target: Sdf.Path  # empty but for a string that's the path its relationship targets
    readers: list[Sdf.Path]  # the bound materials that read it
    derived_from: list[str]  # what it's usually worked out from
    problems: list[Problem]


@dataclass(frozen=True)
class Group:
    """Primvars that are one piece of data together, such as joint indices and weights."""

    id: str
    label: str
    members: list[str]
    missing: list[str]  # what should be there besides
    problems: list[Problem]


@dataclass(frozen=True)
class Missing:
    """What a bound material reads and the prim doesn't have."""

    name: str
    uv: bool
    materials: list[Sdf.Path]


@dataclass(frozen=True)
class PrimvarSet:
    """A prim's primvars, by name."""

    prim: Sdf.Path
    primvars: list[Primvar]
    groups: list[Group]
    missing: list[Missing]
    problems: list[Problem]  # about the prim rather than one primvar

    def get(self, name: str) -> Primvar | None:
        """The primvar with that name, or None."""
        return next((primvar for primvar in self.primvars if primvar.name == name), None)

    def all_problems(self) -> list[Problem]:
        """The prim's problems, its primvars' and its groups'."""
        result = list(self.problems)
        for part in [*self.primvars, *self.groups]:
            result += part.problems
        return result


def _prim(prim: Usd.Prim | Sdf.Path | str) -> Usd.Prim:
    if isinstance(prim, Usd.Prim):
        if not prim.IsValid():
            raise ValueError("the prim is gone")
        return prim
    stage = _usd.current_stage()
    if stage is None:
        raise RuntimeError("no stage is open")
    found = stage.GetPrimAtPath(str(prim))
    if not found:
        raise ValueError(f"there's no prim at {prim}")
    return found


def _time_code(time: Usd.TimeCode | float | None) -> Usd.TimeCode:
    return Usd.TimeCode.EarliestTime() if time is None else Usd.TimeCode(time)


def _seconds(time: Usd.TimeCode | float | None) -> float | None:
    code = _time_code(time)
    return None if code.IsDefault() else code.GetValue()


def _problems(dicts: Iterable[dict[str, Any]]) -> list[Problem]:
    return [Problem(**each) for each in dicts]


def _primvar(each: dict[str, Any]) -> Primvar:
    each["source_prim"] = Sdf.Path(each["source_prim"])
    each["id_target"] = Sdf.Path(each["id_target"]) if each["id_target"] else Sdf.Path()
    each["readers"] = [Sdf.Path(path) for path in each["readers"]]
    each["problems"] = _problems(each["problems"])
    return Primvar(**each)


def info(prim: Usd.Prim | Sdf.Path | str, time: Usd.TimeCode | float | None = None, inherited: bool = True, readers: bool = True,
         scan: bool = True) -> PrimvarSet:
    """A prim's primvars with an opinion (a schema's fallback isn't one), at `time`, the
    earliest by default. `prim` is a ``Usd.Prim`` of any stage, or a path on the current one.

    ``inherited=False`` leaves out inherited primvars and the bound material's,
    ``readers=False`` what materials read, and ``scan=False`` the look at every index and
    value: for many prims, or big ones."""
    found = _prim(prim)
    result = _native.primvar_info(found.GetStage(), str(found.GetPath()), _seconds(time), inherited, readers, scan)
    return PrimvarSet(
        prim=Sdf.Path(result["prim"]),
        primvars=[_primvar(each) for each in result["primvars"]],
        groups=[Group(**{**each, "problems": _problems(each["problems"])}) for each in result["groups"]],
        missing=[Missing(each["name"], each["uv"], [Sdf.Path(path) for path in each["materials"]]) for each in result["missing"]],
        problems=_problems(result["problems"]))


def values(prim: Usd.Prim | Sdf.Path | str, name: str, time: Usd.TimeCode | float | None = None, flattened: bool = False) -> Any:
    """A primvar's value as USD has it (a ``Vt`` array, which numpy reads whole), or None
    if there's none: the prim's own, else an inherited one, else the bound material's.
    ``flattened=True`` gives one value for each index. Raises ValueError if its indices
    are outside its values."""
    found = _prim(prim)
    primvar = UsdGeom.PrimvarsAPI(found).FindPrimvarWithInheritance(name)
    if not primvar or not primvar.HasAuthoredValue():
        primvar = None
        if found.IsA(UsdGeom.Gprim):
            for purpose in (UsdShade.Tokens.allPurpose, UsdShade.Tokens.preview, UsdShade.Tokens.full):
                material, _ = UsdShade.MaterialBindingAPI(found).ComputeBoundMaterial(purpose)
                own = UsdGeom.PrimvarsAPI(material.GetPrim()).GetPrimvar(name) if material else None
                if own and own.HasAuthoredValue():
                    primvar = own
                    break
    if primvar is None:
        return None
    code = _time_code(time)
    if not flattened or not primvar.IsIndexed():
        return primvar.Get(code)
    result = primvar.ComputeFlattened(code)
    if result is None:
        raise ValueError(f"the indices of {name} on {found.GetPath()} are outside its values")
    return result


def element_count(prim: Usd.Prim | Sdf.Path | str, interpolation: str, time: Usd.TimeCode | float | None = None) -> int | None:
    """How many elements an interpolation has on a prim: a mesh's points for "vertex". None
    where USD doesn't say, as for a Cube's."""
    found = _prim(prim)
    return _native.primvar_element_count(found.GetStage(), str(found.GetPath()), str(interpolation), _seconds(time))


def focus(stage: Usd.Stage | None = None) -> str:
    """The primvar the Primvars panel and its views look at on `stage` (default: the current
    stage), without ``primvars:``; "" for none. Every panel on the stage shares it."""
    return _usd.stage_manager().primvar_focus(_usd.stage_id(_usd._resolve(stage)))


def set_focus(name: str, stage: Usd.Stage | None = None) -> None:
    """Puts the primvar `name` in focus on `stage` (default: the current stage): the Primvars
    panel picks its row and the views show it. "" for none."""
    _usd.stage_manager().set_primvar_focus(_usd.stage_id(_usd._resolve(stage)), str(name))


def register_relation(id: str, label: str, members: Iterable[str], complete: bool = False, same_interpolation: bool = False,
                      same_element_size: bool = False, derived_from: Iterable[str] = ()) -> None:
    """Says that primvars belong together, or what they're derived from. `members` are names
    without ``primvars:``, and a * at the end stands for any text ("groom:*"). With
    `complete`, a prim that has one should have all of them; `same_interpolation` and
    `same_element_size` say that those have to agree. Registering an id again replaces it,
    a built-in one ("skel") too. A plugin's goes when it reloads. For a check of your own,
    write a rule (``usdcc.validation``)."""
    _native.set_primvar_relation(id, label, list(members), complete, same_interpolation, same_element_size, list(derived_from))
    _plugins.note("Primvar relation", label or id)
    _plugins.track(lambda: remove_relation(id))


def remove_relation(id: str) -> bool:
    """Removes a relation; False if there was none."""
    return _native.remove_primvar_relation(id)


def relations() -> list[dict[str, Any]]:
    """Every relation (dicts), the built-in ones included."""
    return _native.primvar_relations()
