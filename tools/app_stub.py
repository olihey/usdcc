"""The stub of _usdcc_app, the module compiled into the app (docs/VSCODE.md section 4), from the
signatures pybind11 writes into its docstrings. Only the app, and usdcc_python_tests, have the
module, so the test app_stub_is_current runs this, and compares it with src/python/stubs/_usdcc_app.pyi.
To write that again after changing the bindings, run the test with USDCC_UPDATE_STUBS=1:

    build\\tests\\RelWithDebInfo\\usdcc_python_tests.exe app_stub
"""

from __future__ import annotations

import ast
import inspect
import re

HEADER = '''"""The running usdcc app, as it's compiled in. Use usdcc.app, usdcc.usd and usdcc.tools, not this.

Generated: see tools/app_stub.py.
"""

import collections.abc
import typing
from typing import Any
'''


def _signature(text, classes):
    """`text`, "(a: str) -> int", made readable to Python: what pybind11 can't name is Any."""
    text = re.sub(r"\b_usdcc_app\.", "", text)
    text = re.sub(r"<[^<>]*object at 0x[^<>]*>", "...", text)
    try:
        function = ast.parse(f"def f{text}: ...").body[0]
    except SyntaxError:
        # C++ types, such as "usdcc::ui::Busy", or a default that isn't Python.
        return None

    class Names(ast.NodeTransformer):
        def visit_Name(self, node):
            known = node.id in classes or node.id in BUILTINS
            return node if known else ast.copy_location(ast.Name("Any"), node)

        def visit_Attribute(self, node):
            root = node
            while isinstance(root, ast.Attribute):
                root = root.value
            if isinstance(root, ast.Name) and root.id in ("typing", "collections"):
                return _plain(node)
            return ast.copy_location(ast.Name("Any"), node)

    arguments = function.args
    for argument in [*arguments.posonlyargs, *arguments.args, *arguments.kwonlyargs, arguments.vararg, arguments.kwarg]:
        if argument is not None and argument.annotation is not None:
            argument.annotation = Names().visit(argument.annotation)
    if function.returns is not None:
        function.returns = Names().visit(function.returns)
    arguments.defaults = [value if isinstance(value, (ast.Constant, ast.UnaryOp)) else ast.Constant(...) for value in arguments.defaults]
    returns = f" -> {ast.unparse(function.returns)}" if function.returns is not None else ""
    return f"({ast.unparse(arguments)}){returns}".replace("int | int", "int")


BUILTINS = {"int", "float", "str", "bool", "bytes", "object", "None", "list", "dict", "tuple", "set", "type", "typing", "collections", "Any"}
# What pybind11 takes rather than what one passes: an int where it says SupportsInt.
PLAIN = {"typing.SupportsInt": "int", "typing.SupportsFloat": "float", "typing.SupportsIndex": "int"}


def _plain(node):
    name = PLAIN.get(ast.unparse(node))
    return ast.copy_location(ast.Name(name), node) if name else node


def _function(name, value, classes, indent, method=False):
    docs = (value.__doc__ or "").strip().split("\n")
    signatures = [line[line.index("("):] for line in docs if re.match(rf"(\d+\. )?{re.escape(name)}\(", line)]
    if len(signatures) > 1 and signatures[0].startswith("(*args"):
        signatures = signatures[1:]  # "Overloaded function."
    lines = []
    for text in signatures or ["(*args: Any, **kwargs: Any) -> Any"]:
        made = _signature(text, classes) or ("(self, *args: Any, **kwargs: Any) -> Any" if method else "(*args: Any, **kwargs: Any) -> Any")
        if len(signatures) > 1:
            lines.append(f"{indent}@typing.overload")
        lines.append(f"{indent}def {name}{made}: ...")
    return lines


def _class(name, cls, classes):
    lines = [f"class {name}:"]
    for member, value in sorted(vars(cls).items()):
        if member.startswith("_") and (member != "__init__" or "__init__(self" not in (value.__doc__ or "") or "*args, **kwargs" in value.__doc__):
            continue  # nothing Python can make has an __init__ of its own
        if isinstance(value, property):
            signature = _signature("(" + (value.fget.__doc__ or "").strip().split("\n")[0].partition("(")[2], classes) if value.fget else None
            returns = signature.rpartition(" -> ")[2] if signature and " -> " in signature else "Any"
            lines += ["    @property", f"    def {member}(self) -> {returns}: ..."]
        elif callable(value):
            lines += _function(member, value, classes, "    ", method=True)
        else:
            lines.append(f"    {member}: Any")
    return lines if len(lines) > 1 else [*lines, "    ..."]


def generate(module):
    # Not pybind11's loader, which is there too.
    classes = {name for name, value in vars(module).items() if inspect.isclass(value) and value.__module__ == module.__name__}
    lines = [HEADER]
    for name in sorted(classes):
        lines += [*_class(name, getattr(module, name), classes), ""]
    for name, value in sorted(vars(module).items()):
        if name.startswith("_") or inspect.isclass(value) or inspect.ismodule(value):
            continue
        lines += _function(name, value, classes, "") if callable(value) else [f"{name}: {type(value).__name__}"]
    return "\n".join(lines) + "\n"
