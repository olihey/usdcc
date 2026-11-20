// V-Ray's shader nodes for the Material Editor (src/plugins/vray_render/.../shaders.py,
// docs/VRAY_RENDER.md section 8): definitions generated from plugininfo's JSON, here a
// few plugins written for the test, registered with Sdr through the plugin's cache, and
// the list counts the V-Ray export fills in. Needs no V-Ray: plugininfo itself isn't
// run. Needs PySide6, like the other plugin tests; without it this file isn't built.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/python_runtime.h"

#include <string>

using usdcc::test::pythonTestApp;

namespace {
bool run(const std::string& code) { return pythonTestApp().python.runString(code); }

std::string evaluated(const std::string& expression) {
    const auto value = pythonTestApp().python.evaluate(expression);
    return value ? *value : "?";
}

void forgetPluginModules() {
    run("import sys\n"
        "for name in [n for n in sys.modules if n.split('.')[0] == 'usdcc_vray_render']:\n"
        "    del sys.modules[name]\n");
}
}  // namespace

USDCC_TEST(vray_shader_nodes) {
    forgetPluginModules();
    CHECK(run(R"(
import builtins, json, os, sys, tempfile
sys.path.insert(0, os.path.join(r")" USDCC_EXAMPLE_PLUGINS R"(", "vray_render", "python"))
from usdcc_vray_render import shaders

root = tempfile.mkdtemp(prefix="usdcc-vray-nodes-test-")
descriptions = os.path.join(root, "json")

def plugin(folder, name, label, parameters):
    os.makedirs(os.path.join(descriptions, folder), exist_ok=True)
    with open(os.path.join(descriptions, folder, name + ".json"), "w", encoding="utf-8") as stream:
        json.dump({"ID": name, "Name": label, "Desciption": name, "Type": folder.upper(), "Parameters": parameters,
                   "Widget": {"widgets": []}}, stream)

# As plugininfo -json writes them, with a parameter of each kind that matters.
plugin("BRDF", "BRDFVRayMtl", "V Ray Mtl", [
    {"attr": "diffuse", "type": "TEXTURE", "default": [0.5, 0.5, 0.5, 1.0], "desc": "The diffuse color",
     "ui": {"display_name": "Diffuse color", "rollout": "Diffuse"}},
    {"attr": "brdf_type", "type": "ENUM", "default": "4", "desc": "The BRDF",
     "items": [["0", "Phong", "Phong"], ["4", "GGX", "GGX"]], "ui": {"display_name": "BRDF Type"}},
    {"attr": "reflect_glossiness", "type": "FLOAT_TEXTURE", "default": 1.0, "desc": "", "ui": {"rollout": "<EndRollout>"}},
    {"attr": "option_use_irradiance_map", "type": "BOOL", "default": True, "desc": ""},
    {"attr": "scene_name", "type": "STRING_LIST", "default": "", "desc": "", "elements_count": 0},
    {"attr": "anything", "type": "LIST", "default": None, "desc": ""},
])
plugin("BRDF", "BRDFLayered", "Layered", [
    {"attr": "brdfs", "type": "PLUGIN_LIST", "default": None, "desc": "The layers", "elements_count": 0},
    {"attr": "opacities", "type": "FLOAT_LIST", "default": 1.0, "desc": "", "elements_count": 0},
    {"attr": "channels", "type": "PLUGIN_LIST", "default": None, "desc": "", "elements_count": 0},
])
plugin("material", "MtlSingleBRDF", "Mtl Single BRDF", [
    {"attr": "brdf", "type": "PLUGIN", "default": None, "desc": ""},
])
plugin("textures", "TexChecker", "Checker", [
    {"attr": "white_color", "type": "TEXTURE", "default": [1.0, 1.0, 1.0, 1.0], "desc": ""},
    {"attr": "uvwgen", "type": "PLUGIN", "default": None, "desc": ""},
    {"attr": "out_alpha", "type": "OUTPUT_FLOAT_TEXTURE", "default": 0.0, "desc": ""},
])
plugin("textures", "BitmapBuffer", "Bitmap Buffer", [
    {"attr": "file", "type": "STRING", "default": "", "desc": "", "ui": {"file_extensions": ["exr", "png"]}},
])
plugin("textures", "ND_add_float", "Add", [])
plugin("uvwgen", "UVWGenChannel", "UVW Gen Channel", [
    {"attr": "uvw_transform", "type": "TRANSFORM", "default": [2, 0, 0, 0, 3, 0, 0, 0, 1, 0.5, 0.25, 0], "desc": ""},
])
plugin("light", "LightOmni", "Omni", [])
categories = shaders.parse_categories('''Using plugin search paths: "C:/V-Ray/bin/plugins"

Category "BSDF":
    BRDFVRayMtl    BRDFLayered    BRDFVRayMtl
Category "Bitmaps":
    BitmapBuffer
Category "Textures":
    TexChecker
Category "Textures Float":
    TexChecker    ND_add_float
''')
text, identifiers = shaders.generate(shaders.read_descriptions(descriptions), categories, "7.40")

# A folder that passes for an App SDK, and its cached definitions: nothing runs plugininfo.
sdk = os.path.join(root, "AppSDK")
for name in ("python/vray.py", "bin/VRaySDKLibrary.dll", "bin/plugininfo.exe"):
    os.makedirs(os.path.dirname(os.path.join(sdk, name)), exist_ok=True)
    open(os.path.join(sdk, name), "w").close()
cache = shaders.ShaderCache(os.path.join(root, "cache"))
builtins.vray_nothing_cached = cache.cached(sdk) is None
cache.write(sdk, text, identifiers, "7.40")

from usdcc_vray_render import node_loader
loader = node_loader.Loader(sdk, cache.folder)
loader.start()
builtins.vray_shaders = shaders
builtins.vray_categories = categories
builtins.vray_identifiers = identifiers
builtins.vray_loader = loader
builtins.vray_root = root
)"),
          "generating definitions from plugininfo-like JSON, and loading them from the cache");
    CHECK(evaluated("vray_categories['BSDF']") == "['BRDFVRayMtl', 'BRDFLayered']", "categories read, each plugin once: %s",
          evaluated("vray_categories").c_str());
    CHECK(evaluated("vray_identifiers") ==
              "['vray:BRDFLayered', 'vray:BRDFVRayMtl', 'vray:BitmapBuffer', 'vray:MtlSingleBRDF', 'vray:TexChecker', 'vray:UVWGenChannel']",
          "the shading plugins, without MaterialX's ND_ ones or lights: %s", evaluated("vray_identifiers").c_str());
    CHECK(evaluated("vray_nothing_cached") == "True", "nothing cached at first");
    CHECK(evaluated("vray_loader.state") == "registered", "the loader takes the cached file: %s %s",
          evaluated("vray_loader.state").c_str(), evaluated("vray_loader.message").c_str());

    CHECK(run(R"(
import builtins
from pxr import Sdf, Sdr
registry = Sdr.Registry()
mtl = registry.GetShaderNodeByIdentifier("vray:BRDFVRayMtl")
diffuse = mtl.GetShaderInput("diffuse")
brdf_type = mtl.GetShaderInput("brdf_type")
glossiness = mtl.GetShaderInput("reflect_glossiness")
layered = registry.GetShaderNodeByIdentifier("vray:BRDFLayered")
checker = registry.GetShaderNodeByIdentifier("vray:TexChecker")
uvw = registry.GetShaderNodeByIdentifier("vray:UVWGenChannel")
bitmap = registry.GetShaderNodeByIdentifier("vray:BitmapBuffer")
builtins.vray_read = {
    "valid": all(registry.GetShaderNodeByIdentifier(i).IsValid() for i in vray_identifiers),
    "system": mtl.GetSourceType(),
    "label": mtl.GetLabel(),
    "role": mtl.GetMetadata().get("role"),
    "out": [str(mtl.GetShaderOutput("out").GetTypeAsSdfType().GetSdfType())],
    "diffuse": (str(diffuse.GetTypeAsSdfType().GetSdfType()), tuple(round(v, 3) for v in diffuse.GetDefaultValue()),
                diffuse.IsConnectable(), diffuse.GetLabel(), diffuse.GetPage(), diffuse.GetHelp()),
    "brdf_type": (str(brdf_type.GetTypeAsSdfType().GetSdfType()), brdf_type.GetDefaultValue(), brdf_type.IsConnectable(),
                  [tuple(o) for o in brdf_type.GetOptions()]),
    "glossiness_page": glossiness.GetPage(),
    "bool": str(mtl.GetShaderInput("option_use_irradiance_map").GetTypeAsSdfType().GetSdfType()),
    "skipped": [n for n in ("scene_name", "anything") if mtl.GetShaderInput(n)],
    "list": sorted(n for n in layered.GetShaderInputNames() if n.startswith("brdfs")),
    "count": (layered.GetShaderInput("brdfs_count").GetMetadata().get("vrayListSlots"),
              layered.GetShaderInput("brdfs_count").IsConnectable()),
    "slot_help": layered.GetShaderInput("brdfs_2").GetHelp(),
    "values": str(layered.GetShaderInput("opacities").GetTypeAsSdfType().GetSdfType()),
    "channels": bool(layered.GetShaderInput("channels_count")),
    "checker": (checker.GetMetadata().get("role"), str(checker.GetShaderOutput("out").GetTypeAsSdfType().GetSdfType()),
                str(checker.GetShaderOutput("out_alpha").GetTypeAsSdfType().GetSdfType())),
    "bitmap": (bitmap.GetMetadata().get("role"), str(bitmap.GetShaderInput("file").GetTypeAsSdfType().GetSdfType())),
    "transform": uvw.GetShaderInput("uvw_transform").GetDefaultValue(),
}
)"),
          "reading the nodes back from Sdr");
    CHECK(evaluated("vray_read['valid']") == "True", "every node parses");
    CHECK(evaluated("(vray_read['system'], vray_read['label'], vray_read['role'], vray_read['out'])") ==
              "('vray', 'V-Ray Mtl', 'BRDF', ['token'])",
          "a BRDF: shading system vray, V-Ray's name, its group, the plugin itself as outputs:out: %s",
          evaluated("(vray_read['system'], vray_read['label'], vray_read['role'], vray_read['out'])").c_str());
    CHECK(evaluated("vray_read['diffuse']") == "('color3f', (0.5, 0.5, 0.5), True, 'Diffuse color', 'Diffuse', 'The diffuse color')",
          "a texture parameter: color3f without the alpha, connectable, labelled, on its rollout's page: %s",
          evaluated("vray_read['diffuse']").c_str());
    CHECK(evaluated("vray_read['brdf_type']") == "('int', 4, False, [('Phong', '0'), ('GGX', '4')])",
          "an enum: an int, a value only, with V-Ray's choices: %s", evaluated("vray_read['brdf_type']").c_str());
    CHECK(evaluated("vray_read['glossiness_page']") == "Diffuse", "the rollout runs to <EndRollout>");
    CHECK(evaluated("vray_read['bool']") == "bool", "a bool");
    CHECK(evaluated("vray_read['skipped']") == "[]", "no scene_name, which V-Ray's reader fills, and no untyped LIST");
    CHECK(evaluated("vray_read['list']") ==
              "['brdfs_1', 'brdfs_2', 'brdfs_3', 'brdfs_4', 'brdfs_5', 'brdfs_6', 'brdfs_7', 'brdfs_8', 'brdfs_count']",
          "a list of connections: a count and slots from 1: %s", evaluated("vray_read['list']").c_str());
    CHECK(evaluated("vray_read['count']") == "('8', False)", "the count says how many slots, and takes a value only");
    CHECK(evaluated("vray_read['slot_help']") == "The layers", "each slot has the list's help");
    CHECK(evaluated("vray_read['values']") == "float[]", "a list of values is an array");
    CHECK(evaluated("vray_read['channels']") == "False", "no render channels list");
    CHECK(evaluated("vray_read['checker']") == "('Texture', 'color3f', 'float')",
          "a texture in Textures and Textures Float is a color texture, with its float output too: %s",
          evaluated("vray_read['checker']").c_str());
    CHECK(evaluated("vray_read['bitmap']") == "('Bitmap', 'asset')", "a file is an asset");
    CHECK(evaluated("vray_read['transform']") == "( (2, 0, 0, 0), (0, 3, 0, 0), (0, 0, 1, 0), (0.5, 0.25, 0, 1) )",
          "a transform: V-Ray's three axes and offset as rows: %s", evaluated("vray_read['transform']").c_str());

    CHECK(evaluated("[n['group'] for n in __import__('usdcc.shading').shading.catalog() if n['identifier'] == 'vray:BRDFVRayMtl']") ==
              "['BRDF']",
          "the Material Editor's catalog has them, grouped");
    CHECK(evaluated("vray_shaders.register(vray_loader.path, vray_identifiers)") == "0", "registering again adds nothing");

    // The export fills in the counts V-Ray needs.
    CHECK(run(R"(
import builtins
from pxr import Gf, Sdf, Usd, UsdShade
stage = Usd.Stage.CreateInMemory()
layers = UsdShade.Shader.Define(stage, "/Looks/Mat/Layers")
layers.CreateIdAttr("vray:BRDFLayered")
for index in (1, 3):
    top = UsdShade.Shader.Define(stage, "/Looks/Mat/Top%d" % index)
    top.CreateIdAttr("vray:BRDFVRayMtl")
    layers.CreateInput("brdfs_%d" % index, Sdf.ValueTypeNames.Token).ConnectToSource(top.CreateOutput("out", Sdf.ValueTypeNames.Token))
more = UsdShade.Shader.Define(stage, "/Looks/Mat/More")
more.CreateIdAttr("vray:BRDFLayered")
more.CreateInput("brdfs_count", Sdf.ValueTypeNames.Int).Set(5)
more.CreateInput("brdfs_2", Sdf.ValueTypeNames.Token).ConnectToSource(top.GetOutput("out"))
from usdcc_vray_render import scene
files = scene.SceneFiles()
exported = Usd.Stage.Open(files.export(stage, scene.Camera(Gf.Matrix4d(1.0), 1.0), (64, 36)))
builtins.vray_counts = (exported.GetPrimAtPath("/Looks/Mat/Layers").GetAttribute("inputs:brdfs_count").Get(),
                        exported.GetPrimAtPath("/Looks/Mat/More").GetAttribute("inputs:brdfs_count").Get(),
                        bool(stage.GetPrimAtPath("/Looks/Mat/Layers").GetAttribute("inputs:brdfs_count")))
files.remove()
)"),
          "exporting a stage with V-Ray lists");
    CHECK(evaluated("vray_counts") == "(3, 5, False)",
          "the count is the last slot used, a larger one stays, and the stage itself is left alone: %s",
          evaluated("vray_counts").c_str());

    CHECK(run(R"(
import builtins, shutil
from usdcc_vray_render import node_loader
node_loader.CURRENT = vray_loader
node_loader.stop()
builtins.vray_profile_gone = not __import__("usdcc.shading").shading.remove_renderer_profile("vray")
shutil.rmtree(vray_root, ignore_errors=True)
)"),
          "stopping the loader");
    CHECK(evaluated("vray_profile_gone") == "True", "takes V-Ray's profile away");
    CHECK(run("import builtins, sys\nfor name in [n for n in dir(builtins) if n.startswith('vray_')]: delattr(builtins, name)\n"
              "sys.path[:] = [p for p in sys.path if not p.replace('\\\\', '/').endswith('vray_render/python')]\n"),
          "cleaning up");
    forgetPluginModules();
}
