// SPDX-License-Identifier: BSD-3-Clause
#include "conversion.h"
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace usdcc::blender_mesh {
namespace {
PXR_NAMESPACE_USING_DIRECTIVE
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> bool checked(std::string& error, Function&& function) {
    error.clear();
    try { function(); return true; }
    catch (const std::exception& e) { error = e.what(); }
    catch (...) { error = "Unknown USD mesh conversion failure"; }
    return false;
}
void staticAttribute(const UsdAttribute& attr) {
    require(attr.GetNumTimeSamples() == 0 && !attr.HasAuthoredConnections(),
            attr.GetPath().GetString() + ": time samples or connections are unsupported");
}
int domain(const TfToken& interpolation) {
    if (interpolation == UsdGeomTokens->vertex || interpolation == UsdGeomTokens->varying) return USDCC_MESH_POINT;
    if (interpolation == UsdGeomTokens->uniform) return USDCC_MESH_FACE;
    if (interpolation == UsdGeomTokens->faceVarying) return USDCC_MESH_CORNER;
    throw std::runtime_error("Unsupported interpolation: " + interpolation.GetString());
}
size_t cardinality(const MeshData& mesh, int d) {
    return d == USDCC_MESH_POINT ? mesh.points.size() / 3 : d == USDCC_MESH_FACE ? mesh.counts.size() : mesh.indices.size();
}
template<class T, int N> bool flattenVector(const VtValue& value, Channel& channel) {
    if (!value.IsHolding<VtArray<T>>()) return false;
    channel.components = N;
    const auto& values = value.UncheckedGet<VtArray<T>>();
    channel.values.reserve(values.size() * N);
    for (const auto& v : values)
        for (int i = 0; i < N; ++i) channel.values.push_back(v[i]);
    return true;
}
Channel channel(const std::string& name, int d, const VtValue& value) {
    Channel result;
    result.name = name;
    result.domain = d;
    if (value.IsHolding<VtFloatArray>()) {
        const auto& values = value.UncheckedGet<VtFloatArray>();
        result.values.assign(values.begin(), values.end());
    } else require(flattenVector<GfVec2f, 2>(value, result) || flattenVector<GfVec3f, 3>(value, result) ||
                   flattenVector<GfVec4f, 4>(value, result), name + ": unsupported topology-dependent type");
    return result;
}
struct Input {
    std::vector<UsdccMeshAttribute> attributes;
    UsdccMeshInput view{};
    explicit Input(const MeshData& data) {
        for (const auto& c : data.channels)
            attributes.push_back({c.domain, c.components, c.values.data(), c.values.size()});
        view = {USDCC_MESH_ABI, data.points.size() / 3, data.counts.size(), data.indices.size(), attributes.size(),
                data.points.data(), data.counts.data(), data.indices.data(), data.materials.data(), attributes.data(), data.leftHanded};
        require(data.points.size() % 3 == 0 && data.materials.size() == data.counts.size(), "Malformed mesh storage");
    }
};
void validateMesh(const MeshData& data) {
    Input input(data);
    char error[512]{};
    const int status = usdcc_blender_mesh_validate(&input.view, error, sizeof(error));
    require(status == 0, error);
}
bool supportedType(const SdfValueTypeName& type) {
    // Float vector roles (UV, color, normal, point, vector) share the same typed
    // storage; retain the exact Sdf type/role when authoring.
    const auto cpp = type.GetType();
    return type.IsArray() && (cpp == TfType::Find<VtFloatArray>() || cpp == TfType::Find<VtVec2fArray>() ||
                             cpp == TfType::Find<VtVec3fArray>() || cpp == TfType::Find<VtVec4fArray>());
}
void captureMesh(const UsdStageRefPtr& stage, const SdfPath& path, Snapshot& out) {
    require(bool(stage) && path.IsPrimPath() && !path.ContainsPrimVariantSelection(), "A valid mesh prim path is required");
    const auto prim = stage->GetPrimAtPath(path);
    const UsdGeomMesh mesh(prim);
    require(bool(mesh) && prim.IsActive() && prim.IsLoaded() && prim.IsDefined(), "Select an active, loaded UsdGeomMesh");
    const auto target = stage->GetEditTarget();
    const auto layers = stage->GetLayerStack();
    require(target.GetLayer() && target.GetLayer()->PermissionToEdit() &&
            std::find(layers.begin(), layers.end(), target.GetLayer()) != layers.end() &&
            target.GetMapFunction().IsIdentity(), "A writable, unmapped local edit target is required");
    for (auto p = prim; p && !p.IsPseudoRoot(); p = p.GetParent()) {
        require(!p.IsInstanceProxy() && !p.IsInstanceable() && !p.IsInPrototype(), "Instanced geometry is unsupported");
        require(!p.HasMetadata(TfToken("clips")), "Value-clip geometry is unsupported");
        for (const auto& spec : p.GetPrimStack())
            require(!spec->GetPath().ContainsPrimVariantSelection(), "Variant-composed editing is unsupported");
        for (const auto& schema : p.GetAppliedSchemas())
            require(schema != TfToken("SkelBindingAPI"), "Skinned or blend-shape geometry is unsupported");
        for (const auto& attr : p.GetAttributes())
            require(attr.GetName().GetString().rfind("skel:", 0) != 0 || !attr.HasAuthoredValueOpinion(),
                    "Skinned or blend-shape geometry is unsupported");
        for (const auto& rel : p.GetRelationships())
            require(rel.GetName().GetString().rfind("skel:", 0) != 0 || !rel.HasAuthoredTargets(),
                    "Skinned or blend-shape geometry is unsupported");
    }
    // Reject even equal-valued time samples. Default-time editing must not leave
    // stronger animation opinions behind for any geometry-dependent property.
    for (const auto& attr : prim.GetAttributes()) {
        const auto name = attr.GetName().GetString();
        if (name.rfind("xformOp:", 0) != 0 && name != "xformOpOrder" &&
            name.rfind("userProperties:", 0) != 0) staticAttribute(attr);
    }
    TfToken scheme, orientation;
    require(mesh.GetSubdivisionSchemeAttr().Get(&scheme) && scheme == UsdGeomTokens->none,
            "Only polygon meshes with subdivisionScheme = none are supported");
    require(mesh.GetOrientationAttr().Get(&orientation) &&
            (orientation == UsdGeomTokens->rightHanded || orientation == UsdGeomTokens->leftHanded), "Invalid mesh orientation");
    for (const auto& attr : {mesh.GetHoleIndicesAttr(), mesh.GetCreaseIndicesAttr(), mesh.GetCreaseLengthsAttr(),
                             mesh.GetCreaseSharpnessesAttr(), mesh.GetCornerIndicesAttr(), mesh.GetCornerSharpnessesAttr(),
                             mesh.GetVelocitiesAttr(), mesh.GetAccelerationsAttr()})
        require(!attr.HasAuthoredValueOpinion(), attr.GetName().GetString() + ": authored data is unsupported");
    VtVec3fArray points;
    VtIntArray counts, indices;
    require(mesh.GetPointsAttr().Get(&points) && mesh.GetFaceVertexCountsAttr().Get(&counts) &&
            mesh.GetFaceVertexIndicesAttr().Get(&indices), "Default-time polygon geometry is missing");
    require(points.size() <= USDCC_MESH_MAX_POINTS && counts.size() <= USDCC_MESH_MAX_FACES &&
            indices.size() <= USDCC_MESH_MAX_CORNERS, "Mesh exceeds conversion size limits");
    out.path = path;
    out.mesh.leftHanded = orientation == UsdGeomTokens->leftHanded;
    out.mesh.counts.assign(counts.begin(), counts.end());
    out.mesh.indices.assign(indices.begin(), indices.end());
    out.mesh.materials.assign(counts.size(), -1);
    for (const auto& point : points) out.mesh.points.insert(out.mesh.points.end(), point.data(), point.data() + 3);
    validateMesh(out.mesh);
    size_t floatBudget = out.mesh.points.size();
    for (const auto& var : UsdGeomPrimvarsAPI(prim).FindPrimvarsWithInheritance()) {
        staticAttribute(var.GetAttr());
        Primvar record;
        record.name = var.GetPrimvarName();
        record.type = var.GetTypeName();
        record.interpolation = var.GetInterpolation();
        record.elementSize = var.GetElementSize();
        record.unauthoredIndex = var.GetUnauthoredValuesIndex();
        record.indexed = var.IsIndexed();
        require(var.Get(&record.values), var.GetAttr().GetPath().GetString() + ": no default value");
        if (record.indexed) {
            staticAttribute(var.GetIndicesAttr());
            require(var.GetIndices(&record.indices), "Missing primvar indices");
            require(record.values.IsArrayValued() && record.elementSize > 0 && record.indices.size() <= USDCC_MESH_MAX_CORNERS,
                    record.name.GetString() + ": invalid indexed storage");
            for (int index : record.indices)
                require(index >= 0 && (size_t(index) + 1) <= record.values.GetArraySize() / record.elementSize,
                        record.name.GetString() + ": primvar index out of range");
        }
        if (record.name == TfToken("normals") && record.interpolation == UsdGeomTokens->constant) {
            require(record.elementSize == 1 && record.unauthoredIndex < 0 &&
                    record.type.GetType() == TfType::Find<VtVec3fArray>(), "Constant primvars:normals requires a float3 array");
            VtVec3fArray flattened;
            require(var.ComputeFlattened(&flattened) && flattened.size() == 1, "Constant primvars:normals requires one normal");
            floatBudget += points.size() * 3;
            require(floatBudget <= 64000000 && out.mesh.channels.size() < USDCC_MESH_MAX_ATTRIBUTES, "Mesh normal budget exceeded");
            record.channel = int(out.mesh.channels.size());
            out.mesh.channels.push_back(channel("primvars:normals", USDCC_MESH_POINT, VtValue(VtVec3fArray(points.size(), flattened[0]))));
        } else if (record.interpolation != UsdGeomTokens->constant) {
            require(record.elementSize == 1 && record.unauthoredIndex < 0 && supportedType(record.type),
                    record.name.GetString() + ": unsupported type, elementSize, or unauthored values");
            const int d = domain(record.interpolation);
            const auto expected = cardinality(out.mesh, d);
            require((record.indexed ? record.indices.size() : record.values.GetArraySize()) == expected,
                    record.name.GetString() + ": attribute cardinality mismatch");
            // Bound expansion before ComputeFlattened allocates indexed data.
            floatBudget += expected * 4;
            require(floatBudget <= 64000000 && out.mesh.channels.size() < USDCC_MESH_MAX_ATTRIBUTES,
                    "Mesh attribute conversion budget exceeded");
            VtValue flattened;
            require(var.ComputeFlattened(&flattened), record.name.GetString() + ": invalid primvar indexing");
            record.channel = int(out.mesh.channels.size());
            out.mesh.channels.push_back(channel("primvars:" + record.name.GetString(), d, flattened));
        }
        out.primvars.push_back(std::move(record));
    }
    if (mesh.GetNormalsAttr().HasValue()) {
        VtVec3fArray normals;
        require(mesh.GetNormalsAttr().Get(&normals), "Normals have no default value");
        out.normalsInterpolation = mesh.GetNormalsInterpolation();
        const auto expected = out.normalsInterpolation == UsdGeomTokens->constant ? 1 :
            cardinality(out.mesh, domain(out.normalsInterpolation));
        require(normals.size() == expected, "Normals cardinality mismatch");
        require(out.mesh.channels.size() < USDCC_MESH_MAX_ATTRIBUTES &&
                floatBudget + (out.normalsInterpolation == UsdGeomTokens->constant ? points.size() : expected) * 3 <= 64000000,
                "Mesh normals conversion budget exceeded");
        // Constant normals still use a point channel expanded for BMesh, and are
        // collapsed only after every returned value has been checked.
        if (out.normalsInterpolation == UsdGeomTokens->constant) {
            require(normals.size() == 1, "Constant normals require one value");
            normals = VtVec3fArray(points.size(), normals[0]);
        }
        out.normalsChannel = int(out.mesh.channels.size());
        out.mesh.channels.push_back(channel("normals", out.normalsInterpolation == UsdGeomTokens->constant ?
            USDCC_MESH_POINT : domain(out.normalsInterpolation), VtValue(normals)));
    }
    const auto subsets = UsdGeomSubset::GetAllGeomSubsets(mesh);
    const auto familyType = UsdGeomSubset::GetFamilyType(mesh, TfToken("materialBind"));
    if (!subsets.empty()) require(familyType == UsdGeomTokens->nonOverlapping || familyType == UsdGeomTokens->partition,
                                  "Material subsets require a nonOverlapping or partition family");
    for (const auto& subset : subsets) {
        for (const auto& attr : subset.GetPrim().GetAttributes()) staticAttribute(attr);
        TfToken element, family;
        require(subset.GetElementTypeAttr().Get(&element) && element == UsdGeomTokens->face &&
                subset.GetFamilyNameAttr().Get(&family) && family == TfToken("materialBind"),
                "Only face materialBind subsets are supported");
        staticAttribute(subset.GetIndicesAttr());
        Subset record{subset.GetPath(), {}};
        require(subset.GetIndicesAttr().Get(&record.indices), "Subset has no default indices");
        for (int face : record.indices) {
            require(face >= 0 && size_t(face) < counts.size() && out.mesh.materials[face] == -1,
                    "Material subset indices overlap, repeat, or are out of range");
            out.mesh.materials[face] = int(out.subsets.size());
        }
        out.subsets.push_back(std::move(record));
    }
    if (!subsets.empty() && familyType == UsdGeomTokens->partition)
        require(std::find(out.mesh.materials.begin(), out.mesh.materials.end(), -1) == out.mesh.materials.end(),
                "Material partition does not cover every face");
    validateMesh(out.mesh);
}
void checkResult(const Snapshot& snapshot, const Result& result) {
    validateMesh(result.mesh);
    const auto& a = snapshot.mesh;
    const auto& b = result.mesh;
    require(a.leftHanded == b.leftHanded && a.channels.size() == b.channels.size(), "Result schema mismatch");
    if (result.operation != 0) {
        require(result.operation >= USDCC_MESH_TRIANGULATE && result.operation <= USDCC_MESH_BEVEL &&
                result.sourcePoints.size() == b.points.size() / 3 && result.sourceFaces.size() == b.counts.size() &&
                result.sourceCorners.size() == b.indices.size() && result.normals.size() == b.indices.size() * 3,
                "Invalid operation result shape");
        auto validMap = [](const auto& map, size_t n) {
            return std::all_of(map.begin(), map.end(), [n](int v) { return v >= -1 && (v < 0 || size_t(v) < n); });
        };
        require(validMap(result.sourcePoints, a.points.size() / 3) && validMap(result.sourceFaces, a.counts.size()) &&
                validMap(result.sourceCorners, a.indices.size()), "Invalid operation provenance");
        for (float v : result.normals) require(std::isfinite(v), "Non-finite generated normal");
        for (int material : b.materials) require(material == -1 || size_t(material) < snapshot.subsets.size(), "Unknown result material");
        if (!snapshot.subsets.empty() && std::find(a.materials.begin(), a.materials.end(), -1) == a.materials.end())
            require(std::find(b.materials.begin(), b.materials.end(), -1) == b.materials.end(), "Operation broke complete material-subset coverage");
        for (size_t i = 0; i < a.channels.size(); ++i)
            require(a.channels[i].name == b.channels[i].name && a.channels[i].domain == b.channels[i].domain &&
                    a.channels[i].components == b.channels[i].components, "Operation changed attribute schema");
        if (result.operation == USDCC_MESH_TRIANGULATE) {
            for (int n : b.counts) require(n == 3, "Triangulation returned a non-triangle");
            for (size_t i = 0; i < a.channels.size(); ++i) {
                const auto& src = a.channels[i]; const auto& dst = b.channels[i];
                const auto& map = src.domain == USDCC_MESH_POINT ? result.sourcePoints :
                                  src.domain == USDCC_MESH_FACE ? result.sourceFaces : result.sourceCorners;
                for (size_t j = 0; j < map.size(); ++j) {
                    require(map[j] >= 0, "Triangulation lost attribute provenance");
                    require(std::equal(dst.values.begin() + j * src.components, dst.values.begin() + (j + 1) * src.components,
                                       src.values.begin() + size_t(map[j]) * src.components), "Triangulation changed source attribute values");
                }
            }
        }
        return;
    }
    // Phase 2 exposes only an unmodified round trip. Requiring a bijective,
    // identity provenance map prevents accidental use as an editing API.
    auto identity = [](const std::vector<int>& map, size_t count) {
        if (map.size() != count) return false;
        for (size_t i = 0; i < count; ++i) if (map[i] != int(i)) return false;
        return true;
    };
    require(identity(result.sourcePoints, a.points.size() / 3) && identity(result.sourceFaces, a.counts.size()) &&
            identity(result.sourceCorners, a.indices.size()), "Round-trip provenance mismatch");
    require(a.points == b.points && a.counts == b.counts && a.indices == b.indices && a.materials == b.materials,
            "Round trip changed topology, coordinates, or materials");
    for (size_t i = 0; i < a.channels.size(); ++i) {
        const auto& x = a.channels[i]; const auto& y = b.channels[i];
        require(x.name == y.name && x.domain == y.domain && x.components == y.components && x.values == y.values,
                "Round trip changed attribute " + x.name);
    }
}
}
bool capture(const PXR_NS::UsdStageRefPtr& stage, const PXR_NS::SdfPath& path, Snapshot& output, std::string& error) {
    return checked(error, [&] { Snapshot value; captureMesh(stage, path, value); output = std::move(value); });
}
bool roundTrip(const Snapshot& snapshot, Result& output, std::string& error) {
    return checked(error, [&] {
        Input input(snapshot.mesh);
        Result result;
        result.mesh = snapshot.mesh;
        std::fill(result.mesh.points.begin(), result.mesh.points.end(), std::numeric_limits<float>::quiet_NaN());
        std::fill(result.mesh.counts.begin(), result.mesh.counts.end(), -1);
        std::fill(result.mesh.indices.begin(), result.mesh.indices.end(), -1);
        std::fill(result.mesh.materials.begin(), result.mesh.materials.end(), -2);
        result.sourcePoints.assign(input.view.point_count, -1);
        result.sourceFaces.assign(input.view.face_count, -1);
        result.sourceCorners.assign(input.view.corner_count, -1);
        std::vector<float*> attributes;
        for (auto& c : result.mesh.channels) {
            std::fill(c.values.begin(), c.values.end(), std::numeric_limits<float>::quiet_NaN());
            attributes.push_back(c.values.data());
        }
        UsdccMeshOutput out{result.mesh.points.data(), result.mesh.counts.data(), result.mesh.indices.data(),
            result.mesh.materials.data(), attributes.data(), result.sourcePoints.data(), result.sourceFaces.data(), result.sourceCorners.data()};
        char diagnostic[512]{};
        const int status = usdcc_blender_mesh_roundtrip(&input.view, &out, diagnostic, sizeof(diagnostic));
        require(status == 0, diagnostic);
        checkResult(snapshot, result);
        output = std::move(result);
    });
}
bool validateResult(const Snapshot& snapshot, const Result& result, std::string& error) {
    return checked(error, [&] { checkResult(snapshot, result); });
}
UsdccMeshOptions defaultOptions(int operation) {
    UsdccMeshOptions options{};
    options.abi = USDCC_MESH_ABI; options.operation = operation;
    options.distance = operation == USDCC_MESH_MERGE ? 0.0001f : 0.1f;
    options.angle = operation == USDCC_MESH_DISSOLVE ? 0.08726646f : 0.5235988f;
    options.ratio = 0.5f; options.profile = 0.5f; options.segments = 1;
    options.quad_method = 4; options.ngon_method = 0; options.clamp_overlap = 1; options.bevel_material = -1;
    return options;
}
bool execute(const MeshData& data, const UsdccMeshOptions& options, Result& output, std::string& error) {
    return checked(error, [&] {
        Input input(data);
        char diagnostic[512]{};
        using Handle = std::unique_ptr<UsdccMeshResult, decltype(&usdcc_blender_mesh_result_free)>;
        Handle handle(usdcc_blender_mesh_execute(&input.view, &options, diagnostic, sizeof(diagnostic)), usdcc_blender_mesh_result_free);
        require(bool(handle), diagnostic);
        UsdccMeshResultView view{};
        require(usdcc_blender_mesh_result_view(handle.get(), &view) == 0, "Missing result buffers");
        Result result;
        result.operation = options.operation;
        result.diagnostic = view.diagnostic;
        require(usdcc_blender_mesh_result_timings(handle.get(), &result.importMs, &result.operationMs, &result.exportMs) == 0,
                "Missing native timing metadata");
        const auto& mesh = view.mesh;
        result.mesh.leftHanded = mesh.left_handed != 0;
        result.mesh.points.assign(mesh.points, mesh.points + mesh.point_count * 3);
        result.mesh.counts.assign(mesh.face_counts, mesh.face_counts + mesh.face_count);
        result.mesh.indices.assign(mesh.corner_points, mesh.corner_points + mesh.corner_count);
        result.mesh.materials.assign(mesh.face_materials, mesh.face_materials + mesh.face_count);
        result.mesh.channels = data.channels;
        require(mesh.attribute_count == result.mesh.channels.size(), "Result channel count mismatch");
        for (size_t i = 0; i < mesh.attribute_count; ++i) {
            const auto& attr = mesh.attributes[i];
            require(attr.domain == data.channels[i].domain && attr.components == data.channels[i].components, "Result channel type mismatch");
            result.mesh.channels[i].values.assign(attr.values, attr.values + attr.value_count);
        }
        result.sourcePoints.assign(view.source_points, view.source_points + mesh.point_count);
        result.sourceFaces.assign(view.source_faces, view.source_faces + mesh.face_count);
        result.sourceCorners.assign(view.source_corners, view.source_corners + mesh.corner_count);
        result.normals.assign(view.normals, view.normals + mesh.corner_count * 3);
        output = std::move(result);
    });
}
namespace {
bool equalMesh(const MeshData& a, const MeshData& b) {
    if (a.leftHanded != b.leftHanded || a.points != b.points || a.counts != b.counts || a.indices != b.indices ||
        a.materials != b.materials || a.channels.size() != b.channels.size()) return false;
    for (size_t i = 0; i < a.channels.size(); ++i) {
        const auto& x = a.channels[i]; const auto& y = b.channels[i];
        if (x.name != y.name || x.domain != y.domain || x.components != y.components || x.values != y.values) return false;
    }
    return true;
}
template<class T, int N> PXR_NS::VtValue vectors(const std::vector<float>& data) {
    PXR_NS::VtArray<T> values(data.size() / N);
    for (size_t i = 0; i < values.size(); ++i) for (int j = 0; j < N; ++j) values[i][j] = data[i * N + j];
    return PXR_NS::VtValue(values);
}
PXR_NS::VtValue channelValue(const Channel& channel) {
    switch (channel.components) {
    case 1: return PXR_NS::VtValue(PXR_NS::VtFloatArray(channel.values.begin(), channel.values.end()));
    case 2: return vectors<PXR_NS::GfVec2f, 2>(channel.values);
    case 3: return vectors<PXR_NS::GfVec3f, 3>(channel.values);
    case 4: return vectors<PXR_NS::GfVec4f, 4>(channel.values);
    default: throw std::runtime_error("Unknown channel component count");
    }
}
}
bool sameSnapshot(const Snapshot& a, const Snapshot& b) {
    if (a.path != b.path || !equalMesh(a.mesh, b.mesh) || a.normalsInterpolation != b.normalsInterpolation ||
        a.primvars.size() != b.primvars.size() || a.subsets.size() != b.subsets.size()) return false;
    for (size_t i = 0; i < a.primvars.size(); ++i) {
        const auto& x = a.primvars[i]; const auto& y = b.primvars[i];
        if (x.name != y.name || x.type != y.type || x.values != y.values || x.indices != y.indices ||
            x.interpolation != y.interpolation || x.indexed != y.indexed || x.elementSize != y.elementSize ||
            x.unauthoredIndex != y.unauthoredIndex) return false;
    }
    for (size_t i = 0; i < a.subsets.size(); ++i)
        if (a.subsets[i].path != b.subsets[i].path || a.subsets[i].indices != b.subsets[i].indices) return false;
    return true;
}
bool changed(const Snapshot& source, const Result& result) {
    if (!equalMesh(source.mesh, result.mesh)) return true;
    if (result.operation != USDCC_MESH_NORMALS) return false;
    if (source.normalsChannel < 0 || source.normalsInterpolation != PXR_NS::UsdGeomTokens->faceVarying ||
        source.mesh.channels[source.normalsChannel].values != result.normals) return true;
    for (const auto& var : source.primvars) if (var.name == PXR_NS::TfToken("normals")) {
        if (var.interpolation != PXR_NS::UsdGeomTokens->faceVarying || var.channel < 0 ||
            source.mesh.channels[var.channel].values != result.normals) return true;
    }
    return false;
}
PXR_NS::SdfLayerRefPtr resultLayer(const Snapshot& snapshot, const Result& result, std::string& error) {
    PXR_NAMESPACE_USING_DIRECTIVE
    SdfLayerRefPtr layer;
    checked(error, [&] {
        checkResult(snapshot, result);
        const auto stage = UsdStage::CreateInMemory("mesh-roundtrip.usda");
        auto mesh = UsdGeomMesh(stage->OverridePrim(snapshot.path));
        VtVec3fArray points(result.mesh.points.size() / 3);
        for (size_t i = 0; i < points.size(); ++i) points[i] = GfVec3f(result.mesh.points.data() + i * 3);
        require(mesh.CreatePointsAttr().Set(points) &&
                mesh.CreateFaceVertexCountsAttr().Set(VtIntArray(result.mesh.counts.begin(), result.mesh.counts.end())) &&
                mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray(result.mesh.indices.begin(), result.mesh.indices.end())),
                "Could not author scratch geometry");
        GfVec3f lo = points[0], hi = points[0];
        for (const auto& p : points) for (int i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], p[i]); hi[i] = std::max(hi[i], p[i]); }
        require(mesh.CreateExtentAttr().Set(VtVec3fArray{lo, hi}), "Could not author extent");
        for (const auto& var : snapshot.primvars) {
            if (var.channel < 0) continue; // retain unrelated constant/inherited opinions
            const bool normals = result.operation && var.name == TfToken("normals");
            auto target = UsdGeomPrimvarsAPI(mesh).CreatePrimvar(var.name, var.type,
                normals ? UsdGeomTokens->faceVarying : var.interpolation, var.elementSize);
            // Validation proves BMesh returned these exact values and indices.
            // Retaining the original representation preserves indexing and roles.
            const VtValue value = !result.operation ? var.values : normals ? vectors<GfVec3f, 3>(result.normals) : channelValue(result.mesh.channels[var.channel]);
            require(target.Set(value), "Could not author primvar values");
            if (!result.operation && var.indexed) require(target.SetIndices(var.indices), "Could not author primvar indices");
            else {
                target.BlockIndices();
                require(!target.IsIndexed(), "Could not block weaker primvar indexing");
            }
        }
        if (snapshot.normalsChannel >= 0 || result.operation) {
            const auto& values = result.operation ? result.normals : result.mesh.channels[snapshot.normalsChannel].values;
            VtVec3fArray normals(!result.operation && snapshot.normalsInterpolation == UsdGeomTokens->constant ? 1 : values.size() / 3);
            for (size_t i = 0; i < normals.size(); ++i) normals[i] = GfVec3f(values.data() + i * 3);
            require(mesh.CreateNormalsAttr().Set(normals) && mesh.SetNormalsInterpolation(result.operation ? UsdGeomTokens->faceVarying : snapshot.normalsInterpolation),
                    "Could not author scratch normals");
        }
        for (size_t i = 0; i < snapshot.subsets.size(); ++i) {
            const auto& subset = snapshot.subsets[i];
            auto target = UsdGeomSubset(stage->OverridePrim(subset.path));
            VtIntArray indices;
            if (!result.operation) indices = subset.indices;
            else for (size_t f = 0; f < result.mesh.materials.size(); ++f) if (result.mesh.materials[f] == int(i)) indices.push_back(int(f));
            require(target.CreateIndicesAttr().Set(indices), "Could not author scratch subset");
        }
        layer = stage->GetRootLayer();
    });
    return layer;
}
bool author(const PXR_NS::UsdStageRefPtr& stage, const Snapshot& snapshot, const Result& result, std::string& error) {
    auto scratch = resultLayer(snapshot, result, error);
    if (!scratch) return false;
    return checked(error, [&] {
        const auto target = stage->GetEditTarget();
        require(target.GetLayer() && target.GetLayer()->PermissionToEdit() && target.GetMapFunction().IsIdentity(), "Authoring target is no longer writable/local");
        std::vector<PXR_NS::SdfPath> properties;
        scratch->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&](const auto& path) {
            if (scratch->GetAttributeAtPath(path)) properties.push_back(path);
        });
        PXR_NS::TfErrorMark mark;
        {
            PXR_NS::SdfChangeBlock block;
            for (const auto& path : properties) {
                auto prim = PXR_NS::SdfCreatePrimInLayer(target.GetLayer(), path.GetPrimPath());
                require(bool(prim), "Cannot create local geometry override");
                const auto source = scratch->GetAttributeAtPath(path);
                auto attribute = target.GetLayer()->GetAttributeAtPath(path);
                if (!attribute) attribute = PXR_NS::SdfAttributeSpec::New(prim, source->GetName(), source->GetTypeName(), source->GetVariability(), source->IsCustom());
                require(bool(attribute), "Cannot create local attribute override");
                for (const auto& key : source->ListInfoKeys()) attribute->SetInfo(key, source->GetInfo(key));
            }
        }
        require(mark.IsClean(), "USD rejected geometry authoring");
        require(verify(stage, snapshot, result, error), error);
    });
}
bool verify(const PXR_NS::UsdStageRefPtr& stage, const Snapshot& source, const Result& result, std::string& error) {
    return checked(error, [&] {
        auto expected = resultLayer(source, result, error);
        require(bool(expected), error);
        expected->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&](const auto& path) {
            const auto spec = expected->GetAttributeAtPath(path);
            if (!spec) return;
            auto attr = stage->GetAttributeAtPath(path);
            require(bool(attr), "Authored attribute is absent from composition: " + path.GetString());
            for (const auto& key : spec->ListInfoKeys()) {
                if (key == PXR_NS::SdfFieldKeys->Default) {
                    const PXR_NS::VtValue value = spec->GetDefaultValue();
                    if (value.IsHolding<PXR_NS::SdfValueBlock>()) require(!attr.HasValue(), "A stronger value hides the index block");
                    else { PXR_NS::VtValue composed; require(attr.Get(&composed) && composed == value, "A stronger opinion hides geometry: " + path.GetString()); }
                } else if (key != PXR_NS::SdfFieldKeys->Custom && key != PXR_NS::SdfFieldKeys->Variability && key != PXR_NS::SdfFieldKeys->TypeName) {
                    PXR_NS::VtValue composed;
                    require(attr.GetMetadata(key, &composed) && composed == spec->GetInfo(key), "Authored metadata is hidden: " + path.GetString());
                }
            }
        });
    });
}
}
