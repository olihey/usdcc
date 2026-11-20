"""Tries a freshly generated schema plugin in a process of its own (docs/SCHEMAS.md §5.3).

    python -m usdcc._schema_check <usd folder>

Registers the plugin in <usd folder>, then makes a prim of each prim type and
applies each API schema, as a user would. Prints one line of JSON:
{"schemas": [...], "errors": [...]}. A schema registered after a stage has used
the registry would only half work, which is why this runs in its own process.
Imports nothing of usdcc's but this module, so it runs in a plain Python with pxr.
"""

import json
import sys


def main(folder):
    report = {"schemas": [], "errors": []}

    def error(message):
        report["errors"].append(" ".join(str(message).split()))

    from pxr import Plug, Tf, Usd

    try:
        plugins = Plug.Registry().RegisterPlugins(folder)
    except Tf.ErrorException as e:
        error(e)
        plugins = []
    if not plugins:
        error("No plugin registered from " + folder)
        return report

    schemas = []
    for plugin in plugins:
        for info in plugin.metadata.get("Types", {}).values():
            if "schemaIdentifier" in info:
                schemas.append((info["schemaIdentifier"], info.get("schemaKind", "")))
    try:
        # A name another plugin has already fails here ("Cannot set alias").
        stage = Usd.Stage.CreateInMemory()
    except Tf.ErrorException as e:
        error(e)
        return report

    registry = Usd.SchemaRegistry()
    for index, (name, kind) in enumerate(sorted(schemas)):
        report["schemas"].append(name)
        try:
            if kind == "concreteTyped":
                if not registry.FindConcretePrimDefinition(name):
                    error(f"{name}: USD has no definition for it")
                    continue
                prim = stage.DefinePrim(f"/Type{index}", name)
                if not prim or prim.GetPrimTypeInfo().GetSchemaTypeName() != name:
                    error(f"{name}: a prim of it doesn't compose as one")
            elif kind == "abstractTyped":
                if not registry.FindAbstractPrimDefinition(name):
                    error(f"{name}: USD has no definition for it")
            elif kind in ("singleApplyAPI", "multipleApplyAPI"):
                if not registry.FindAppliedAPIPrimDefinition(name):
                    error(f"{name}: USD has no definition for it")
                    continue
                # On a prim of a type it can apply to.
                allowed = Usd.SchemaRegistry.GetAPISchemaCanOnlyApplyToTypeNames(name)
                prim = stage.DefinePrim(f"/Api{index}", allowed[0] if allowed else "")
                instance = "check" if kind == "multipleApplyAPI" else ""
                applied = prim.ApplyAPI(name, instance) if instance else prim.ApplyAPI(name)
                if not applied:
                    error(f"{name}: applying it failed")
        except Tf.ErrorException as e:
            error(f"{name}: {e}")
    return report


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: python -m usdcc._schema_check <usd folder>")
    print(json.dumps(main(sys.argv[1])))
