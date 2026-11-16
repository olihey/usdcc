// SPDX-License-Identifier: BSD-3-Clause
#include "test_harness.h"
#include "conversion.h"
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <functional>
#include <future>
#include <limits>

PXR_NAMESPACE_USING_DIRECTIVE
namespace bm = usdcc::blender_mesh;
namespace {
UsdGeomMesh fixtureMesh(const UsdStageRefPtr& stage, const SdfPath& path = SdfPath("/Mesh")) {
    auto mesh = UsdGeomMesh::Define(stage, path);
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
    mesh.CreatePointsAttr().Set(VtVec3fArray{{0,0,0}, {1,0,0}, {2,0,0}, {0,1,0}, {1,1,0}, {2,1,0}});
    mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{4,4});
    mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray{0,1,4,3, 1,2,5,4});
    auto api = UsdGeomPrimvarsAPI(mesh);
    auto uv = api.CreatePrimvar(TfToken("st"), SdfValueTypeNames->TexCoord2fArray, UsdGeomTokens->faceVarying);
    uv.Set(VtVec2fArray{{0,0}, {1,0}, {1,1}, {0,1}, {2,0}, {2,1}});
    uv.SetIndices(VtIntArray{0,1,2,3, 0,4,5,3}); // shared points, different UVs
    auto color = api.CreatePrimvar(TfToken("displayColor"), SdfValueTypeNames->Color3fArray, UsdGeomTokens->uniform);
    color.Set(VtVec3fArray{{1,0,0}, {0,1,0}});
    color.SetIndices(VtIntArray{1,0});
    auto rgba = api.CreatePrimvar(TfToken("cornerColor"), SdfValueTypeNames->Color4fArray, UsdGeomTokens->faceVarying);
    rgba.Set(VtVec4fArray{{1,0,0,0.25f}, {0,0,1,0.75f}});
    rgba.SetIndices(VtIntArray{0,0,1,1,1,0,1,0});
    api.CreatePrimvar(TfToken("weight"), SdfValueTypeNames->FloatArray, UsdGeomTokens->vertex)
        .Set(VtFloatArray{0,1,2,3,4,5});
    api.CreatePrimvar(TfToken("label"), SdfValueTypeNames->String, UsdGeomTokens->constant).Set(std::string("keep me"));
    mesh.CreateNormalsAttr().Set(VtVec3fArray(8, GfVec3f(0,0,1)));
    mesh.SetNormalsInterpolation(UsdGeomTokens->faceVarying);
    UsdGeomXformable(mesh).AddScaleOp().Set(GfVec3f(2,3,-1));
    mesh.GetPrim().SetCustomDataByKey(TfToken("unrelated"), VtValue(std::string("preserve")));
    UsdGeomSubset::SetFamilyType(mesh, TfToken("materialBind"), UsdGeomTokens->partition);
    for (int i = 0; i < 2; ++i) {
        auto subset = UsdGeomSubset::Define(stage, path.AppendChild(TfToken(i ? "Blue" : "Red")));
        subset.CreateElementTypeAttr().Set(UsdGeomTokens->face);
        subset.CreateFamilyNameAttr().Set(TfToken("materialBind"));
        subset.CreateIndicesAttr().Set(VtIntArray{i});
        auto mat = UsdShadeMaterial::Define(stage, path.AppendChild(TfToken("Looks")).AppendChild(TfToken(i ? "Blue" : "Red")));
        auto shader = UsdShadeShader::Define(stage, mat.GetPath().AppendChild(TfToken("Surface")));
        shader.CreateIdAttr().Set(TfToken("UsdPreviewSurface"));
        shader.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(i ? GfVec3f(0,0,1) : GfVec3f(1,0,0));
        shader.CreateOutput(TfToken("surface"), SdfValueTypeNames->Token);
        mat.CreateSurfaceOutput().GetAttr().SetConnections({shader.GetPath().AppendProperty(TfToken("outputs:surface"))});
        subset.GetPrim().CreateRelationship(TfToken("material:binding")).SetTargets({mat.GetPath()});
        subset.GetPrim().SetCustomDataByKey(TfToken("keep"), VtValue(i));
    }
    return mesh;
}
std::string text(const SdfLayerHandle& layer) {
    std::string result;
    layer->ExportToString(&result);
    return result;
}
void compare(const UsdGeomMesh& a, const UsdGeomMesh& b) {
    for (const auto& getter : {UsdGeomTokens->points, UsdGeomTokens->faceVertexCounts, UsdGeomTokens->faceVertexIndices,
                              UsdGeomTokens->normals, UsdGeomTokens->orientation}) {
        VtValue x, y;
        a.GetPrim().GetAttribute(getter).Get(&x); b.GetPrim().GetAttribute(getter).Get(&y);
        CHECK(x == y, "preserve %s", getter.GetText());
    }
    for (const auto& var : UsdGeomPrimvarsAPI(a).FindPrimvarsWithInheritance()) {
        auto other = UsdGeomPrimvarsAPI(b).FindPrimvarWithInheritance(var.GetPrimvarName());
        VtValue x, y;
        CHECK(var.ComputeFlattened(&x) && other.ComputeFlattened(&y) && x == y, "flattened %s", var.GetPrimvarName().GetText());
        var.Get(&x); other.Get(&y);
        CHECK(x == y && var.GetTypeName() == other.GetTypeName() && var.GetInterpolation() == other.GetInterpolation(),
              "typed indexed representation %s", var.GetPrimvarName().GetText());
        VtIntArray ix, iy;
        var.GetIndices(&ix); other.GetIndices(&iy);
        CHECK(ix == iy && var.IsIndexed() == other.IsIndexed(), "indices %s", var.GetPrimvarName().GetText());
    }
    CHECK(UsdGeomXformCache().GetLocalToWorldTransform(a.GetPrim()) == UsdGeomXformCache().GetLocalToWorldTransform(b.GetPrim()),
          "non-uniform and reflected transforms remain on USD prim");
    CHECK(a.GetPrim().GetCustomData() == b.GetPrim().GetCustomData(), "unrelated custom data");
    for (const auto& subset : UsdGeomSubset::GetAllGeomSubsets(a)) {
        auto other = UsdGeomSubset(b.GetPrim().GetStage()->GetPrimAtPath(subset.GetPath()));
        VtIntArray x, y;
        subset.GetIndicesAttr().Get(&x); other.GetIndicesAttr().Get(&y);
        SdfPathVector tx, ty;
        subset.GetPrim().GetRelationship(TfToken("material:binding")).GetTargets(&tx);
        other.GetPrim().GetRelationship(TfToken("material:binding")).GetTargets(&ty);
        CHECK(x == y && tx.size() == 1 && tx == ty && subset.GetPrim().GetCustomData() == other.GetPrim().GetCustomData(), "subset indices, materials and metadata");
        if (tx.size() == 1 && ty.size() == 1) {
            auto ma = a.GetPrim().GetStage()->GetPrimAtPath(tx[0]);
            auto mb = b.GetPrim().GetStage()->GetPrimAtPath(ty[0]);
            SdfPathVector cx, cy;
            ma.GetAttribute(TfToken("outputs:surface")).GetConnections(&cx);
            mb.GetAttribute(TfToken("outputs:surface")).GetConnections(&cy);
            VtValue vx, vy;
            a.GetPrim().GetStage()->GetPrimAtPath(tx[0].AppendChild(TfToken("Surface"))).GetAttribute(TfToken("inputs:diffuseColor")).Get(&vx);
            b.GetPrim().GetStage()->GetPrimAtPath(ty[0].AppendChild(TfToken("Surface"))).GetAttribute(TfToken("inputs:diffuseColor")).Get(&vy);
            CHECK(cx.size() == 1 && cx == cy && !vx.IsEmpty() && vx == vy, "material network connections and shader values");
        }
    }
}
}

USDCC_TEST(blender_mesh_conversion_composed_roundtrip) {
    auto asset = UsdStage::CreateInMemory();
    fixtureMesh(asset, SdfPath("/Asset"));
    auto stage = UsdStage::CreateInMemory();
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->z);
    UsdGeomSetStageMetersPerUnit(stage, 0.01);
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    UsdGeomPrimvarsAPI(stage->GetPrimAtPath(SdfPath("/World")))
        .CreatePrimvar(TfToken("inherited"), SdfValueTypeNames->Int, UsdGeomTokens->constant).Set(42);
    stage->OverridePrim(SdfPath("/World/Mesh")).GetReferences().AddReference(asset->GetRootLayer()->GetIdentifier(), SdfPath("/Asset"));
    auto mesh = UsdGeomMesh(stage->GetPrimAtPath(SdfPath("/World/Mesh")));
    // Unsaved stronger opinions, including a session opinion, must be captured.
    auto points = VtVec3fArray{};
    mesh.GetPointsAttr().Get(&points); points[5][1] = 1.25f; mesh.GetPointsAttr().Set(points);
    stage->SetEditTarget(stage->GetSessionLayer());
    const auto assetBefore = text(asset->GetRootLayer());
    const auto rootBefore = text(stage->GetRootLayer());
    for (bool left : {false, true}) {
        mesh.GetOrientationAttr().Set(left ? UsdGeomTokens->leftHanded : UsdGeomTokens->rightHanded);
        const auto sessionBefore = text(stage->GetSessionLayer());
        bm::Snapshot snapshot;
        bm::Result result;
        std::string error;
        const bool captured = bm::capture(stage, mesh.GetPath(), snapshot, error);
        CHECK(captured, "capture composed mesh: %s", error.c_str());
        if (!captured) continue;
        CHECK(snapshot.mesh.points[16] == 1.25f && snapshot.mesh.leftHanded == left, "composed geometry and orientation");
        bool converted = true;
        for (int repeat = 0; repeat < 20 && converted; ++repeat) converted = bm::roundTrip(snapshot, result, error);
        CHECK(converted, "BMesh repeated round trip: %s", error.c_str());
        if (!converted) continue;
        auto override = bm::resultLayer(snapshot, result, error);
        CHECK(override, "validated scratch override: %s", error.c_str());
        if (!override) continue;
        auto combined = SdfLayer::CreateAnonymous();
        combined->SetSubLayerPaths({override->GetIdentifier(), stage->GetSessionLayer()->GetIdentifier(), stage->GetRootLayer()->GetIdentifier()});
        auto composed = UsdStage::Open(combined);
        compare(mesh, UsdGeomMesh(composed->GetPrimAtPath(mesh.GetPath())));
        VtVec3fArray extent;
        CHECK(UsdGeomMesh(composed->GetPrimAtPath(mesh.GetPath())).GetExtentAttr().Get(&extent) &&
              extent == VtVec3fArray({GfVec3f(0,0,0), GfVec3f(2,1.25f,0)}), "local extent recomputed");
        CHECK(text(stage->GetSessionLayer()) == sessionBefore && text(stage->GetRootLayer()) == rootBefore &&
              text(asset->GetRootLayer()) == assetBefore, "capture/conversion never author source, root, or session layers");
        auto invalid = result; invalid.sourceCorners[0] = -1;
        CHECK(!bm::resultLayer(snapshot, invalid, error), "invalid provenance cannot produce authoring layer");
        invalid = result; invalid.mesh.channels[0].values[0] += 1;
        CHECK(!bm::validateResult(snapshot, invalid, error), "attribute corruption rejected");
    }
}

USDCC_TEST(blender_mesh_conversion_rejects_unsupported_without_authoring) {
    using Edit = std::function<void(UsdStageRefPtr, UsdGeomMesh)>;
    const std::vector<std::pair<const char*, Edit>> edits = {
        {"subdivision", [](auto, auto m) { m.GetSubdivisionSchemeAttr().Set(UsdGeomTokens->catmullClark); }},
        {"time samples", [](auto, auto m) { VtVec3fArray p; m.GetPointsAttr().Get(&p); m.GetPointsAttr().Set(p, 1.0); }},
        {"holes", [](auto, auto m) { m.CreateHoleIndicesAttr().Set(VtIntArray{}); }},
        {"creases", [](auto, auto m) { m.CreateCreaseIndicesAttr().Set(VtIntArray{0,1}); }},
        {"velocity", [](auto, auto m) { m.CreateVelocitiesAttr().Set(VtVec3fArray(6)); }},
        {"bad index", [](auto, auto m) { m.GetFaceVertexIndicesAttr().Set(VtIntArray{0,1,4,99,1,2,5,4}); }},
        {"count mismatch", [](auto, auto m) { m.GetFaceVertexCountsAttr().Set(VtIntArray{4,3}); }},
        {"negative count", [](auto, auto m) { m.GetFaceVertexCountsAttr().Set(VtIntArray{-4,4}); }},
        {"negative index", [](auto, auto m) { m.GetFaceVertexIndicesAttr().Set(VtIntArray{0,1,4,-1,1,2,5,4}); }},
        {"repeated vertex", [](auto, auto m) { m.GetFaceVertexIndicesAttr().Set(VtIntArray{0,1,1,3,1,2,5,4}); }},
        {"duplicate face", [](auto, auto m) { m.GetFaceVertexIndicesAttr().Set(VtIntArray{0,1,4,3,3,4,1,0}); }},
        {"empty mesh", [](auto, auto m) { m.GetPointsAttr().Set(VtVec3fArray{}); }},
        {"zero-area face", [](auto, auto m) { m.GetPointsAttr().Set(VtVec3fArray{{0,0,0},{1,0,0},{2,0,0},{3,0,0},{4,0,0},{5,0,0}}); }},
        {"NaN", [](auto, auto m) { VtVec3fArray p; m.GetPointsAttr().Get(&p); p[0][0] = std::numeric_limits<float>::quiet_NaN(); m.GetPointsAttr().Set(p); }},
        {"infinite UV", [](auto, auto m) { UsdGeomPrimvarsAPI(m).GetPrimvar(TfToken("st")).Set(VtVec2fArray(6, GfVec2f(std::numeric_limits<float>::infinity()))); }},
        {"unsupported int channel", [](auto, auto m) { UsdGeomPrimvarsAPI(m).CreatePrimvar(TfToken("id"), SdfValueTypeNames->IntArray, UsdGeomTokens->vertex).Set(VtIntArray(6)); }},
        {"attribute count", [](auto, auto m) { UsdGeomPrimvarsAPI(m).GetPrimvar(TfToken("weight")).Set(VtFloatArray{1}); }},
        {"attribute animation", [](auto, auto m) { UsdGeomPrimvarsAPI(m).GetPrimvar(TfToken("weight")).Set(VtFloatArray(6), 1.0); }},
        {"element size", [](auto, auto m) { UsdGeomPrimvarsAPI(m).GetPrimvar(TfToken("weight")).SetElementSize(2); }},
        {"bad UV indexing", [](auto, auto m) { UsdGeomPrimvarsAPI(m).GetPrimvar(TfToken("st")).SetIndices(VtIntArray{0,1,2,3,4,5,6,7}); }},
        {"normals count", [](auto, auto m) { m.GetNormalsAttr().Set(VtVec3fArray{GfVec3f(0,0,1)}); }},
        {"overlapping subsets", [](auto s, auto) { UsdGeomSubset(s->GetPrimAtPath(SdfPath("/Mesh/Blue"))).GetIndicesAttr().Set(VtIntArray{0}); }},
        {"unrestricted family", [](auto, auto m) { UsdGeomSubset::SetFamilyType(m, TfToken("materialBind"), UsdGeomTokens->unrestricted); }},
        {"subset animation", [](auto s, auto) { UsdGeomSubset(s->GetPrimAtPath(SdfPath("/Mesh/Blue"))).GetIndicesAttr().Set(VtIntArray{1}, 1.0); }},
        {"subset out of range", [](auto s, auto) { UsdGeomSubset(s->GetPrimAtPath(SdfPath("/Mesh/Blue"))).GetIndicesAttr().Set(VtIntArray{2}); }},
        {"incomplete partition", [](auto s, auto) { UsdGeomSubset(s->GetPrimAtPath(SdfPath("/Mesh/Blue"))).GetIndicesAttr().Set(VtIntArray{}); }},
        {"unknown subset", [](auto s, auto) { UsdGeomSubset(s->GetPrimAtPath(SdfPath("/Mesh/Blue"))).GetFamilyNameAttr().Set(TfToken("selection")); }},
        {"value clips", [](auto, auto m) { VtDictionary clips; clips["default"] = VtValue(VtDictionary{}); m.GetPrim().SetMetadata(TfToken("clips"), clips); }},
        {"skel binding", [](auto, auto m) { m.GetPrim().CreateRelationship(TfToken("skel:skeleton")).SetTargets({SdfPath("/Skeleton")}); }},
        {"instanceable", [](auto, auto m) { m.GetPrim().SetInstanceable(true); }},
        {"read only", [](auto s, auto) { s->GetRootLayer()->SetPermissionToEdit(false); }},
        {"variant composition", [](auto, auto m) { auto v = m.GetPrim().GetVariantSets().AddVariantSet("shape"); v.AddVariant("a"); v.SetVariantSelection("a"); auto scope = v.GetVariantEditContext(); m.GetPrim().CreateAttribute(TfToken("tag"), SdfValueTypeNames->Int).Set(1); }},
    };
    for (const auto& [name, edit] : edits) {
        auto stage = UsdStage::CreateInMemory(); auto mesh = fixtureMesh(stage);
        edit(stage, mesh);
        const auto before = text(stage->GetRootLayer());
        bm::Snapshot snapshot; snapshot.path = SdfPath("/Unchanged");
        std::string error;
        CHECK(!bm::capture(stage, mesh.GetPath(), snapshot, error) && !error.empty(), "reject %s: %s", name, error.c_str());
        CHECK(snapshot.path == SdfPath("/Unchanged") && text(stage->GetRootLayer()) == before, "%s leaves outputs and USD untouched", name);
        stage->GetRootLayer()->SetPermissionToEdit(true);
    }
}

USDCC_TEST(blender_mesh_conversion_ngons_boundaries_and_concurrent_calls) {
    auto stage = UsdStage::CreateInMemory();
    auto mesh = UsdGeomMesh::Define(stage, SdfPath("/Mesh"));
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
    mesh.CreatePointsAttr().Set(VtVec3fArray{{0,0,0}, {1,0,0}, {2,1,0}, {1,2,0}, {0,1,0},
                                          {3,0,0}, {4,0,0}, {3,1,0}, {100,100,100}});
    mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{5,3});
    mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray{0,1,2,3,4, 5,6,7});
    bm::Snapshot snapshot; bm::Result result; std::string error;
    CHECK(bm::capture(stage, mesh.GetPath(), snapshot, error), "capture n-gon, disconnected triangle and loose point: %s", error.c_str());
    auto a = std::async(std::launch::async, [&] { bm::Result r; std::string e; return bm::roundTrip(snapshot, r, e); });
    auto b = std::async(std::launch::async, [&] { char report[512]; return usdcc_blender_mesh_probe(10, report, sizeof(report)) == 0; });
    CHECK(bm::roundTrip(snapshot, result, error) && a.get() && b.get(), "concurrent callers share serialized worker initialization: %s", error.c_str());
    CHECK(result.mesh.points.size() == 27 && result.mesh.counts == std::vector<int>({5,3}), "loose point and boundaries preserved");
    UsdccMeshInput invalid{}; char diagnostic[128];
    CHECK(usdcc_blender_mesh_validate(&invalid, diagnostic, sizeof(diagnostic)) != 0, "reject ABI mismatch");
    invalid.abi = USDCC_MESH_ABI; invalid.point_count = USDCC_MESH_MAX_POINTS + 1;
    CHECK(usdcc_blender_mesh_roundtrip(&invalid, nullptr, diagnostic, sizeof(diagnostic)) != 0, "reject oversized descriptor before pointer access");
    CHECK(usdcc_blender_mesh_validate(nullptr, diagnostic, sizeof(diagnostic)) != 0, "reject null input");
}

USDCC_TEST(blender_mesh_conversion_domains_normals_and_nonmanifold) {
    for (const auto& interpolation : {UsdGeomTokens->constant, UsdGeomTokens->uniform, UsdGeomTokens->vertex,
                                      UsdGeomTokens->varying, UsdGeomTokens->faceVarying}) {
        auto stage = UsdStage::CreateInMemory(); auto mesh = fixtureMesh(stage);
        size_t count = interpolation == UsdGeomTokens->constant ? 1 : interpolation == UsdGeomTokens->uniform ? 2 :
                       interpolation == UsdGeomTokens->faceVarying ? 8 : 6;
        mesh.GetNormalsAttr().Set(VtVec3fArray(count, GfVec3f(0,0,1)));
        mesh.SetNormalsInterpolation(interpolation);
        UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("vector"), SdfValueTypeNames->Float2Array, interpolation)
            .Set(VtVec2fArray(count, GfVec2f(0.125f, -0.5f)));
        bm::Snapshot snapshot; bm::Result result; std::string error;
        CHECK(bm::capture(stage, mesh.GetPath(), snapshot, error) && bm::roundTrip(snapshot, result, error),
              "domain %s and normals: %s", interpolation.GetText(), error.c_str());
        auto layer = bm::resultLayer(snapshot, result, error);
        CHECK(layer, "scratch output for %s: %s", interpolation.GetText(), error.c_str());
    }
    auto stage = UsdStage::CreateInMemory();
    auto mesh = UsdGeomMesh::Define(stage, SdfPath("/Mesh"));
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
    mesh.CreatePointsAttr().Set(VtVec3fArray{{0,0,0}, {1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}});
    mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{3,3,3});
    mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray{0,1,2, 1,0,3, 0,1,4});
    bm::Snapshot snapshot; bm::Result result; std::string error;
    CHECK(bm::capture(stage, mesh.GetPath(), snapshot, error) && bm::roundTrip(snapshot, result, error),
          "three radial faces on one edge can round trip: %s", error.c_str());
    auto variants = mesh.GetPrim().GetVariantSets().AddVariantSet("shape");
    variants.AddVariant("a"); variants.SetVariantSelection("a");
    stage->SetEditTarget(UsdEditTarget::ForLocalDirectVariant(stage->GetRootLayer(), SdfPath("/Mesh{shape=a}")));
    CHECK(!bm::capture(stage, mesh.GetPath(), snapshot, error), "mapped variant edit target rejected");
}
