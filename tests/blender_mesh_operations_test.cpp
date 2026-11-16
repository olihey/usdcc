// SPDX-License-Identifier: BSD-3-Clause
#include "test_harness.h"
#include "python_test_app.h"
#include "conversion.h"
#include "jobs.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QTest>
#include <chrono>

PXR_NAMESPACE_USING_DIRECTIVE
namespace bm = usdcc::blender_mesh;
namespace {
UsdGeomMesh cube(const UsdStageRefPtr& stage, const SdfPath& path = SdfPath("/Mesh")) {
    auto mesh = UsdGeomMesh::Define(stage, path);
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
    mesh.CreatePointsAttr().Set(VtVec3fArray{{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                          {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}});
    mesh.CreateFaceVertexCountsAttr().Set(VtIntArray(6,4));
    mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray{3,2,1,0,4,5,6,7,0,1,5,4,1,2,6,5,2,3,7,6,3,0,4,7});
    return mesh;
}
UsdGeomMesh quads(const UsdStageRefPtr& stage, bool split = false) {
    auto mesh = UsdGeomMesh::Define(stage, SdfPath("/Mesh"));
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
    VtVec3fArray points{{0,0,0},{1,0,0},{2,0,0},{0,1,0},{1,1,0},{2,1,0}};
    if (split) { points.push_back(points[1]); points.push_back(points[4]); }
    mesh.CreatePointsAttr().Set(points);
    mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{4,4});
    mesh.CreateFaceVertexIndicesAttr().Set(split ? VtIntArray{0,1,4,3,6,2,5,7} : VtIntArray{0,1,4,3,1,2,5,4});
    return mesh;
}
bm::Result run(const UsdGeomMesh& mesh, UsdccMeshOptions options, bool* success = nullptr) {
    bm::Snapshot snapshot; bm::Result result; std::string error;
    const bool ok = bm::capture(mesh.GetPrim().GetStage(), mesh.GetPath(), snapshot, error) &&
                    bm::execute(snapshot.mesh, options, result, error) && bm::validateResult(snapshot, result, error);
    CHECK(ok, "operation %d: %s", options.operation, error.c_str());
    if (success) *success = ok;
    return result;
}
bool wait(bm::Jobs& jobs) {
    QElapsedTimer timer; timer.start();
    while (jobs.busy() && timer.elapsed() < 20000) QTest::qWait(1);
    CHECK(!jobs.busy(), "job finishes: %s", jobs.status().c_str());
    return !jobs.busy();
}
std::string text(const SdfLayerHandle& layer) { std::string result; layer->ExportToString(&result); return result; }
}
USDCC_TEST(blender_mesh_operations_native_contracts) {
    for (bool left : {false,true}) {
        auto stage = UsdStage::CreateInMemory(); auto mesh = quads(stage);
        mesh.CreateOrientationAttr().Set(left ? UsdGeomTokens->leftHanded : UsdGeomTokens->rightHanded);
        auto uv = UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("st"), SdfValueTypeNames->TexCoord2fArray, UsdGeomTokens->faceVarying);
        uv.Set(VtVec2fArray{{0,0},{1,0},{1,1},{0,1},{2,0},{2,1}}); uv.SetIndices(VtIntArray{0,1,2,3,0,4,5,3});
        for (int method = 0; method < 5; ++method) {
            auto options = bm::defaultOptions(USDCC_MESH_TRIANGULATE); options.quad_method = method;
            auto result = run(mesh, options);
            CHECK(result.mesh.counts == std::vector<int>({3,3,3,3}) && result.sourceCorners.size() == 12,
                  "quad method %d preserves exact UV provenance in orientation %d", method, left);
        }
        auto dissolve = run(mesh, bm::defaultOptions(USDCC_MESH_DISSOLVE));
        CHECK(dissolve.mesh.counts.size() == 2, "UV seam is protected from dissolve");
    }
    {
        auto stage = UsdStage::CreateInMemory(); auto mesh = quads(stage, true);
        auto result = run(mesh, bm::defaultOptions(USDCC_MESH_MERGE));
        CHECK(result.mesh.points.size() == 18 && result.mesh.counts.size() == 2, "merge removes duplicate boundary points");
        auto var = UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("st"), SdfValueTypeNames->Float2Array, UsdGeomTokens->faceVarying);
        var.Set(VtVec2fArray{{0,0},{1,0},{1,1},{0,1},{2,0},{3,0},{3,1},{2,1}});
        result = run(mesh, bm::defaultOptions(USDCC_MESH_MERGE));
        CHECK(result.mesh.points.size() == 24, "merge does not cross protected UV boundaries");
    }
    {
        auto stage = UsdStage::CreateInMemory(); auto mesh = quads(stage);
        auto result = run(mesh, bm::defaultOptions(USDCC_MESH_DISSOLVE));
        CHECK(result.mesh.counts.size() == 1, "coplanar edge dissolves");
        auto color = UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("color"), SdfValueTypeNames->Color3fArray, UsdGeomTokens->uniform);
        color.Set(VtVec3fArray{{1,0,0},{0,1,0}});
        result = run(mesh, bm::defaultOptions(USDCC_MESH_DISSOLVE));
        CHECK(result.mesh.counts.size() == 2, "uniform color boundary is protected");
        bm::Snapshot snapshot; std::string error; bm::capture(stage, mesh.GetPath(), snapshot, error);
        CHECK(!bm::execute(snapshot.mesh, bm::defaultOptions(USDCC_MESH_DECIMATE), result, error), "decimate refuses open/protected geometry");
    }
    {
        auto stage = UsdStage::CreateInMemory(); auto mesh = quads(stage);
        mesh.CreateNormalsAttr().Set(VtVec3fArray(8, GfVec3f(0,0,1)));
        mesh.SetNormalsInterpolation(UsdGeomTokens->faceVarying);
        auto normals = UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("normals"), SdfValueTypeNames->Normal3fArray, UsdGeomTokens->constant);
        normals.Set(VtVec3fArray{GfVec3f(1,0,0)});
        bm::Snapshot snapshot; std::string error; bm::Result result;
        CHECK(bm::capture(stage, mesh.GetPath(), snapshot, error) && bm::execute(snapshot.mesh, bm::defaultOptions(USDCC_MESH_NORMALS), result, error),
              "capture and regenerate constant normal primvar: %s", error.c_str());
        CHECK(bm::changed(snapshot, result), "wrong primvar normals cannot hide behind correct built-in normals as a no-op");
        auto layer = bm::resultLayer(snapshot, result, error);
        CHECK(layer, "normal primvar scratch result: %s", error.c_str());
        if (layer) {
            auto composed = SdfLayer::CreateAnonymous(); composed->SetSubLayerPaths({layer->GetIdentifier(),stage->GetRootLayer()->GetIdentifier()});
            auto output = UsdStage::Open(composed);
            auto var = UsdGeomPrimvarsAPI(output->GetPrimAtPath(mesh.GetPath())).GetPrimvar(TfToken("normals"));
            VtVec3fArray values;
            CHECK(var.GetInterpolation() == UsdGeomTokens->faceVarying && var.Get(&values) && values == VtVec3fArray(8,GfVec3f(0,0,1)),
                  "canonical normals primvar is regenerated with matching domain");
        }
    }
    {
        auto stage = UsdStage::CreateInMemory(); auto mesh = cube(stage);
        auto result = run(mesh, bm::defaultOptions(USDCC_MESH_BEVEL));
        CHECK(result.mesh.points.size() == 72 && result.mesh.counts.size() == 26, "native bevel matches the pinned cube fixture");
        auto options = bm::defaultOptions(USDCC_MESH_BEVEL); options.segments = 3;
        result = run(mesh, options);
        CHECK(result.mesh.counts.size() > 26 && result.normals.size() == result.mesh.indices.size() * 3, "multi-segment bevel generates normals and valid topology");
        auto filtered = options; filtered.angle = 3.0f;
        result = run(mesh, filtered);
        CHECK(result.mesh.counts.size() == 6 && result.mesh.points.size() == 24, "bevel angle filter can produce a no-op");
        result = run(mesh, bm::defaultOptions(USDCC_MESH_DECIMATE));
        CHECK(result.mesh.counts.size() < 6 && !result.diagnostic.empty(), "decimate reduces cube and reports achieved count");
        auto indices = VtIntArray{}; mesh.GetFaceVertexIndicesAttr().Get(&indices);
        std::reverse(indices.begin(), indices.begin() + 4); mesh.GetFaceVertexIndicesAttr().Set(indices);
        result = run(mesh, bm::defaultOptions(USDCC_MESH_NORMALS));
        CHECK(result.mesh.counts.size() == 6 && result.normals.size() == 72, "inconsistent closed winding repaired");
        size_t corner = 0;
        for (int count : result.mesh.counts) {
            float outward = 0;
            for (int j = 0; j < count; ++j) for (int k = 0; k < 3; ++k)
                outward += result.mesh.points[size_t(result.mesh.indices[corner+j]) * 3 + k] * result.normals[corner * 3 + k];
            CHECK(outward > 0, "recalculated closed cube normals point outward");
            corner += count;
        }
    }
    {
        auto stage = UsdStage::CreateInMemory(); auto mesh = cube(stage);
        VtVec2fArray uvs;
        for (int face = 0; face < 6; ++face)
            for (const auto& uv : {GfVec2f(0,0),GfVec2f(1,0),GfVec2f(1,1),GfVec2f(0,1)}) uvs.push_back(uv + GfVec2f(float(face * 10)));
        UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("st"), SdfValueTypeNames->TexCoord2fArray, UsdGeomTokens->faceVarying).Set(uvs);
        UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("color"), SdfValueTypeNames->Color3fArray, UsdGeomTokens->vertex)
            .Set(VtVec3fArray{{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}});
        UsdGeomSubset::SetFamilyType(mesh, TfToken("materialBind"), UsdGeomTokens->partition);
        for (int i = 0; i < 2; ++i) {
            auto subset = UsdGeomSubset::Define(stage, mesh.GetPath().AppendChild(TfToken(i ? "B" : "A")));
            subset.CreateElementTypeAttr().Set(UsdGeomTokens->face); subset.CreateFamilyNameAttr().Set(TfToken("materialBind"));
            subset.CreateIndicesAttr().Set(i ? VtIntArray{1,3,5} : VtIntArray{0,2,4});
        }
        auto adjacent = run(mesh, bm::defaultOptions(USDCC_MESH_BEVEL));
        CHECK(std::all_of(adjacent.mesh.materials.begin(), adjacent.mesh.materials.end(), [](int m) { return m == 0 || m == 1; }),
              "adjacent-material bevel preserves complete partition coverage");
        auto options = bm::defaultOptions(USDCC_MESH_BEVEL); options.bevel_material = 1;
        bool ok = false;
        auto result = run(mesh, options, &ok);
        if (!ok) return;
        CHECK(std::count(result.mesh.materials.begin(), result.mesh.materials.end(), 0) == 3 &&
              std::count(result.mesh.materials.begin(), result.mesh.materials.end(), 1) == 23, "bevel assigns the chosen existing material only to new faces");
        const auto& uv = *std::find_if(result.mesh.channels.begin(), result.mesh.channels.end(), [](const auto& c) { return c.name == "primvars:st"; });
        const auto& color = *std::find_if(result.mesh.channels.begin(), result.mesh.channels.end(), [](const auto& c) { return c.name == "primvars:color"; });
        CHECK(uv.values.size() == result.mesh.indices.size() * 2 && color.values.size() == result.mesh.points.size(), "bevel carries typed corner and point data");
        CHECK(std::all_of(color.values.begin(), color.values.end(), [](float v) { return v >= -0.001f && v <= 1.001f; }), "interpolated vertex colors remain within the source range");
        const int axis[6] = {2,2,1,0,1,0}; const float plane[6] = {-1,1,-1,1,1,-1};
        size_t start = 0; int originalIslands = 0;
        for (int count : result.mesh.counts) {
            for (int face = 0; face < 6; ++face) {
                bool onPlane = true;
                for (int j = 0; j < count; ++j) onPlane &= std::abs(result.mesh.points[size_t(result.mesh.indices[start+j]) * 3 + axis[face]] - plane[face]) < 0.00001f;
                if (onPlane) {
                    ++originalIslands;
                    for (int j = 0; j < count * 2; ++j)
                        CHECK(uv.values[start * 2 + j] >= face * 10 - 0.001f && uv.values[start * 2 + j] <= face * 10 + 1.001f,
                              "bevel preserves original face UV island %d", face);
                }
            }
            start += count;
        }
        CHECK(originalIslands == 6, "all six original UV islands remain independently represented");
    }
}
USDCC_TEST(blender_mesh_operations_jobs_transactions) {
    auto& app = usdcc::test::pythonTestApp();
    usdcc::plugin::Host host;
    host.id = "test.blender.jobs"; host.stageManager = &app.stages; host.undoManager = &app.undo;
    auto stage = app.stages.adoptStage(UsdStage::CreateInMemory()); auto mesh = cube(stage->usdStage());
    const auto before = text(stage->usdStage()->GetRootLayer());
    const auto path = mesh.GetPath();
    bm::Jobs jobs(host);
    CHECK(jobs.start(stage, {path,path}, bm::defaultOptions(USDCC_MESH_TRIANGULATE)), "deduplicated batch starts");
    if (wait(jobs)) {
        CHECK(app.undo.stack(stage)->count() == 1, "one fresh undo step after async completion: %s", jobs.status().c_str());
        CHECK(jobs.status().find("committed") != std::string::npos, "successful commit reported");
        const auto after = text(stage->usdStage()->GetRootLayer());
        app.undo.stack(stage)->undo(); CHECK(text(stage->usdStage()->GetRootLayer()) == before, "undo restores exact layer, including created attributes");
        app.undo.stack(stage)->redo(); CHECK(text(stage->usdStage()->GetRootLayer()) == after, "redo restores stored result without rerunning Blender");
        jobs.start(stage, {path}, bm::defaultOptions(USDCC_MESH_TRIANGULATE)); wait(jobs);
        CHECK(app.undo.stack(stage)->count() == 1 && jobs.status().find("No geometry") != std::string::npos, "no-op has no undo entry");
    }
    app.stages.closeStage(stage);
    for (int scenario = 0; scenario < 5; ++scenario) {
        auto current = app.stages.adoptStage(UsdStage::CreateInMemory()); cube(current->usdStage());
        const auto original = text(current->usdStage()->GetRootLayer());
        bm::Jobs worker(host);
        CHECK(worker.start(current, {path}, bm::defaultOptions(USDCC_MESH_TRIANGULATE)), "scenario %d starts", scenario);
        if (scenario == 0) worker.cancel();
        if (scenario == 1) current->usdStage()->GetPrimAtPath(path).SetCustomDataByKey(TfToken("edited"), VtValue(true));
        if (scenario == 2) app.stages.closeStage(current);
        if (scenario == 3) current->usdStage()->SetEditTarget(current->usdStage()->GetSessionLayer());
        if (scenario == 4) { auto other = app.stages.createStage(); app.stages.setCurrentStage(other); app.stages.closeStage(other); }
        const auto untouched = text(current->usdStage()->GetRootLayer());
        wait(worker);
        if (scenario < 4) CHECK(text(current->usdStage()->GetRootLayer()) == untouched, "cancel/stale/closed/target change authors nothing: %s", worker.status().c_str());
        else CHECK(app.undo.stack(current)->count() == 1, "switching current stage still commits on original stage");
        if (scenario != 2) app.stages.closeStage(current);
    }
    {
        auto current = app.stages.adoptStage(UsdStage::CreateInMemory()); cube(current->usdStage());
        auto second = cube(current->usdStage(), SdfPath("/Second"));
        VtIntArray counts; second.GetFaceVertexCountsAttr().Get(&counts);
        current->usdStage()->SetEditTarget(current->usdStage()->GetSessionLayer()); second.GetFaceVertexCountsAttr().Set(counts);
        current->usdStage()->SetEditTarget(current->usdStage()->GetRootLayer());
        const auto original = text(current->usdStage()->GetRootLayer());
        bm::Jobs worker(host); worker.start(current, {path,second.GetPath()}, bm::defaultOptions(USDCC_MESH_TRIANGULATE)); wait(worker);
        CHECK(text(current->usdStage()->GetRootLayer()) == original && app.undo.stack(current)->count() == 0,
              "stronger opinion causes full multi-mesh rollback: %s", worker.status().c_str());
        app.stages.closeStage(current);
    }
    {
        auto current = app.stages.adoptStage(UsdStage::CreateInMemory()); cube(current->usdStage());
        const auto original = text(current->usdStage()->GetRootLayer());
        { bm::Jobs worker(host); worker.start(current, {path}, bm::defaultOptions(USDCC_MESH_BEVEL)); }
        QApplication::processEvents();
        CHECK(text(current->usdStage()->GetRootLayer()) == original, "shutdown joins and cancels without late authoring");
        app.stages.closeStage(current);
    }
    {
        auto common = SdfLayer::CreateAnonymous("shared.usda");
        auto source = UsdStage::Open(common); cube(source);
        auto rootA = SdfLayer::CreateAnonymous(), rootB = SdfLayer::CreateAnonymous();
        rootA->SetSubLayerPaths({common->GetIdentifier()}); rootB->SetSubLayerPaths({common->GetIdentifier()});
        auto current = app.stages.adoptStage(UsdStage::Open(rootA));
        auto other = app.stages.adoptStage(UsdStage::Open(rootB));
        current->usdStage()->SetEditTarget(common); other->usdStage()->SetEditTarget(common);
        const auto original = text(common);
        app.undo.beginEdit(other, "Other stage edit");
        bm::Jobs worker(host); worker.start(current, {path}, bm::defaultOptions(USDCC_MESH_TRIANGULATE)); wait(worker);
        CHECK(text(common) == original && app.undo.isEditing(other) && app.undo.stack(current)->count() == 0,
              "async commit never joins another stage's recording of the shared layer");
        app.undo.cancelEdit(other); app.stages.closeStage(current); app.stages.closeStage(other);
    }
}
