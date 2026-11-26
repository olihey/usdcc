// usdcc.primvars (docs/PRIMVAR_VIEWER.md §9.2): the model from Python, values, and relations.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/python_runtime.h"
#include "usdcc/usd/primvar_info.h"

#include <string>

using usdcc::test::pythonTestApp;

namespace {
bool python(const std::string& code) { return pythonTestApp().python.runString(code); }
const std::string kStage = std::string("from pxr import Sdf, Usd, UsdGeom, Vt\nimport usdcc.primvars as p\nstage = Usd.Stage.Open(r\"") +
                           USDCC_TEST_DATA_DIR + "/broken_primvars.usda\")\nprim = lambda name: stage.GetPrimAtPath(\"/World/\" + name)\n";
}  // namespace

USDCC_TEST(python_primvars_info) {
    CHECK(python(kStage + R"(
good = p.info(prim("Good"))
assert good.prim == Sdf.Path("/World/Good") and not good.all_problems(), good.all_problems()
assert [each.name for each in good.primvars] == ["dirt", "displayColor", "id", "names", "st", "wetness"], good.primvars
st = good.get("st")
assert (st.type, st.interpolation, st.indexed, st.value_count, st.element_count) == ("texCoord2f[]", "faceVarying", True, 4, 8), st
assert (st.expected_count, st.expected_of, st.source) == (8, "face corners", "local"), st
assert st.values_layer == stage.GetRootLayer().identifier and st.id_target == Sdf.Path(), st
wetness = good.get("wetness")
assert wetness.source == "inherited" and wetness.source_prim == Sdf.Path("/World"), wetness
assert good.get("nothing") is None
assert p.info(prim("Good"), inherited=False).get("wetness") is None
)"), "info()");
    CHECK(python(kStage + R"(
broken = p.info(prim("ElementCount"))
(problem,) = broken.all_problems()
assert (problem.severity, problem.code, problem.primvars) == ("error", "ElementCount", ["dirt"]), problem
(group,) = p.info(prim("Incomplete")).groups
assert (group.id, group.members, group.missing) == ("skel", ["skel:jointIndices"], ["skel:jointWeights"]), group
assert group.problems[0].code == "Incomplete"
assert p.info(prim("IndicesOnly")).problems[0].code == "IndicesOnly"
assert not p.info(prim("IndexRange"), scan=False).all_problems()
)"), "problems and groups");
    CHECK(python(kStage + R"(
mesh = prim("Good")
assert [p.element_count(mesh, each) for each in ("constant", "uniform", "varying", "vertex", "faceVarying")] == [1, 2, 6, 6, 8]
assert p.element_count(prim("NotGeometry"), "vertex") is None
stage.SetEditTarget(stage.GetSessionLayer())  # the file's layer is the other tests' too
dirt = UsdGeom.PrimvarsAPI(mesh).GetPrimvar("dirt")
dirt.Set(Vt.FloatArray(6), 1.0)
dirt.Set(Vt.FloatArray(5), 2.0)
assert not p.info(mesh, 1.0).all_problems() and p.info(mesh, Usd.TimeCode(2.0)).all_problems()[0].code == "ElementCount"
assert not p.info(mesh).all_problems() and not p.info(mesh, Usd.TimeCode.Default()).all_problems()
)"), "element_count() and times");
}

USDCC_TEST(python_primvars_values) {
    CHECK(python(kStage + R"(
good = prim("Good")
assert len(p.values(good, "st")) == 4 and len(p.values(good, "st", flattened=True)) == 8
assert list(p.values(good, "dirt", flattened=True)) == [0, 1, 2, 3, 4, 5]
assert p.values(good, "wetness") == 0.5 and p.values(good, "nothing") is None
try:
    p.values(prim("IndexRange"), "st", flattened=True)
    raise AssertionError("no error")
except ValueError as error:
    assert "outside" in str(error), error
)"), "values()");
    CHECK(python(R"(
from pxr import Usd
import usdcc.primvars as p
stage = Usd.Stage.CreateInMemory()
stage.GetRootLayer().ImportFromString('''#usda 1.0
def Mesh "Mesh" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    rel material:binding = </Material>
}
def Material "Material"
{
    float primvars:gloss = 0.25 (
        interpolation = "constant"
    )
}
''')
mesh = stage.GetPrimAtPath("/Mesh")
assert p.info(mesh).get("gloss").source == "material" and p.values(mesh, "gloss") == 0.25
)"), "the bound material's");
    CHECK(python(R"(
from pxr import Usd
import usdcc.primvars as p
stage = Usd.Stage.CreateInMemory()
for bad in ("/Nowhere", "not a path"):
    try:
        p.info(bad)
        raise AssertionError("no error")
    except (RuntimeError, ValueError):
        pass
gone = stage.DefinePrim("/Gone")
stage.RemovePrim("/Gone")
try:
    p.info(gone)
    raise AssertionError("no error")
except ValueError:
    pass
)"), "no such prim");
}

USDCC_TEST(python_primvars_focus) {
    CHECK(python(std::string("import usdcc.usd, usdcc.primvars as p\nstage = usdcc.usd.open_stage(r\"") + USDCC_TEST_DATA_DIR +
                 R"(/broken_primvars.usda")
try:
    assert p.focus(stage) == ""
    p.set_focus("st", stage)
    assert p.focus(stage) == "st"
    p.set_focus("primvars:dirt", stage)
    assert p.focus(stage) == "dirt", p.focus(stage)
    p.set_focus("", stage)
    assert p.focus(stage) == ""
finally:
    usdcc.usd.close_stage(stage)
)"),
          "focus() and set_focus()");
}

USDCC_TEST(python_primvars_relations) {
    const std::size_t builtIn = usdcc::usd::PrimvarRelations::instance().relations().size();
    CHECK(python(R"(
import usdcc.primvars as p
assert "skel" in [each["id"] for each in p.relations()]
p.register_relation("studio.groom", "Groom", ["groom:*"], same_interpolation=True, derived_from=["points"])
p.register_relation("studio.groom", "Groom", ["groom:*"], same_interpolation=True, derived_from=["points"])
(mine,) = [each for each in p.relations() if each["id"] == "studio.groom"]
assert mine["members"] == ["groom:*"] and mine["same_interpolation"] and not mine["complete"], mine
from pxr import Sdf, Usd, UsdGeom
stage = Usd.Stage.CreateInMemory()
api = UsdGeom.PrimvarsAPI(UsdGeom.Mesh.Define(stage, "/Mesh"))
api.CreatePrimvar("groom:a", Sdf.ValueTypeNames.Float, UsdGeom.Tokens.constant).Set(1.0)
api.CreatePrimvar("groom:b", Sdf.ValueTypeNames.FloatArray, UsdGeom.Tokens.vertex).Set([1.0])
found = p.info(api.GetPrim())
assert [each.id for each in found.groups] == ["studio.groom"] and found.groups[0].problems[0].code == "InterpolationDiffers", found
assert found.get("groom:a").derived_from == ["points"], found
assert p.remove_relation("studio.groom") and not p.remove_relation("studio.groom")
for bad in (("", ["a"]), ("x", [])):
    try:
        p.register_relation(bad[0], "", bad[1])
        raise AssertionError("no error")
    except ValueError:
        pass
)"), "relations");
    CHECK(usdcc::usd::PrimvarRelations::instance().relations().size() == builtIn, "none left behind");
}
