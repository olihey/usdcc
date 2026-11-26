#include "usdcc/usd/primvar_info.h"

#include "usdcc/usd/shading_preview.h"

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/math.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/hermiteCurves.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/skeleton.h>
#include <pxr/usd/usdSkel/tokens.h>
#include <pxr/usdValidation/usdValidation/error.h>
#include <pxr/usdValidation/usdValidation/registry.h>
#include <pxr/usdValidation/usdValidation/timeRange.h>
#include <pxr/usdValidation/usdValidation/validator.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
constexpr const char* kPrimvars = "primvars:";
constexpr const char* kIndices = ":indices";
// How far the weights of a point may be from 1 together.
constexpr float kWeightTolerance = 1e-3f;

using Severity = PrimvarProblem::Severity;

// "1 point", "8 points": the plural is what's given.
std::string counted(std::size_t count, const std::string& plural) {
    std::string unit = plural;
    if (count == 1 && TfStringEndsWith(unit, "indices")) {
        unit.replace(unit.size() - 4, 4, "ex");
    } else if (count == 1 && TfStringEndsWith(unit, "s")) {
        unit.pop_back();
    }
    return std::to_string(count) + ' ' + unit;
}

std::string joined(const std::vector<std::string>& parts, const char* last = " and ") {
    std::string result;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += i + 1 == parts.size() ? last : ", ";
        result += parts[i];
    }
    return result;
}

std::optional<std::size_t> arraySize(const UsdAttribute& attribute, UsdTimeCode time) {
    VtValue value;
    if (!attribute || !attribute.Get(&value, time) || !value.IsArrayValued()) return std::nullopt;
    return value.GetArraySize();
}

// The layer with the strongest opinion on one of `fields`.
std::string strongestLayer(const UsdAttribute& attribute, UsdTimeCode time, const TfTokenVector& fields) {
    if (!attribute) return {};
    for (const SdfPropertySpecHandle& spec : attribute.GetPropertyStack(time)) {
        if (!spec) continue;
        for (const TfToken& field : fields) {
            if (spec->HasInfo(field)) return spec->GetLayer()->GetIdentifier();
        }
    }
    return {};
}

const TfTokenVector& valueFields() {
    static const TfTokenVector fields{SdfFieldKeys->Default, SdfFieldKeys->TimeSamples, SdfFieldKeys->Spline};
    return fields;
}

bool finite(float value) { return std::isfinite(value); }
bool finite(double value) { return std::isfinite(value); }
bool finite(GfHalf value) { return value.isFinite(); }
template <typename Vec>
bool finite(const Vec& value) {
    for (int i = 0; i < Vec::dimension; ++i) {
        if (!finite(value[i])) return false;
    }
    return true;
}

template <typename T>
bool addNonFinite(const VtValue& value, std::size_t& count) {
    if (value.IsHolding<VtArray<T>>()) {
        const VtArray<T>& array = value.UncheckedGet<VtArray<T>>();
        count += std::count_if(array.cbegin(), array.cend(), [](const T& each) { return !finite(each); });
        return true;
    }
    if (value.IsHolding<T>()) {
        if (!finite(value.UncheckedGet<T>())) ++count;
        return true;
    }
    return false;
}

template <typename... Ts>
std::size_t nonFiniteOf(const VtValue& value) {
    std::size_t count = 0;
    (void)(addNonFinite<Ts>(value, count) || ...);
    return count;
}

std::size_t nonFinite(const VtValue& value) {
    return nonFiniteOf<float, GfVec2f, GfVec3f, GfVec4f, double, GfVec2d, GfVec3d, GfVec4d, GfHalf, GfVec2h, GfVec3h,
                       GfVec4h>(value);
}

// A type whose role says it's one value per element, not data a shader takes as a list.
bool isPerElementRole(const SdfValueTypeName& type) {
    const TfToken& role = type.GetRole();
    return role == SdfValueRoleNames->Color || role == SdfValueRoleNames->TextureCoordinate ||
           role == SdfValueRoleNames->Normal || role == SdfValueRoleNames->Point || role == SdfValueRoleNames->Vector;
}

std::string layerName(const std::string& identifier) { return SdfLayer::GetDisplayNameFromIdentifier(identifier); }

// A primvar's parts, and the problems that are its own. `counts`: the prim's it's authored on,
// null for a primvar that isn't checked here.
PrimvarInfo readPrimvar(const UsdGeomPrimvar& primvar, UsdTimeCode time, const PrimvarElementCounts* counts, bool scan) {
    PrimvarInfo info;
    const UsdAttribute& attribute = primvar.GetAttr();
    const UsdPrim prim = attribute.GetPrim();
    info.name = primvar.GetPrimvarName().GetString();
    info.type = primvar.GetTypeName();
    info.interpolation = primvar.GetInterpolation();
    info.interpolationAuthored = primvar.HasAuthoredInterpolation();
    info.elementSize = primvar.GetElementSize();
    info.timeVarying = primvar.ValueMightBeTimeVarying();
    info.unauthoredValuesIndex = primvar.GetUnauthoredValuesIndex();
    info.sourcePrim = prim.GetPath();
    info.valuesLayer = strongestLayer(attribute, time, valueFields());
    info.interpolationLayer = strongestLayer(attribute, time, {UsdGeomTokens->interpolation});

    const auto problem = [&info](Severity severity, const char* code, std::string message) {
        info.problems.push_back({severity, code, std::move(message), {info.name}});
    };

    VtValue value;
    info.hasValue = attribute.Get(&value, time) && !value.IsEmpty();
    if (info.hasValue) info.valueCount = value.IsArrayValued() ? value.GetArraySize() : 1;

    // Read by name: GetIndices() complains in the log about indices that aren't int[].
    VtIntArray indices;
    const UsdAttribute indicesAttribute = prim.GetAttribute(TfToken(attribute.GetName().GetString() + kIndices));
    const bool indicesAreInts = indicesAttribute && indicesAttribute.GetTypeName() == SdfValueTypeNames->IntArray;
    if (indicesAttribute && indicesAttribute.HasAuthoredValue()) {
        info.indicesLayer = strongestLayer(indicesAttribute, time, valueFields());
        if (indicesAreInts) {
            info.indexed = indicesAttribute.Get(&indices, time);
        } else if (counts) {
            problem(Severity::Error, "IndicesType",
                    "Its indices are " + indicesAttribute.GetTypeName().GetAsToken().GetString() + ", not int[]");
        }
    }

    // Relevant to arrays only, as USD has it.
    const std::size_t size = info.type.IsArray() && info.elementSize > 0 ? std::size_t(info.elementSize) : 1;
    info.elementCount = info.indexed ? indices.size() : info.valueCount / size;

    if (primvar.IsIdTarget()) {
        SdfPathVector targets;
        const UsdRelationship relationship = prim.GetRelationship(TfToken(attribute.GetName().GetString() + ":idFrom"));
        if (relationship && relationship.GetForwardedTargets(&targets) && !targets.empty()) info.idTarget = targets.front();
        if (counts && !info.idTarget.IsEmpty() && !prim.GetStage()->GetObjectAtPath(info.idTarget)) {
            problem(Severity::Warning, "IdTarget",
                    "It's the path of " + info.idTarget.GetString() + ", which isn't there");
        }
    }
    if (!counts) return info;

    const PrimvarElementCounts::Count expected = counts->of(info.interpolation);
    info.expectedCount = expected.count;
    info.expectedOf = expected.of;

    if (!UsdGeomPrimvar::IsValidInterpolation(info.interpolation)) {
        problem(Severity::Error, "Interpolation", "\"" + info.interpolation.GetString() + "\" isn't an interpolation");
    }
    if (info.elementSize < 1) {
        problem(Severity::Error, "ElementSize", "Its element size is " + std::to_string(info.elementSize));
    }
    const bool constant = info.interpolation == UsdGeomTokens->constant;
    if (!constant && (prim.IsA<UsdGeomXform>() || prim.IsA<UsdGeomScope>())) {
        problem(Severity::Warning, "NothingToInterpolate",
                "It's " + info.interpolation.GetString() + " on a prim that isn't geometry, and only constant primvars are inherited");
    }
    if (!info.hasValue) return info;

    const auto fitting = [&counts](std::size_t count) {
        std::vector<std::string> fits;
        for (const TfToken& each : counts->fitting(count)) fits.push_back(each.GetString() + " (" + counts->of(each).of + ")");
        return joined(fits, " or ");
    };
    if (constant) {
        // A constant array may be a list a shader takes whole; one that looks like a value for
        // each element is a mistake.
        if (info.type.IsArray() && !counts->wholeValues && info.elementCount > 1) {
            const std::string fits = fitting(info.elementCount);
            if (!info.interpolationAuthored && !fits.empty()) {
                problem(Severity::Warning, "NoInterpolation",
                        "It has no interpolation, which means constant, and " + counted(info.elementCount, "values") +
                            ": that fits " + fits);
            } else if (isPerElementRole(info.type)) {
                problem(Severity::Warning, "ConstantArray",
                        "It's constant, with " + counted(info.elementCount, "values") + ": only the first is used" +
                            (fits.empty() ? std::string() : ". That many fits " + fits));
            }
        }
    } else if (info.expectedCount && info.elementSize > 0) {
        const bool whole = info.indexed || info.valueCount % size == 0;
        if (!whole || info.elementCount != *info.expectedCount) {
            std::string message = info.interpolation.GetString() + " needs " +
                                  counted(*info.expectedCount * (info.indexed ? 1 : size), info.indexed ? "indices" : "values") +
                                  " (" + counted(*info.expectedCount, info.expectedOf) +
                                  (!info.indexed && size > 1 ? ", " + std::to_string(size) + " each" : std::string()) +
                                  "), and it has " + std::to_string(info.indexed ? indices.size() : info.valueCount);
            if (const std::string fits = whole ? fitting(info.elementCount) : std::string(); !fits.empty()) {
                message += ", which fits " + fits;
            }
            problem(Severity::Error, "ElementCount", std::move(message));
        }
    }

    if (info.indexed && scan && info.elementSize > 0) {
        const std::size_t table = info.valueCount / size;
        const std::size_t outside = std::count_if(indices.cbegin(), indices.cend(), [&](int index) {
            if (info.unauthoredValuesIndex >= 0 && index == info.unauthoredValuesIndex) return false;
            return index < 0 || std::size_t(index) >= table;
        });
        if (outside > 0) {
            std::string message = std::to_string(outside) + " of " + counted(indices.size(), "indices") +
                                  (outside == 1 ? " is" : " are") + " outside its " + counted(table, "values");
            // The usual reason: values authored over a weaker layer's indices.
            if (!info.valuesLayer.empty() && info.valuesLayer != info.indicesLayer) {
                message += " (values from " + layerName(info.valuesLayer) + ", indices from " + layerName(info.indicesLayer) + ")";
            }
            problem(Severity::Error, "IndexRange", std::move(message));
        }
    }
    if (scan) {
        if (const std::size_t count = nonFinite(value); count > 0) {
            problem(Severity::Warning, "NonFinite",
                    counted(count, "values") + (count == 1 ? " is" : " are") + " infinite or not a number");
        }
    }
    // The primvar wins where the schema has an attribute for the same thing.
    if (info.name == "normals" || info.name == "widths") {
        if (const UsdAttribute loser = prim.GetAttribute(TfToken(info.name)); loser && loser.HasAuthoredValue()) {
            problem(Severity::Info, "Overrides", "The " + info.name + " attribute isn't used, as the primvar wins over it");
        }
    }
    return info;
}

// --- Relations -----------------------------------------------------------------------

// How many joints the prim's joint indices are of: its own list, else the Skeleton's.
std::optional<std::size_t> jointCount(const UsdPrim& prim) {
    // skel:joints is inherited, as the rest of the binding is.
    for (UsdPrim at = prim; at && !at.IsPseudoRoot(); at = at.GetParent()) {
        const UsdAttribute joints = at.GetAttribute(UsdSkelTokens->skelJoints);
        if (joints && joints.HasAuthoredValue()) return arraySize(joints, UsdTimeCode::Default());
    }
    if (const UsdSkelSkeleton skeleton = UsdSkelBindingAPI(prim).GetInheritedSkeleton()) {
        return arraySize(skeleton.GetJointsAttr(), UsdTimeCode::Default());
    }
    return std::nullopt;
}

void checkSkinning(const UsdPrim& prim, UsdTimeCode time, const PrimvarSet& primvars, std::vector<PrimvarProblem>& problems) {
    const auto valuesOf = [&](const std::string& name, VtValue* value) {
        const PrimvarInfo* info = primvars.find(name);
        if (!info || !info->hasValue) return static_cast<const PrimvarInfo*>(nullptr);
        const UsdPrim source = prim.GetStage()->GetPrimAtPath(info->sourcePrim);
        const UsdAttribute attribute = source ? source.GetAttribute(TfToken(kPrimvars + name)) : UsdAttribute();
        return attribute && attribute.Get(value, time) ? info : nullptr;
    };
    VtValue value;
    if (const PrimvarInfo* info = valuesOf("skel:jointIndices", &value); info && value.IsHolding<VtIntArray>()) {
        if (const std::optional<std::size_t> joints = jointCount(prim)) {
            const VtIntArray& indices = value.UncheckedGet<VtIntArray>();
            const std::size_t outside = std::count_if(indices.cbegin(), indices.cend(), [&joints](int index) {
                return index < 0 || std::size_t(index) >= *joints;
            });
            if (outside > 0) {
                problems.push_back({Severity::Error, "JointRange",
                                    std::to_string(outside) + " of " + counted(indices.size(), "joint indices") +
                                        (outside == 1 ? " is" : " are") + " outside the " + counted(*joints, "joints"),
                                    {"skel:jointIndices"}});
            }
        }
    }
    if (const PrimvarInfo* info = valuesOf("skel:jointWeights", &value);
        info && value.IsHolding<VtFloatArray>() && info->elementSize > 0) {
        const VtFloatArray& weights = value.UncheckedGet<VtFloatArray>();
        const std::size_t size = std::size_t(info->elementSize);
        std::size_t off = 0;
        for (std::size_t at = 0; at + size <= weights.size(); at += size) {
            float sum = 0;
            for (std::size_t i = 0; i < size; ++i) sum += weights[at + i];
            // Not "> tolerance": a NaN is off too.
            if (!(std::abs(sum - 1.0f) <= kWeightTolerance)) ++off;
        }
        if (off > 0) {
            problems.push_back({Severity::Warning, "WeightSum",
                                "The weights of " + std::to_string(off) + " of " + counted(weights.size() / size, "elements") +
                                    (off == 1 ? " doesn't" : " don't") + " add up to 1",
                                {"skel:jointWeights"}});
        }
    }
}

std::vector<PrimvarRelation> builtInRelations() {
    std::vector<PrimvarRelation> result;
    const auto add = [&result](const char* id, const char* label, std::vector<std::string> members) -> PrimvarRelation& {
        PrimvarRelation relation;
        relation.id = id;
        relation.label = label;
        relation.members = std::move(members);
        result.push_back(std::move(relation));
        return result.back();
    };
    PrimvarRelation& skinning = add("skel", "Skinning", {"skel:jointIndices", "skel:jointWeights"});
    skinning.complete = skinning.sameInterpolation = skinning.sameElementSize = true;
    skinning.check = checkSkinning;
    // Independent interpolations are legal, so nothing is checked.
    add("display", "Display color", {"displayColor", "displayOpacity"});
    PrimvarRelation& tangents = add("tangents", "Tangents", {"tangents", "bitangents", "binormals"});
    tangents.sameInterpolation = true;
    tangents.derivedFrom = {"points", "normals", "a UV set"};
    add("normals", "Normals", {"normals"}).derivedFrom = {"points", "faceVertexIndices"};
    return result;
}

PrimvarGroup makeGroup(const PrimvarRelation& relation, const UsdPrim& prim, UsdTimeCode time, const PrimvarSet& set) {
    PrimvarGroup group;
    group.id = relation.id;
    group.label = relation.label;
    std::vector<const PrimvarInfo*> members;
    bool local = false;
    for (const PrimvarInfo& info : set.primvars) {
        if (!info.hasValue || !relation.has(info.name)) continue;
        group.members.push_back(info.name);
        members.push_back(&info);
        local = local || info.source == PrimvarSource::Local;
    }
    if (members.empty()) return group;
    if (relation.complete) {
        for (const std::string& name : relation.members) {
            const bool pattern = !name.empty() && name.back() == '*';
            const PrimvarInfo* info = set.find(name);
            if (!pattern && (!info || !info->hasValue)) group.missing.push_back(name);
        }
    }
    // What's wrong is said where it's authored.
    if (!local) return group;
    if (!group.missing.empty()) {
        group.problems.push_back({Severity::Error, "Incomplete",
                                  "It has " + joined(group.members) + " but not " + joined(group.missing), group.members});
    }
    const auto differ = [&members](auto value) {
        return std::any_of(members.begin(), members.end(), [&](const PrimvarInfo* each) { return value(*each) != value(*members.front()); });
    };
    const auto listed = [&members](auto text) {
        std::vector<std::string> parts;
        for (const PrimvarInfo* each : members) parts.push_back(each->name + " is " + text(*each));
        return joined(parts);
    };
    if (relation.sameInterpolation && differ([](const PrimvarInfo& info) { return info.interpolation; })) {
        group.problems.push_back({Severity::Error, "InterpolationDiffers",
                                  listed([](const PrimvarInfo& info) { return info.interpolation.GetString(); }) +
                                      ": they have to be the same",
                                  group.members});
    }
    if (relation.sameElementSize && differ([](const PrimvarInfo& info) { return info.elementSize; })) {
        group.problems.push_back({Severity::Error, "ElementSizeDiffers",
                                  listed([](const PrimvarInfo& info) { return std::to_string(info.elementSize) + " an element"; }) +
                                      ": they have to be the same",
                                  group.members});
    }
    if (relation.check) relation.check(prim, time, set, group.problems);
    return group;
}

// --- Materials -----------------------------------------------------------------------

// The materials the prim's surface has: for each purpose, and those of its subsets.
std::vector<UsdShadeMaterial> boundMaterials(const UsdPrim& prim) {
    std::vector<UsdShadeMaterial> result;
    const auto add = [&result](const UsdPrim& of) {
        for (const TfToken& purpose : {UsdShadeTokens->allPurpose, UsdShadeTokens->preview, UsdShadeTokens->full}) {
            const UsdShadeMaterial material = UsdShadeMaterialBindingAPI(of).ComputeBoundMaterial(purpose);
            if (!material) continue;
            const bool known = std::any_of(result.begin(), result.end(), [&material](const UsdShadeMaterial& other) {
                return other.GetPath() == material.GetPath();
            });
            if (!known) result.push_back(material);
        }
    };
    add(prim);
    for (const UsdGeomSubset& subset : UsdShadeMaterialBindingAPI(prim).GetMaterialBindSubsets()) add(subset.GetPrim());
    return result;
}

}  // namespace

// --- Counts --------------------------------------------------------------------------

PrimvarElementCounts::Count PrimvarElementCounts::of(const TfToken& interpolation) const {
    if (interpolation == UsdGeomTokens->constant) return {1, "prims"};
    if (interpolation == UsdGeomTokens->uniform) return uniform;
    if (interpolation == UsdGeomTokens->varying) return varying;
    if (interpolation == UsdGeomTokens->vertex) return vertex;
    if (interpolation == UsdGeomTokens->faceVarying) return faceVarying;
    return {};
}

std::vector<TfToken> PrimvarElementCounts::fitting(std::size_t count) const {
    std::vector<TfToken> result;
    for (const TfToken& each : {UsdGeomTokens->vertex, UsdGeomTokens->faceVarying, UsdGeomTokens->uniform, UsdGeomTokens->varying}) {
        const Count expected = of(each);
        if (!expected.count || *expected.count != count) continue;
        // One for each kind of element: varying is vertex on a mesh.
        const bool known = std::any_of(result.begin(), result.end(), [&](const TfToken& other) { return of(other).of == expected.of; });
        if (!known) result.push_back(each);
    }
    return result;
}

PrimvarElementCounts primvarElementCounts(const UsdPrim& prim, UsdTimeCode time) {
    PrimvarElementCounts counts;
    if (const UsdGeomPointInstancer instancer{prim}) {
        // As on points, with instances for points (pointInstancer.h).
        counts.wholeValues = true;
        counts.vertex = {arraySize(instancer.GetProtoIndicesAttr(), time), "instances"};
        counts.varying = counts.faceVarying = counts.vertex;
        return counts;
    }
    const UsdGeomPointBased pointBased(prim);
    if (!pointBased) return counts;
    const PrimvarElementCounts::Count points{arraySize(pointBased.GetPointsAttr(), time), "points"};
    counts.vertex = points;
    if (const UsdGeomMesh mesh{prim}) {
        counts.varying = points;
        VtIntArray faces;
        if (mesh.GetFaceVertexCountsAttr().Get(&faces, time)) {
            counts.uniform = {faces.size(), "faces"};
            std::size_t corners = 0;
            for (const int face : faces) corners += std::size_t(std::max(face, 0));
            counts.faceVarying = {corners, "face corners"};
        }
    } else if (prim.IsA<UsdGeomPoints>()) {
        counts.varying = counts.faceVarying = points;
        counts.uniform = {1, "prims"};
    } else if (const UsdGeomBasisCurves curves{prim}) {
        if (const auto count = arraySize(curves.GetCurveVertexCountsAttr(), time)) {
            counts.uniform = {count, "curves"};
            counts.varying = {curves.ComputeVaryingDataSize(time), "segment ends"};
            counts.faceVarying = counts.varying;
        }
    } else if (const UsdGeomCurves curves{prim}) {
        // Varying isn't specified for these, but Hermite curves', which is per point.
        if (const auto count = arraySize(curves.GetCurveVertexCountsAttr(), time)) counts.uniform = {count, "curves"};
        if (prim.IsA<UsdGeomHermiteCurves>()) counts.varying = points;
    }
    return counts;
}

// --- Sets ----------------------------------------------------------------------------

const PrimvarInfo* PrimvarSet::find(const std::string& name) const {
    const auto it = std::find_if(primvars.begin(), primvars.end(), [&name](const PrimvarInfo& info) { return info.name == name; });
    return it == primvars.end() ? nullptr : &*it;
}

std::vector<PrimvarProblem> PrimvarSet::allProblems() const {
    std::vector<PrimvarProblem> result = problems;
    for (const PrimvarInfo& info : primvars) result.insert(result.end(), info.problems.begin(), info.problems.end());
    for (const PrimvarGroup& group : groups) result.insert(result.end(), group.problems.begin(), group.problems.end());
    return result;
}

PrimvarSet primvarInfo(const UsdPrim& prim, UsdTimeCode time, const PrimvarInfoOptions& options) {
    PrimvarSet set;
    if (!prim || prim.IsPseudoRoot()) return set;
    set.prim = prim.GetPath();
    const UsdGeomPrimvarsAPI api(prim);
    const PrimvarElementCounts counts = primvarElementCounts(prim, time);
    std::map<std::string, PrimvarInfo> found;
    for (const UsdGeomPrimvar& primvar : api.GetAuthoredPrimvars()) {
        PrimvarInfo info = readPrimvar(primvar, time, &counts, options.scan);
        found[info.name] = std::move(info);
    }
    for (const UsdProperty& property : prim.GetAuthoredPropertiesInNamespace("primvars")) {
        const std::string name = property.GetName().GetString();
        if (!TfStringEndsWith(name, kIndices) || !property.Is<UsdAttribute>() || !property.As<UsdAttribute>().HasAuthoredValue()) continue;
        const std::string values = name.substr(0, name.size() - std::string(kIndices).size());
        if (prim.GetAttribute(TfToken(values))) continue;
        const std::string primvar = values.substr(std::string(kPrimvars).size());
        set.problems.push_back({Severity::Warning, "IndicesOnly", primvar + " has indices but isn't there", {primvar}});
    }

    const std::vector<UsdShadeMaterial> materials =
        (options.inherited || options.readers) && prim.IsA<UsdGeomGprim>() ? boundMaterials(prim) : std::vector<UsdShadeMaterial>();
    if (options.inherited) {
        // A local one without a value doesn't hide the inherited one.
        const auto add = [&](const UsdGeomPrimvar& primvar, PrimvarSource source) {
            const auto it = found.find(primvar.GetPrimvarName().GetString());
            if (it != found.end() && it->second.hasValue) return;
            PrimvarInfo info = readPrimvar(primvar, time, nullptr, false);
            if (!info.hasValue) return;
            info.source = source;
            const PrimvarElementCounts::Count expected = counts.of(info.interpolation);
            info.expectedCount = expected.count;
            info.expectedOf = expected.of;
            found[info.name] = std::move(info);
        };
        for (const UsdGeomPrimvar& primvar : api.FindPrimvarsWithInheritance()) {
            if (primvar.GetAttr().GetPrim() != prim) add(primvar, PrimvarSource::Inherited);
        }
        for (const UsdShadeMaterial& material : materials) {
            for (const UsdGeomPrimvar& primvar : UsdGeomPrimvarsAPI(material.GetPrim()).GetPrimvarsWithAuthoredValues()) {
                add(primvar, PrimvarSource::Material);
            }
        }
    }
    for (auto& [name, info] : found) set.primvars.push_back(std::move(info));

    if (options.readers) {
        std::map<std::string, MissingPrimvar> missing;
        for (const UsdShadeMaterial& material : materials) {
            for (const MaterialPrimvar& read : primvarsReadBy(material)) {
                if (!read.reached) continue;
                const auto it = std::find_if(set.primvars.begin(), set.primvars.end(),
                                             [&read](const PrimvarInfo& info) { return info.name == read.name; });
                if (it != set.primvars.end() && it->hasValue) {
                    it->readers.push_back(material.GetPath());
                } else if (!rendererSuppliesPrimvar(read.name)) {
                    MissingPrimvar& each = missing[read.name];
                    each.name = read.name;
                    each.uv = each.uv || read.uv;
                    each.materials.push_back(material.GetPath());
                }
            }
        }
        for (auto& [name, each] : missing) set.missing.push_back(std::move(each));
    }

    for (const PrimvarRelation& relation : PrimvarRelations::instance().relations()) {
        for (PrimvarInfo& info : set.primvars) {
            if (relation.has(info.name)) {
                info.derivedFrom.insert(info.derivedFrom.end(), relation.derivedFrom.begin(), relation.derivedFrom.end());
            }
        }
        PrimvarGroup group = makeGroup(relation, prim, time, set);
        // One on its own is no group, unless it should have company or something's wrong with it.
        if (group.members.size() > 1 || !group.missing.empty() || !group.problems.empty()) set.groups.push_back(std::move(group));
    }
    return set;
}

// --- Statistics ----------------------------------------------------------------------

namespace {
double number(float value) { return value; }
double number(double value) { return value; }
double number(GfHalf value) { return float(value); }
double number(int value) { return value; }
double number(unsigned int value) { return value; }
double number(int64_t value) { return double(value); }
double number(uint64_t value) { return double(value); }
double number(unsigned char value) { return value; }
double number(bool value) { return value ? 1.0 : 0.0; }

template <typename T>
struct Components {
    static constexpr int size = 1;
    static double at(const T& value, int) { return number(value); }
};
template <typename T>
    requires requires { T::dimension; }
struct Components<T> {
    static constexpr int size = T::dimension;
    static double at(const T& value, int i) { return number(value[i]); }
};

template <typename T>
void numbers(const VtArray<T>& values, PrimvarStatistics& result) {
    constexpr int size = Components<T>::size;
    result.components = size;
    std::vector<double> sum(size, 0.0), low(size, 0.0), high(size, 0.0);
    std::size_t finite = 0;
    for (const T& value : values) {
        double parts[size];
        bool ok = true;
        for (int i = 0; i < size; ++i) {
            parts[i] = Components<T>::at(value, i);
            ok = ok && std::isfinite(parts[i]);
        }
        if (!ok) {
            ++result.nonFinite;
            continue;
        }
        for (int i = 0; i < size; ++i) {
            if (finite == 0 || parts[i] < low[i]) low[i] = parts[i];
            if (finite == 0 || parts[i] > high[i]) high[i] = parts[i];
            sum[i] += parts[i];
        }
        ++finite;
    }
    if (finite == 0) return;
    result.min = std::move(low);
    result.max = std::move(high);
    result.mean.resize(size);
    for (int i = 0; i < size; ++i) result.mean[i] = sum[i] / double(finite);
}

template <typename T, typename Key>
void distinct(const VtArray<T>& values, PrimvarStatistics& result, Key key) {
    std::unordered_map<std::string, std::size_t> seen;
    for (const T& value : values) {
        ++seen[key(value)];
        if (seen.size() > PrimvarStatistics::kDistinctLimit) return;
    }
    result.distinct = seen.size();
    std::vector<std::pair<std::string, std::size_t>> sorted(seen.begin(), seen.end());
    const std::size_t keep = std::min(sorted.size(), PrimvarStatistics::kCommonest);
    std::partial_sort(sorted.begin(), sorted.begin() + std::ptrdiff_t(keep), sorted.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    sorted.resize(keep);
    result.commonest = std::move(sorted);
}

// A scalar as an array of one.
template <typename T>
bool tryNumbers(const VtValue& value, PrimvarStatistics& result, bool counted) {
    VtArray<T> values;
    if (value.IsHolding<VtArray<T>>()) {
        values = value.UncheckedGet<VtArray<T>>();
    } else if (value.IsHolding<T>()) {
        values = VtArray<T>{value.UncheckedGet<T>()};
    } else {
        return false;
    }
    numbers(values, result);
    if constexpr (std::is_integral_v<T>) {
        if (counted) distinct(values, result, [](const T& each) { return std::to_string(static_cast<long long>(each)); });
    }
    return true;
}

template <typename... Ts>
bool anyNumbers(const VtValue& value, PrimvarStatistics& result, bool counted) {
    return (tryNumbers<Ts>(value, result, counted) || ...);
}

template <typename T, typename Key>
bool tryWords(const VtValue& value, PrimvarStatistics& result, Key key) {
    if (value.IsHolding<VtArray<T>>()) {
        distinct(value.UncheckedGet<VtArray<T>>(), result, key);
    } else if (value.IsHolding<T>()) {
        distinct(VtArray<T>{value.UncheckedGet<T>()}, result, key);
    } else {
        return false;
    }
    return true;
}
}  // namespace

PrimvarStatistics primvarStatistics(const VtValue& values) {
    PrimvarStatistics result;
    if (values.IsEmpty()) return result;
    result.count = values.IsArrayValued() ? values.GetArraySize() : 1;
    if (anyNumbers<float, double, GfHalf, GfVec2f, GfVec3f, GfVec4f, GfVec2d, GfVec3d, GfVec4d, GfVec2h, GfVec3h, GfVec4h,
                   GfVec2i, GfVec3i, GfVec4i>(values, result, false) ||
        anyNumbers<int, unsigned int, int64_t, uint64_t, unsigned char, bool>(values, result, true)) {
        return result;
    }
    tryWords<std::string>(values, result, [](const std::string& text) { return text; }) ||
        tryWords<TfToken>(values, result, [](const TfToken& token) { return token.GetString(); });
    return result;
}

bool flattenPrimvar(VtValue& values, const VtIntArray& indices, int elementSize) {
    if (indices.empty() || !values.IsArrayValued()) return true;
    VtValue flat;
    std::string error;
    if (!UsdGeomPrimvar::ComputeFlattened(&flat, values, indices, std::max(elementSize, 1), &error)) return false;
    values = std::move(flat);
    return true;
}

// --- PrimvarRelations ----------------------------------------------------------------

bool PrimvarRelation::has(const std::string& name) const {
    return std::any_of(members.begin(), members.end(), [&name](const std::string& member) {
        if (!member.empty() && member.back() == '*') return TfStringStartsWith(name, member.substr(0, member.size() - 1));
        return name == member;
    });
}

PrimvarRelations& PrimvarRelations::instance() {
    static PrimvarRelations* const relations = new PrimvarRelations;  // outlives Python and everything else
    return *relations;
}

PrimvarRelations::PrimvarRelations() { reset(); }

void PrimvarRelations::set(PrimvarRelation relation) {
    const auto it = std::find_if(m_relations.begin(), m_relations.end(),
                                 [&relation](const PrimvarRelation& other) { return other.id == relation.id; });
    if (it != m_relations.end()) {
        *it = std::move(relation);
    } else {
        m_relations.push_back(std::move(relation));
    }
}

bool PrimvarRelations::remove(const std::string& id) {
    return std::erase_if(m_relations, [&id](const PrimvarRelation& relation) { return relation.id == id; }) > 0;
}

void PrimvarRelations::reset() { m_relations = builtInRelations(); }

// --- Validation ----------------------------------------------------------------------

namespace validation {

namespace {
// A prim's problems as OpenUSD's errors, at the properties they're about.
void report(const UsdPrim& prim, const std::vector<PrimvarProblem>& problems, UsdValidationErrorVector& errors) {
    for (const PrimvarProblem& problem : problems) {
        UsdValidationErrorSites sites;
        for (const std::string& name : problem.primvars) {
            const SdfPath path = prim.GetPath().AppendProperty(TfToken(kPrimvars + name));
            // It may be inherited, or only its indices there.
            if (prim.GetStage()->GetPropertyAtPath(path)) sites.emplace_back(prim.GetStage(), path);
        }
        if (sites.empty()) sites.emplace_back(prim.GetStage(), prim.GetPath());
        const UsdValidationErrorType type = problem.severity == Severity::Error     ? UsdValidationErrorType::Error
                                            : problem.severity == Severity::Warning ? UsdValidationErrorType::Warn
                                                                                    : UsdValidationErrorType::Info;
        errors.emplace_back(TfToken(problem.code), type, sites,
                            problem.primvars.size() == 1 ? problem.primvars.front() + ": " + problem.message : problem.message);
    }
}

bool hasPrimvars(const UsdPrim& prim) { return !prim.GetAuthoredPropertiesInNamespace("primvars").empty(); }
}  // namespace

void registerPrimvarRules() {
    static std::once_flag once;
    std::call_once(once, []() {
        UsdValidationRegistry& registry = UsdValidationRegistry::GetInstance();
        const auto add = [&registry](const char* name, const char* doc, const UsdValidatePrimTaskFn& task) {
            UsdValidationValidatorMetadata metadata;
            metadata.name = TfToken(name);
            metadata.doc = doc;
            metadata.keywords = {TfToken("Primvars")};
            metadata.schemaTypes = {TfToken("UsdGeomImageable")};
            registry.RegisterValidator(metadata, task);
        };
        add("usdccValidators:Primvars",
            "A primvar's values and indices are as many as its interpolation and element size need, its indices are "
            "inside its values, and its numbers are finite. Checked at the earliest time.",
            [](const UsdPrim& prim, const UsdValidationTimeRange&) {
                UsdValidationErrorVector errors;
                if (!hasPrimvars(prim)) return errors;
                const PrimvarSet set = primvarInfo(prim, UsdTimeCode::EarliestTime(), {false, false, true});
                report(prim, set.problems, errors);
                for (const PrimvarInfo& info : set.primvars) report(prim, info.problems, errors);
                return errors;
            });
        add("usdccValidators:PrimvarGroups",
            "Primvars that belong together, such as joint indices and weights, are all there and fit each other. "
            "Checked at the earliest time.",
            [](const UsdPrim& prim, const UsdValidationTimeRange&) {
                UsdValidationErrorVector errors;
                if (!hasPrimvars(prim)) return errors;
                const PrimvarSet set = primvarInfo(prim, UsdTimeCode::EarliestTime(), {true, false, false});
                for (const PrimvarGroup& group : set.groups) report(prim, group.problems, errors);
                return errors;
            });
    });
}

}  // namespace validation

}  // namespace usdcc::usd
