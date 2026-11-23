"""V-Ray's shader nodes for the Material Editor (docs/MATERIAL_EDITOR.md section 3.4,
docs/VRAY_RENDER.md section 8).

V-Ray's USD bundle registers no shader definitions, so the editor would know no V-Ray
node. This makes them: V-Ray's plugininfo.exe describes every plugin as JSON, without
a license, and ``generate()`` turns the BRDFs, materials, textures and UVW generators
into one .usda of shader definitions, as USD's own shaderDefs.usda is written.
``register()`` then hands each to Sdr, with the identifier V-Ray's USD reader takes:

    def Shader "Mtl"
    {
        uniform token info:id = "vray:BRDFVRayMtl"
        color3f inputs:diffuse.connect = </Looks/Mat/Checker.outputs:out>
        float inputs:reflect_glossiness.connect = </Looks/Mat/Checker.outputs:out_alpha>
        token outputs:out
    }

on a Material's ``outputs:vray:surface``. An input is the V-Ray parameter of its name,
``outputs:out`` is the plugin itself, and another output the V-Ray output parameter of
its name (``Checker::out_alpha``). A BRDF on the terminal is wrapped in a
MtlSingleBRDF by V-Ray.

plugininfo takes half a minute, so the file is cached per V-Ray build in the user data
folder (``ShaderCache``) and made in a thread the first time.
"""

import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import tempfile

SYSTEM = "vray"
PREFIX = SYSTEM + ":"
FILE_NAME = "vray_shaders.usda"
DEFAULT_SURFACE = PREFIX + "BRDFVRayMtl"

# Bumped when what generate() writes changes, so cached files are made again.
FORMAT = 1

# plugininfo's JSON folders that hold shader nodes, and the palette group of each. The
# rest (lights, geometry, settings, render channels, ...) aren't shading networks.
FOLDERS = {"BRDF": "BRDF", "material": "Material", "uvwgen": "UVW", "textures": "Texture"}

# plugininfo -listByCategory's texture categories: the palette group, and what the
# plugin itself is as a connection's source. In this order when a plugin is in several.
TEXTURE_CATEGORIES = [
    ("Bitmaps", "Bitmap", "token"),
    ("Textures", "Texture", "color3f"),
    ("Textures Float", "Float Texture", "float"),
    ("Textures Vector", "Vector Texture", "float3"),
    ("Textures Int", "Int Texture", "int"),
    ("Textures Transform", "Transform Texture", "matrix4d"),
    ("Texture Matrix", "Matrix Texture", "matrix4d"),
]

# V-Ray parameter type: USD type, and whether a texture can drive it. A V-Ray parameter
# that isn't a texture or a plugin takes a value only. Sdr has no 3x3 matrix, so a
# MATRIX is a matrix4d, as a TRANSFORM is, with no translation.
TYPES = {
    "BOOL": ("bool", False),
    "INT": ("int", False),
    "ENUM": ("int", False),
    "FLOAT": ("float", False),
    "COLOR": ("color3f", False),
    "ACOLOR": ("color4f", False),
    "VECTOR": ("float3", False),
    "STRING": ("string", False),
    "TRANSFORM": ("matrix4d", False),
    "MATRIX": ("matrix4d", False),
    "PLUGIN": ("token", True),
    "TEXTURE": ("color3f", True),
    "FLOAT_TEXTURE": ("float", True),
    "INT_TEXTURE": ("int", True),
    "VECTOR_TEXTURE": ("float3", True),
    "TRANSFORM_TEXTURE": ("matrix4d", True),
    "MATRIX_TEXTURE": ("matrix4d", True),
    "BOOL_LIST": ("bool[]", False),
    "INT_LIST": ("int[]", False),
    "FLOAT_LIST": ("float[]", False),
    "COLOR_LIST": ("color3f[]", False),
    "ACOLOR_LIST": ("color4f[]", False),
    "VECTOR_LIST": ("float3[]", False),
    "STRING_LIST": ("string[]", False),
    "TRANSFORM_LIST": ("matrix4d[]", False),
    "MATRIX_LIST": ("matrix4d[]", False),
}

# Lists whose elements are connections. V-Ray's USD reader takes them as
# inputs:<list>_count and one input each, from 1: brdfs_count = 2, brdfs_1, brdfs_2.
# Without the count the list is empty, so the V-Ray panel's export fills it in
# (fill_list_counts()). The definitions have SLOTS of them.
SLOT_TYPES = {"PLUGIN_LIST": "token", "TEXTURE_LIST": "color3f", "FLOAT_TEXTURE_LIST": "float"}
SLOTS = 8
SLOTS_KEY = "vrayListSlots"  # the count input's sdrMetadata: how many slots its list has

# V-Ray's output parameters: a texture's other results, connected to as outputs:<name>.
OUTPUT_TYPES = {
    "OUTPUT_TEXTURE": "color3f",
    "OUTPUT_FLOAT_TEXTURE": "float",
    "OUTPUT_VECTOR_TEXTURE": "float3",
    "OUTPUT_INT_TEXTURE": "int",
    "OUTPUT_TRANSFORM_TEXTURE": "matrix4d",
}

# Parameters V-Ray's USD reader fills itself, from the Material and the stage.
SKIPPED = {"scene_name", "channels"}

# V-Ray's own versions of MaterialX's nodes (ND_add_float, ...), about two thirds of its
# textures. V-Ray reads MaterialX networks as they are, so the MaterialX nodes are the
# ones to use.
SKIPPED_PLUGINS = re.compile(r"^ND_")

_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_ROLLOUT_END = "<EndRollout>"


# plugininfo.

def plugininfo(sdk):
    """V-Ray's plugininfo.exe in an App SDK folder, or None."""
    for name in ("plugininfo.exe", "plugininfo"):
        path = os.path.join(sdk, "bin", name)
        if os.path.isfile(path):
            return path
    return None


def _run(sdk, arguments, timeout):
    """plugininfo's standard output; it reads the plugins from the SDK's bin/plugins.
    Raises RuntimeError if it can't run or fails."""
    program = plugininfo(sdk)
    if program is None:
        raise RuntimeError("%s has no bin/plugininfo.exe" % sdk)
    bin_folder = os.path.dirname(program)
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    done = subprocess.run([program, "-plugindir", os.path.join(bin_folder, "plugins")] + arguments, cwd=bin_folder,
                          capture_output=True, timeout=timeout, creationflags=flags)
    if done.returncode != 0:
        raise RuntimeError("plugininfo %s failed (exit code %d): %s" % (
            " ".join(arguments), done.returncode, done.stderr.decode("utf-8", "replace").strip()[-500:]))
    return done.stdout.decode("utf-8", "replace")


def export_descriptions(sdk, folder, timeout=600):
    """Has plugininfo write each plugin's JSON to `folder`/<type>/<plugin>.json, and
    returns its plugin categories: {category: [plugin, ...]}."""
    _run(sdk, ["-json", "-outputDir", folder], timeout)
    return parse_categories(_run(sdk, ["-list", "-listByCategory"], timeout))


def parse_categories(text):
    """plugininfo -list -listByCategory's output as {category: [plugin, ...]}."""
    categories = {}
    current = None
    for line in text.splitlines():
        found = re.match(r'^Category "(.*)":\s*$', line)
        if found:
            current = categories.setdefault(found.group(1), [])
        elif current is not None and line.startswith((" ", "\t")):
            for name in line.split():
                if name not in current:
                    current.append(name)
        elif line.strip():
            current = None
    return categories


def read_descriptions(folder):
    """The plugins plugininfo wrote to `folder` that are shader nodes, as
    [(JSON folder, description)], by plugin name."""
    found = []
    for kind in FOLDERS:
        path = os.path.join(folder, kind)
        if not os.path.isdir(path):
            continue
        for name in sorted(os.listdir(path)):
            if name.endswith(".json"):
                with open(os.path.join(path, name), encoding="utf-8") as stream:
                    found.append((kind, json.load(stream)))
    return found


# The .usda.

def generate(descriptions, categories, version=""):
    """The text of the definitions file for `descriptions` (read_descriptions()) and
    plugininfo's `categories` (parse_categories()). Returns (text, identifiers)."""
    texture_kinds = {}
    for category, group, out_type in reversed(TEXTURE_CATEGORIES):
        for plugin in categories.get(category, ()):
            texture_kinds[plugin] = (group, out_type)
    lines = ["#usda 1.0", "(", '    doc = """V-Ray shader nodes for usdcc, from V-Ray%s\'s plugininfo."""' % (
        (" " + version) if version else ""), ")", ""]
    identifiers = []
    seen = set()
    for kind, plugin in sorted(descriptions, key=lambda item: item[1].get("ID", "")):
        name = plugin.get("ID", "")
        if not _NAME.match(name) or SKIPPED_PLUGINS.match(name) or name in seen:
            continue
        seen.add(name)
        group, out_type = FOLDERS[kind], "token"
        if kind == "textures":
            group, out_type = texture_kinds.get(name, ("Texture", "color3f"))
        lines += _shader(name, plugin, group, out_type)
        identifiers.append(PREFIX + name)
    return "\n".join(lines) + "\n", identifiers


def _shader(name, plugin, group, out_type):
    metadata = {"role": group}
    label = (plugin.get("Name") or "").strip().replace("V Ray", "V-Ray")
    if label and label != name:
        metadata["label"] = label
    lines = ['def Shader "%s" (' % name, "    sdrMetadata = {"]
    lines += ["        string %s = %s" % (key, _string(value)) for key, value in sorted(metadata.items())]
    lines += ["    }", ")", "{",
              '    uniform token info:id = "%s%s"' % (PREFIX, name),
              '    uniform token info:implementationSource = "sourceAsset"',
              "    uniform asset info:%s:sourceAsset = @./%s@" % (SYSTEM, FILE_NAME),
              "    %s outputs:out" % out_type]
    # A rollout runs from the parameter that names it to the one marked <EndRollout>.
    rollout = ""
    for parameter in plugin.get("Parameters", ()):
        ui = parameter.get("ui") or {}
        if "rollout" in ui and ui["rollout"] != _ROLLOUT_END:
            rollout = ui["rollout"]
        lines += _property(parameter, ui, rollout)
        if ui.get("rollout") == _ROLLOUT_END:
            rollout = ""
    lines += ["}", ""]
    return lines


def _property(parameter, ui, rollout):
    attr, kind = parameter.get("attr", ""), parameter.get("type", "")
    if not _NAME.match(attr) or attr in SKIPPED or attr == "out":
        return []
    if kind in OUTPUT_TYPES:
        return ["    %s outputs:%s" % (OUTPUT_TYPES[kind], attr)]
    if kind in SLOT_TYPES:
        return _slots(attr, kind, ui, parameter)
    if kind not in TYPES:
        return []  # LIST and LIST_LIST: lists of anything, which USD has no type for
    usd_type, connectable = TYPES[kind]
    if kind == "STRING" and (ui.get("file_extensions") or ui.get("file_names") or ui.get("file_op")):
        usd_type = "asset"
    value = _value(usd_type, parameter.get("default"))
    head = "    %s%s inputs:%s%s (" % ("uniform " if not connectable else "", usd_type, attr,
                                       "" if value is None else " = " + value)
    body = []
    if not connectable:
        body.append('        connectability = "interfaceOnly"')
    metadata = {}
    description = (parameter.get("desc") or "").strip()
    if description:
        metadata["help"] = description  # Sdr's help; it doesn't read doc
    label = (ui.get("display_name") or "").strip()
    if label:
        metadata["label"] = label
    page = rollout or ui.get("tab") or ""
    if page:
        metadata["page"] = page
    if kind == "ENUM" and parameter.get("items"):
        metadata["options"] = "|".join("%s:%s" % (str(item[1]).replace("|", "/").replace(":", " -"), item[0])
                                       for item in parameter["items"] if len(item) >= 2)
    if usd_type == "asset" and ui.get("file_extensions"):
        metadata["fileExtensions"] = ",".join(ui["file_extensions"])
    if metadata:
        body.append("        sdrMetadata = {")
        body += ["            string %s = %s" % (key, _string(text)) for key, text in sorted(metadata.items())]
        body.append("        }")
    if not body:
        return [head[:-2]]
    return [head] + body + ["    )"]


def _slots(attr, kind, ui, parameter):
    """A list of connections: its count and SLOTS inputs, on a page of their own."""
    label = (ui.get("display_name") or "").strip() or attr.replace("_", " ").capitalize()
    description = (parameter.get("desc") or "").strip()
    count = {"label": label + " count", "page": label, SLOTS_KEY: str(SLOTS),
             "help": "How many of %s_1 to %s_%d V-Ray takes. The V-Ray panel's export raises it to the last one "
                     "set." % (attr, attr, SLOTS)}
    lines = ["    uniform int inputs:%s_count = 0 (" % attr,
             '        connectability = "interfaceOnly"',
             "        sdrMetadata = {"]
    lines += ["            string %s = %s" % (key, _string(text)) for key, text in sorted(count.items())]
    lines += ["        }", "    )"]
    for index in range(1, SLOTS + 1):
        slot = {"label": "%s %d" % (label, index), "page": label}
        if description:
            slot["help"] = description
        lines += ["    %s inputs:%s_%d (" % (SLOT_TYPES[kind], attr, index), "        sdrMetadata = {"]
        lines += ["            string %s = %s" % (key, _string(text)) for key, text in sorted(slot.items())]
        lines += ["        }", "    )"]
    return lines


def _value(usd_type, default):
    """`default` written as a USD value of `usd_type`, or None for none."""
    if usd_type.endswith("[]"):
        return None  # lists are empty unless set; a list default is one element's
    if default is None or usd_type == "token":
        return None
    try:
        if usd_type == "bool":
            return "true" if _bool(default) else "false"
        if usd_type == "int":
            return str(int(float(default)))
        if usd_type == "float":
            return _number(default)
        if usd_type in ("color3f", "float3"):
            return "(%s)" % ", ".join(_numbers(default, 3))
        if usd_type == "color4f":
            return "(%s)" % ", ".join(_numbers(default, 4))
        if usd_type == "string":
            return _string(str(default))
        if usd_type == "asset":
            return "@%s@" % str(default).replace("@", "") if default else None
        if usd_type == "matrix4d":
            values = _numbers(default, 12) if len(default) >= 12 else _numbers(default, 9) + ["0"] * 3
            rows = [values[0:3] + ["0"], values[3:6] + ["0"], values[6:9] + ["0"], values[9:12] + ["1"]]
            return "(%s)" % ", ".join("(%s)" % ", ".join(row) for row in rows)
    except (TypeError, ValueError):
        return None
    return None


def _bool(value):
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "yes", "on")
    return bool(value)


def _number(value):
    number = float(value)
    if math.isnan(number):
        raise ValueError("nan")
    if math.isinf(number):
        return "inf" if number > 0 else "-inf"
    return repr(number)


def _numbers(value, count):
    """`count` numbers from a number or a list, a single number repeated; a list
    longer than `count` (a color's alpha) is cut."""
    if isinstance(value, (int, float, str)):
        values = [value] * count
    else:
        values = list(value)[:count]
    if len(values) < count:
        raise ValueError("too few")
    return [_number(v) for v in values]


def _string(text):
    return json.dumps(text, ensure_ascii=False)


# The cache.

def build_key(sdk):
    """What tells one V-Ray build from another: its git hash, its SDK library and its
    plugins, with FORMAT. A string fit for a file name."""
    digest = hashlib.sha1(("%d|" % FORMAT).encode())
    git_hash = os.path.join(sdk, "git_hash.txt")
    if os.path.isfile(git_hash):
        with open(git_hash, "rb") as stream:
            digest.update(stream.read())
    files = [os.path.join(sdk, "bin", name) for name in ("VRaySDKLibrary.dll", "libVRaySDKLibrary.so")]
    plugins = os.path.join(sdk, "bin", "plugins")
    if os.path.isdir(plugins):
        files += [os.path.join(plugins, name) for name in sorted(os.listdir(plugins))]
    for path in files:
        try:
            status = os.stat(path)
        except OSError:
            continue
        digest.update(("%s|%d|%d\n" % (os.path.basename(path), status.st_size, int(status.st_mtime))).encode())
    return digest.hexdigest()[:16]


class ShaderCache:
    """Definition files in `folder`, one subfolder per V-Ray build."""

    def __init__(self, folder):
        self.folder = folder

    def path(self, sdk):
        return os.path.join(self.folder, build_key(sdk), FILE_NAME)

    def cached(self, sdk):
        """The file for this V-Ray build if it has been made, else None."""
        path = self.path(sdk)
        return path if os.path.isfile(path) and os.path.isfile(path + ".json") else None

    def identifiers(self, path):
        with open(path + ".json", encoding="utf-8") as stream:
            return json.load(stream)["identifiers"]

    def make(self, sdk, version=""):
        """Runs plugininfo and writes the file for this V-Ray build; returns its path.
        Slow (half a minute): call it off the UI thread."""
        work = tempfile.mkdtemp(prefix="usdcc-vray-shaders-")
        try:
            categories = export_descriptions(sdk, work)
            text, identifiers = generate(read_descriptions(work), categories, version)
        finally:
            shutil.rmtree(work, ignore_errors=True)
        if not identifiers:
            raise RuntimeError("plugininfo described no V-Ray shader plugins")
        return self.write(sdk, text, identifiers, version)

    def write(self, sdk, text, identifiers, version=""):
        """Caches generate()'s `text` and `identifiers` for this V-Ray build; returns the path."""
        path = self.path(sdk)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        # The list last: cached() takes the file only with it.
        with open(path, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        with open(path + ".json", "w", encoding="utf-8") as stream:
            json.dump({"format": FORMAT, "sdk": sdk, "version": version, "identifiers": identifiers}, stream)
        return path


# Sdr.

def registered():
    """Whether V-Ray's nodes are in Sdr already: it can't forget a node, so a plugin
    reload must not add them twice."""
    from pxr import Sdr

    return DEFAULT_SURFACE in Sdr.Registry().GetShaderNodeIdentifiers()


def register(path, identifiers):
    """Adds each node of the definitions file at `path` to Sdr and parses it; how
    many it added. Nothing if they are there already.

    USD's parser first looks for a prim named after the identifier, and "vray:..."
    can't be one, so each node warns once, the first time it is parsed. They are all
    parsed here, where those warnings are dropped; Sdr keeps the parsed nodes."""
    from pxr import Sdr, Tf

    if registered():
        return 0
    registry = Sdr.Registry()
    uri = os.path.abspath(path)
    for identifier in identifiers:
        name = identifier[len(PREFIX):]
        registry.AddDiscoveryResult(Sdr.NodeDiscoveryResult(
            identifier, Sdr.Version().GetAsDefault(), name, "", "usda", SYSTEM, uri, uri, "", {}, "", name))
    with Tf.DiagnosticTrap() as trap:
        for identifier in identifiers:
            registry.GetShaderNodeByIdentifier(identifier)
        trap.EraseMatching(lambda diagnostic: diagnostic.commentary.startswith("Invalid prim name '%s" % PREFIX))
    return len(identifiers)


def fill_list_counts(stage):
    """Sets each V-Ray list's count (see SLOT_TYPES) on `stage`, a copy for V-Ray, to
    its last slot that is set or connected, where the count says fewer. How many
    counts it changed."""
    from pxr import Sdf, Sdr, UsdShade

    registry = Sdr.Registry()
    changed = 0
    for prim in stage.Traverse():
        if not prim.IsA(UsdShade.Shader):
            continue
        identifier = UsdShade.Shader(prim).GetShaderId()
        if not identifier or not identifier.startswith(PREFIX):
            continue
        definition = registry.GetShaderNodeByIdentifier(identifier)
        if definition is None:
            continue
        for name in definition.GetShaderInputNames():
            slots = definition.GetShaderInput(name).GetMetadata().get(SLOTS_KEY)
            if not slots or not name.endswith("_count"):
                continue
            base = name[: -len("_count")]
            last = 0
            for index in range(1, int(slots) + 1):
                attribute = prim.GetAttribute("inputs:%s_%d" % (base, index))
                if attribute and (attribute.HasAuthoredValue() or attribute.HasAuthoredConnections()):
                    last = index
            count = prim.GetAttribute("inputs:" + name)
            authored = (count.Get() or 0) if count and count.HasAuthoredValue() else 0
            if last <= authored:
                continue
            if not count:
                count = prim.CreateAttribute("inputs:" + name, Sdf.ValueTypeNames.Int, custom=False,
                                             variability=Sdf.VariabilityUniform)
            count.Set(last)
            changed += 1
    return changed


def register_profile():
    """How the Material Editor shows V-Ray: its nodes grouped by role (BRDF, Material,
    Texture, ...), and its Material terminal outputs:vray:surface."""
    import usdcc.shading

    usdcc.shading.register_renderer_profile(SYSTEM, "V-Ray", terminal_prefix=SYSTEM,
                                            default_surface_node=DEFAULT_SURFACE, terminals=("surface",))
