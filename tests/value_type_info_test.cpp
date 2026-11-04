// ValueTypeInfo: every value type this USD build registers is classified, has
// a sample value of the right C++ type that can be authored and read back, and
// numeric/array helpers round-trip.
#include "test_harness.h"

#include "usdcc/usd/value_type_info.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <set>

using namespace PXR_NS;
using namespace usdcc::usd;

USDCC_TEST(value_type_info) {
    const std::vector<SdfValueTypeName> types = canonicalValueTypes();
    CHECK(types.size() > 100, "canonical types found (%zu)", types.size());

    std::set<std::string> names;
    for (const auto& t : types) {
        names.insert(t.GetAsToken().GetString());
    }
    CHECK(!names.count("Vec3f") && !names.count("PointFloat") && !names.count("Color"), "legacy aliases dropped");
    CHECK(names.count("float3") && names.count("color3f") && names.count("matrix4d[]"), "canonical names kept");
#if __has_include(<pxr/base/gf/duration.h>)
    CHECK(names.count("duration"), "and duration, in an OpenUSD build that has it");
#endif

    // Classification and samples, authored on a real attribute.
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdPrim prim = stage->DefinePrim(SdfPath("/P"));
    int index = 0;
    for (const auto& t : types) {
        const ValueTypeInfo info = ValueTypeInfo::classify(t);
        const std::string name = t.GetAsToken().GetString();
        CHECK(info.scalarKind != ScalarKind::Unknown, "%s: classified", name.c_str());
        CHECK(info.isArray == t.IsArray(), "%s: array flag", name.c_str());

        const VtValue sample = sampleValue(t);
        if (info.scalarKind == ScalarKind::Opaque) {
            CHECK(sample.IsEmpty(), "%s: no sample for opaque", name.c_str());
            continue;
        }
        CHECK(!sample.IsEmpty(), "%s: has a sample", name.c_str());
        CHECK(sample.GetType() == t.GetType(), "%s: sample has the type's C++ type (%s vs %s)", name.c_str(),
              sample.GetType().GetTypeName().c_str(), t.GetType().GetTypeName().c_str());
        if (info.isArray) {
            CHECK(arraySize(sample) == 4, "%s: sample array has 4 elements (%zu)", name.c_str(), arraySize(sample));
        }

        UsdAttribute attr = prim.CreateAttribute(TfToken("a" + std::to_string(index++)), t);
        CHECK(attr.Set(sample), "%s: sample can be authored", name.c_str());
        VtValue back;
        CHECK(attr.Get(&back) && back == sample, "%s: sample reads back equal", name.c_str());

        if (info.isNumeric() && !info.isArray) {
            const std::vector<double> numbers = numericComponents(sample);
            CHECK(static_cast<int>(numbers.size()) == info.numberCount(), "%s: %d numbers (%zu)", name.c_str(),
                  info.numberCount(), numbers.size());
            CHECK(makeNumericValue(t, numbers) == sample, "%s: numbers round-trip", name.c_str());
        }
    }

    // Specific classifications.
    const ValueTypeInfo color = ValueTypeInfo::classify(SdfValueTypeNames->Color3f);
    CHECK(color.group == TypeGroup::Colors && color.components == 3 && color.scalarKind == ScalarKind::Float,
          "color3f");
    CHECK(color.componentLabels() == std::vector<std::string>({"R", "G", "B"}), "color labels");
    const ValueTypeInfo uv = ValueTypeInfo::classify(SdfValueTypeNames->TexCoord2f);
    CHECK(uv.componentLabels() == std::vector<std::string>({"U", "V"}), "texcoord labels");
    const ValueTypeInfo quat = ValueTypeInfo::classify(SdfValueTypeNames->Quatd);
    CHECK(quat.shape == ValueShape::Quaternion && quat.componentLabels().front() == "W", "quaternion, real first");
    const ValueTypeInfo m4 = ValueTypeInfo::classify(SdfValueTypeNames->Matrix4d);
    CHECK(m4.shape == ValueShape::Matrix && m4.components == 4 && m4.numberCount() == 16, "matrix4d");
    const ValueTypeInfo pts = ValueTypeInfo::classify(SdfValueTypeNames->Point3fArray);
    CHECK(pts.isArray && pts.group == TypeGroup::PointsNormalsVectors && pts.components == 3, "point3f[]");
    CHECK(ValueTypeInfo::classify(SdfValueTypeNames->Int64).isInteger(), "int64 is integer");
    CHECK(!ValueTypeInfo::classify(SdfValueTypeNames->Bool).isNumeric(), "bool is not numeric");

    // Array editing.
    VtValue arr(VtArray<GfVec3f>{GfVec3f(1, 2, 3), GfVec3f(4, 5, 6)});
    CHECK(arraySize(arr) == 2, "size");
    CHECK(arrayElement(arr, 1) == VtValue(GfVec3f(4, 5, 6)), "element");
    CHECK(setArrayElement(arr, 0, VtValue(GfVec3f(9, 9, 9))) && arrayElement(arr, 0) == VtValue(GfVec3f(9, 9, 9)),
          "set element");
    CHECK(!setArrayElement(arr, 0, VtValue(1.0f)), "set rejects the wrong element type");
    CHECK(insertArrayElement(arr, 1, VtValue(GfVec3f(7, 7, 7))) && arraySize(arr) == 3 &&
              arrayElement(arr, 1) == VtValue(GfVec3f(7, 7, 7)),
          "insert");
    CHECK(removeArrayElement(arr, 0) && arraySize(arr) == 2, "remove");
    CHECK(resizeArray(arr, 5) && arraySize(arr) == 5 && arrayElement(arr, 4) == VtValue(GfVec3f(0, 0, 0)),
          "resize fills with defaults");
    CHECK(emptyArray(SdfValueTypeNames->TokenArray).IsHolding<VtArray<TfToken>>(), "empty token array");

    // Formatting.
    CHECK(formatValue(VtValue(VtArray<double>{5.0, 15.0, 40.0, 80.0})) == "[4] 5.0, 15.0, 40.0, \xE2\x80\xA6",
          "array summary: '%s'", formatValue(VtValue(VtArray<double>{5.0, 15.0, 40.0, 80.0})).c_str());
    CHECK(formatValue(VtValue(0.42f)) == "0.42", "float: '%s'", formatValue(VtValue(0.42f)).c_str());
    CHECK(formatValue(VtValue(GfVec3f(0, 1.24f, 0)), 2) == "(0.0, 1.24, 0.0)", "vector: '%s'",
          formatValue(VtValue(GfVec3f(0, 1.24f, 0)), 2).c_str());
}
