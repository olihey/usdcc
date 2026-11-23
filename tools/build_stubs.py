"""Makes build/stubs: types for editors (docs/VSCODE.md section 4), of exactly the pxr and PySide6
usdcc is built with. The build runs it, with OpenUSD's Python.

    python tools/build_stubs.py --out build/stubs --types-usd build/types-usd --pyside external/pyside-install

pxr has no signatures to read, so its stubs are types-usd's (MIT, of an older OpenUSD), with what
this OpenUSD no longer has taken out, and what it has gained put in by name, without types.
PySide6's come from its own generator. Also copies src/python/stubs (_usdcc_app).

A part that can't be made is left out, with a warning: completion lacks it, and nothing else does.
"""

from __future__ import annotations

import argparse
import ast
import importlib
import inspect
import keyword
import os
import pkgutil
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ANY_CALL = "(self, *args: Any, **kwargs: Any) -> Any: ..."
ANY_STATIC = "(*args: Any, **kwargs: Any) -> Any: ..."
# What stubs of types-usd's derive from: OpenUSD's own copy of Boost.Python makes it.
BOOST = "from typing import Any\n\nclass instance:\n    def __init__(self, *args: Any, **kwargs: Any) -> None: ...\n\nclass enum(int):\n    name: str\n    names: dict[str, Any]\n    values: dict[int, Any]\n"


def public(name):
    return not name.startswith("_") and name.isidentifier() and not keyword.iskeyword(name)


def member_lines(name, value, indent):
    """A member, by name."""
    if inspect.isclass(value):
        return class_lines(name, value, indent)
    if isinstance(value, staticmethod):
        return [f"{indent}@staticmethod", f"{indent}def {name}{ANY_STATIC}"]
    if isinstance(value, classmethod):
        return [f"{indent}@classmethod", f"{indent}def {name}(cls, *args: Any, **kwargs: Any) -> Any: ..."]
    if callable(value):
        return [f"{indent}def {name}{ANY_CALL}"]
    return [f"{indent}{name}: Any"]


def new_members(cls, known):
    """The members of `cls` that `known` lacks and it doesn't get from a base either."""
    inherited = [set(vars(base)) for base in cls.__mro__[1:] if base is not object]
    for name, value in sorted(vars(cls).items()):
        if public(name) and name not in known and not any(name in base for base in inherited):
            yield name, value


def class_lines(name, cls, indent):
    lines = [f"{indent}class {name}({base_name(cls)}):"]
    inner = indent + "    "
    lines.append(f"{inner}def __init__(self, *args: Any, **kwargs: Any) -> None: ...")
    for member, value in new_members(cls, set()):
        if value is not cls:
            lines += member_lines(member, value, inner)
    return lines


def base_name(cls):
    for base in cls.__bases__:
        if base.__module__.startswith("pxr.") and "." not in base.__qualname__ and public(base.__name__):
            # pxr.Usd._usd's classes are pxr.Usd's.
            package = ".".join(part for part in base.__module__.split(".") if not part.startswith("_"))
            if getattr(sys.modules.get(package), base.__name__, None) is base:
                return f"{package}.{base.__name__}"
    return "Any"


def import_lines(lines):
    """Imports for the bases that class_lines() named."""
    modules = {line.split("(")[1].rsplit(".", 1)[0] for line in lines if line.lstrip().startswith("class ") and "(pxr." in line}
    return [f"import {module}" for module in sorted(modules)]


def merge(text, module):
    """types-usd's stub of `module`, made to fit it."""
    tree = ast.parse(text)
    source = text.split("\n")
    drop = []  # (first, last) lines
    insert = {}  # after which line: what
    names = set()

    def names_of(node):
        if isinstance(node, (ast.ClassDef, ast.FunctionDef)):
            return [node.name]
        if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            return [node.target.id]
        if isinstance(node, ast.Assign):
            return [target.id for target in node.targets if isinstance(target, ast.Name)]
        return []

    def first_line(node):
        decorators = getattr(node, "decorator_list", [])
        return min([node.lineno] + [decorator.lineno for decorator in decorators])

    def walk(body, owner, indent, known):
        kept = 0
        for node in body:
            found = names_of(node)
            if found and all(public(name) and not hasattr(owner, name) for name in found):
                drop.append((first_line(node), node.end_lineno))
                continue
            kept += 1
            known.update(found)
            if isinstance(node, ast.ClassDef):
                actual = getattr(owner, node.name, None)
                if inspect.isclass(actual):
                    inner = set()
                    left = walk(node.body, actual, indent + "    ", inner)
                    added = []
                    for name, value in new_members(actual, inner):
                        if value is not actual:
                            added += member_lines(name, value, indent + "    ")
                    if not left and not added:
                        added = [indent + "    ..."]
                    insert[node.end_lineno] = added + insert.get(node.end_lineno, [])
        return kept

    walk(tree.body, module, "", names)
    added = []
    for name, value in sorted(vars(module).items()):
        if not public(name) or name in names or inspect.ismodule(value):
            continue
        if inspect.isclass(value):
            # Not what it imported from elsewhere.
            if (value.__module__ or "").startswith(module.__name__):
                added += class_lines(name, value, "")
        elif callable(value):
            added.append(f"def {name}{ANY_STATIC}")
        else:
            added.append(f"{name}: Any")
    dropped = {line for first, last in drop for line in range(first, last + 1)}
    result = []
    for number, line in enumerate(source, 1):
        if number not in dropped:
            result.append(line)
        result += insert.get(number, [])
    body = result + added
    header = []
    if "Any" not in {alias.asname or alias.name for node in tree.body if isinstance(node, ast.ImportFrom) for alias in node.names}:
        header.append("from typing import Any")
    header += import_lines(added + [line for lines in insert.values() for line in lines])
    merged = "\n".join(header + body) + "\n"
    ast.parse(merged)
    return merged, len(drop), sum(len(lines) for lines in insert.values()) + len(added)


def build_pxr(out, types_usd):
    import pxr

    modules = sorted(info.name for info in pkgutil.iter_modules(pxr.__path__) if info.ispkg and public(info.name))
    loaded = {}
    for name in modules:
        try:
            loaded[name] = importlib.import_module(f"pxr.{name}")
        except Exception as error:  # noqa: BLE001 - whatever it is, its stub is left out
            print(f"  pxr.{name} can't be imported: {error}")
    target = out / "pxr"
    target.mkdir(parents=True)
    (target / "__init__.pyi").write_text("", encoding="utf-8")
    gone = gained = 0
    for name, module in loaded.items():
        given = types_usd / "pxr-stubs" / name if types_usd else None
        folder = target / name
        folder.mkdir()
        text = ""
        if given and (given / "__init__.pyi").is_file():
            for file in given.glob("*.pyi"):
                shutil.copy2(file, folder)
            text = (given / "__init__.pyi").read_text(encoding="utf-8")
        merged, less, more = merge(text, module)
        (folder / "__init__.pyi").write_text(merged, encoding="utf-8")
        gone, gained = gone + less, gained + more
    (out / "Boost").mkdir()
    (out / "Boost/__init__.pyi").write_text("", encoding="utf-8")
    (out / "Boost/Python.pyi").write_text(BOOST, encoding="utf-8")
    print(f"  pxr: {len(loaded)} modules, {gone} entries of types-usd's taken out, {gained} lines put in")


def build_pyside(out, pyside):
    generator = pyside / "PySide6/support/generate_pyi.py"
    target = out / "PySide6"
    target.mkdir()
    environment = dict(os.environ, PYTHONPATH=str(pyside), QT_QPA_PLATFORM="offscreen", PYTHONDONTWRITEBYTECODE="1")
    made = subprocess.run([sys.executable, str(generator), "all", "--quiet", "--outpath", str(target)], env=environment,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
    stubs = list(target.glob("Qt*.pyi"))
    if made.returncode or not stubs:
        raise RuntimeError(made.stdout.decode("utf-8", "replace")[-2000:])
    (target / "__init__.pyi").write_text("", encoding="utf-8")
    shiboken = out / "shiboken6"
    shiboken.mkdir()
    for file in (pyside / "shiboken6").glob("*.pyi"):
        shutil.copy2(file, shiboken)
    (shiboken / "__init__.pyi").write_text("from .Shiboken import *\n", encoding="utf-8")
    print(f"  PySide6: {', '.join(sorted(stub.stem for stub in stubs))}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--types-usd", type=Path, help="types-usd's wheel, unpacked")
    parser.add_argument("--pyside", type=Path, help="the folder of PySide6 and shiboken6")
    parser.add_argument("--dlls", nargs="*", default=[], help="Windows: the folders of OpenUSD's DLLs")
    args = parser.parse_args()
    sys.dont_write_bytecode = True
    if args.dlls:
        folders = [os.path.abspath(folder) for folder in args.dlls]
        os.environ["PXR_USD_WINDOWS_DLL_PATH"] = os.pathsep.join(folders)  # pxr reads it
        os.environ["PATH"] = os.pathsep.join([*folders, os.environ.get("PATH", "")])

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.out.parent) as folder:
        new = Path(folder) / "stubs"
        new.mkdir()
        shutil.copytree(REPO / "src/python/stubs", new, dirs_exist_ok=True)
        for part, make in (("pxr", lambda: build_pxr(new, args.types_usd)), ("PySide6", lambda: build_pyside(new, args.pyside))):
            if part == "PySide6" and not args.pyside:
                continue
            try:
                make()
            except Exception as error:  # noqa: BLE001 - completion is all that needs it
                print(f"build_stubs.py: warning: no stubs for {part}: {error}")
                shutil.rmtree(new / part, ignore_errors=True)
        if args.types_usd and (new / "pxr").is_dir():
            # Its licence is stated there; the wheel has no file of its own.
            for metadata in args.types_usd.glob("types_usd-*.dist-info/METADATA"):
                shutil.copy2(metadata, new / "pxr/types-usd-METADATA.txt")
        if args.out.is_dir():
            shutil.rmtree(args.out)
        shutil.copytree(new, args.out)
    sys.stdout.flush()
    os._exit(0)  # OpenUSD's modules needn't be taken down


if __name__ == "__main__":
    main()
