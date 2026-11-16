#include <algorithm>
#include <cmath>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/sphere.h>

#include <QFile>
#include <QTemporaryDir>

#include "stage_bridge.h"
#include "usdcc/usd/undo_manager.h"
#include <QUndoStack>
#include "test_harness.h"

USDCC_TEST(rtx_native_generations_and_progressive_budgets) {
    using namespace usdcc::rtx;
    Schedule schedule;
    auto a = std::make_shared<Snapshot>();
    a->size = QSize(320, 180);
    schedule.replace(a, 100);
    schedule.active = true;
    const auto first = schedule.desired;
    schedule.replace(std::make_shared<Snapshot>(), 20);
    CHECK(!schedule.accepts(first.generation, first.revision), "a replaced scene rejects an old frame");
    auto current = schedule.desired;
    ++schedule.desired.revision;
    CHECK(!schedule.accepts(current.generation, current.revision), "a queued camera move rejects an older view");
    current = schedule.desired;
    CHECK(schedule.accepts(current.generation, current.revision), "current view can publish");
    schedule.visible = false;
    CHECK(!schedule.accepts(current.generation, current.revision), "hidden views suppress publication");
    schedule.visible = true;
    schedule.active = false;
    CHECK(!schedule.accepts(current.generation, current.revision), "Stop suppresses publication");
    schedule.active = true;
    schedule.closed = true;
    CHECK(!schedule.accepts(current.generation, current.revision), "closed panels reject late frames");
    const int expected[] = {1, 2, 4, 8, 16, 32, 64, 100};
    for (int i = 0; i < 8; ++i) CHECK(sampleBudget(i, 100) == expected[i], "progressive budget %d is exact", i);
    CHECK(sampleBudget(0, 1) == 1 && sampleBudget(30, 4096) == 4096,
          "budgets include one sample and saturate without overflow");
}

USDCC_TEST(rtx_native_snapshot_and_edits) {
    using namespace PXR_NS;
    using namespace usdcc::rtx;
    QTemporaryDir folder;
    QFile texture(folder.filePath("texture.asset"));
    CHECK(texture.open(QIODevice::WriteOnly), "create relative asset fixture");
    texture.write("asset");
    texture.close();
    auto source = UsdStage::CreateNew(folder.filePath("source.usda").toStdString());
    source->SetMetadata(TfToken("upAxis"), VtValue(TfToken("Z")));
    source->SetMetadata(TfToken("metersPerUnit"), VtValue(0.01));
    source->DefinePrim(SdfPath("/__usdccRtxNative"), TfToken("Scope"));
    auto cube = UsdGeomCube::Define(source, SdfPath("/Cube"));
    cube.GetPrim().CreateAttribute(TfToken("texture"), SdfValueTypeNames->Asset).Set(SdfAssetPath("texture.asset"));
    source->SetEditTarget(source->GetSessionLayer());
    auto sphere = UsdGeomSphere::Define(source, SdfPath("/UnsavedSphere"));
    sphere.AddTranslateOp().Set(GfVec3d(3, 2, 1));
    auto wrapped = std::make_shared<usdcc::usd::Stage>(source);
    usdcc::usd::RenderCamera camera;
    camera.cameraToWorld.SetTranslate(GfVec3d(1, 2, 10));
    camera.orthographic = true;
    camera.orthoWidth = 8;
    camera.aspect = 2;
    std::string rootBefore, sessionBefore;
    source->GetRootLayer()->ExportToString(&rootBefore);
    source->GetSessionLayer()->ExportToString(&sessionBefore);
    auto snapshot = makeSnapshot(wrapped, camera, QSize(320, 160), Mode::PathTracing);
    auto exported = UsdStage::Open(snapshot->file.toStdString());
    CHECK(snapshot->geometryKnown && snapshot->geometry.contains("/Cube") &&
          snapshot->geometry.contains("/UnsavedSphere"), "snapshot records which geometry can receive native highlights");
    CHECK(exported && exported->GetPrimAtPath(SdfPath("/UnsavedSphere")), "snapshot includes unsaved session opinions");
    CHECK(snapshot->camera.startsWith("/__usdccRtxNative_1/"), "private prims avoid collisions");
    UsdGeomCamera exportedCamera(exported->GetPrimAtPath(SdfPath(snapshot->camera.toStdString())));
    TfToken projection;
    exportedCamera.GetProjectionAttr().Get(&projection);
    CHECK(projection == UsdGeomTokens->orthographic, "orthographic projection is preserved");
    float aperture = 0;
    exportedCamera.GetHorizontalApertureAttr().Get(&aperture);
    CHECK(aperture == 80, "orthographic aperture uses tenths of scene units");
    CHECK(!snapshot->light.on && !exportedCamera.GetExposureFStopAttr().HasAuthoredValue() &&
              !exportedCamera.GetExposureTimeAttr().HasAuthoredValue() &&
              !exportedCamera.GetExposureIsoAttr().HasAuthoredValue() &&
              !exportedCamera.GetExposureResponsivityAttr().HasAuthoredValue(),
          "without light matching the private camera leaves ovrtx its own exposure");
    const LightMatch light{.on = true, .multiplier = 2.0};
    auto matched = makeSnapshot(wrapped, camera, QSize(320, 160), Mode::RealTime, light);
    auto matchedStage = UsdStage::Open(matched->file.toStdString());
    UsdGeomCamera matchedCamera(matchedStage->GetPrimAtPath(SdfPath(matched->camera.toStdString())));
    float fStop = 0, time = 0, iso = 0, responsivity = 0, ev = 0;
    matchedCamera.GetExposureFStopAttr().Get(&fStop);
    matchedCamera.GetExposureTimeAttr().Get(&time);
    matchedCamera.GetExposureIsoAttr().Get(&iso);
    matchedCamera.GetExposureResponsivityAttr().Get(&responsivity);
    matchedCamera.GetExposureAttr().Get(&ev);
    CHECK(matched->light.on && matched->light.multiplier == 2.0 && fStop == 1 && time == 1 && iso == 100 &&
              responsivity == 2 && ev == 0 && !matchedCamera.GetExposureAttr().HasAuthoredValue(),
          "light matching gives the private camera unit exposure with the multiplier as responsivity");
    CHECK(matchedCamera.ComputeLinearExposureScale() == 2.0f, "the camera's exposure scale is the multiplier");
    SdfAssetPath asset;
    exported->GetPrimAtPath(SdfPath("/Cube")).GetAttribute(TfToken("texture")).Get(&asset);
    CHECK(asset.GetAssetPath().find(folder.path().toStdString()) != std::string::npos,
          "relative assets remain anchored after export");
    TfToken axis;
    double units = 0;
    exported->GetMetadata(TfToken("upAxis"), &axis);
    exported->GetMetadata(TfToken("metersPerUnit"), &units);
    CHECK(axis == TfToken("Z") && units == .01, "units and up axis survive the snapshot");
    std::string rootAfter, sessionAfter;
    source->GetRootLayer()->ExportToString(&rootAfter);
    source->GetSessionLayer()->ExportToString(&sessionAfter);
    CHECK(rootBefore == rootAfter && sessionBefore == sessionAfter, "render setup changes neither source layer");
    usdcc::usd::StageChanges changes;
    changes.changedInfoOnlyPaths = {SdfPath("/UnsavedSphere.xformOp:translate")};
    auto edits = transformEdits(wrapped, changes);
    CHECK(edits && edits->contains("/UnsavedSphere") && edits->at("/UnsavedSphere")[12] == 3,
          "transform writes carry the current local matrix");
    changes.changedInfoOnlyPaths = {SdfPath("/Cube.visibility")};
    CHECK(!transformEdits(wrapped, changes), "unsupported changes require a full snapshot");
    changes.resyncedPaths = {SdfPath("/Cube")};
    changes.changedInfoOnlyPaths.clear();
    CHECK(!transformEdits(wrapped, changes), "add/remove/rename requires a full replacement");
    source->RemovePrim(SdfPath("/UnsavedSphere"));
    auto next = makeSnapshot(wrapped, camera, QSize(640, 320), Mode::Minimal);
    auto replacement = UsdStage::Open(next->file.toStdString());
    CHECK(!replacement->GetPrimAtPath(SdfPath("/UnsavedSphere")), "a fresh snapshot omits deleted prims");
    usdcc::usd::UndoManager undo;
    CHECK(undo.apply(wrapped, "Create", [&] { return bool(UsdGeomCube::Define(source, SdfPath("/UndoCube"))); }),
          "record an app edit");
    undo.stack(wrapped)->undo();
    auto undone = makeSnapshot(wrapped, camera, QSize(320, 160), Mode::RealTime);
    CHECK(!UsdStage::Open(undone->file.toStdString())->GetPrimAtPath(SdfPath("/UndoCube")), "snapshot reflects undo");
    undo.stack(wrapped)->redo();
    auto redone = makeSnapshot(wrapped, camera, QSize(320, 160), Mode::RealTime);
    CHECK(UsdStage::Open(redone->file.toStdString())->GetPrimAtPath(SdfPath("/UndoCube")), "snapshot reflects redo");
}

// Light matching scales the materials' emission by k (docs/RTX_LIGHT_INTENSITY.md §2.6): only
// in the snapshot, values, time samples and what feeds them alone, and nothing else.
USDCC_TEST(rtx_native_emission_scaling) {
    using namespace PXR_NS;
    using namespace usdcc::rtx;
    auto source = UsdStage::CreateInMemory();
    source->GetRootLayer()->ImportFromString(R"(#usda 1.0
def Material "A" {
    def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:emissiveColor = (0.2, 0.2, 0.2)
        color3f inputs:emissiveColor.timeSamples = { 1: (1, 1, 1) }
        color3f inputs:diffuseColor = (0.5, 0.5, 0.5)
    }
}
def Material "B" {
    def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:emissiveColor.connect = </B/Glow.outputs:rgb>
    }
    def Shader "Glow" {
        uniform token info:id = "UsdUVTexture"
        float4 inputs:bias = (0.1, 0.1, 0.1, 0.5)
        float3 outputs:rgb
    }
}
def Material "C" {
    def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </C/Shared.outputs:rgb>
        color3f inputs:emissiveColor.connect = </C/Shared.outputs:rgb>
    }
    def Shader "Shared" {
        uniform token info:id = "UsdUVTexture"
        float3 outputs:rgb
    }
}
def Material "D" {
    color3f inputs:glow = (0.5, 0.5, 0.5)
    def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:emissiveColor.connect = </D.inputs:glow>
    }
}
def Material "E" {
    def Shader "Surface" {
        uniform token info:id = "ND_standard_surface_surfaceshader"
        float inputs:emission = 1
    }
}
)");
    auto wrapped = std::make_shared<usdcc::usd::Stage>(source);
    usdcc::usd::RenderCamera camera;
    const double k = kRtxDefaultExposure;
    CHECK(std::abs(1.0 / k - 1133.8) < 0.5, "k is ovrtx's default exposure, about 1/1134: %f", 1.0 / k);

    const auto read = [](const UsdStageRefPtr& stage, const char* path, UsdTimeCode time = UsdTimeCode::Default()) {
        VtValue value;
        stage->GetAttributeAtPath(SdfPath(path)).Get(&value, time);
        return value;
    };
    const auto close = [](double a, double b) { return std::abs(a - b) <= 1e-6 * std::max(1.0, std::abs(b)); };

    auto plain = makeSnapshot(wrapped, camera, QSize(64, 32), Mode::RealTime);
    auto unmatched = UsdStage::Open(plain->file.toStdString());
    CHECK(plain->emission.scaled.empty() && close(read(unmatched, "/A/Surface.inputs:emissiveColor").Get<GfVec3f>()[0], 0.2),
          "without light matching emission stays");

    auto matched = makeSnapshot(wrapped, camera, QSize(64, 32), Mode::RealTime, LightMatch{.on = true, .multiplier = 1.0});
    auto stage = UsdStage::Open(matched->file.toStdString());
    CHECK(close(read(stage, "/A/Surface.inputs:emissiveColor").Get<GfVec3f>()[0], 0.2 * k), "a value is scaled by k");
    CHECK(close(read(stage, "/A/Surface.inputs:emissiveColor", 1.0).Get<GfVec3f>()[1], k), "and its time samples");
    CHECK(close(read(stage, "/A/Surface.inputs:diffuseColor").Get<GfVec3f>()[0], 0.5), "other inputs stay");
    const GfVec4f scale = read(stage, "/B/Glow.inputs:scale").Get<GfVec4f>();
    const GfVec4f bias = read(stage, "/B/Glow.inputs:bias").Get<GfVec4f>();
    CHECK(close(scale[0], k) && close(scale[3], 1.0), "a texture only emission uses gets its scale: %g %g", scale[0], scale[3]);
    CHECK(close(bias[1], 0.1 * k) && close(bias[3], 0.5), "and its bias, alpha left");
    const auto authored = [](const UsdStageRefPtr& on, const char* path) {
        const UsdAttribute attribute = on->GetAttributeAtPath(SdfPath(path));
        return attribute && attribute.HasAuthoredValue();
    };
    CHECK(!authored(stage, "/C/Shared.inputs:scale"),
          "a texture that feeds the diffuse too is left");
    CHECK(std::find(matched->emission.skipped.begin(), matched->emission.skipped.end(),
                    SdfPath("/C/Shared.outputs:rgb")) != matched->emission.skipped.end(),
          "and reported");
    CHECK(close(read(stage, "/D.inputs:glow").Get<GfVec3f>()[2], 0.5 * k), "a material's interface input is followed");
    CHECK(close(read(stage, "/E/Surface.inputs:emission").Get<float>(), 1.0),
          "MaterialX emission stays: ovrtx gives it in Storm's units");
    CHECK(close(read(source, "/A/Surface.inputs:emissiveColor").Get<GfVec3f>()[0], 0.2) &&
              !authored(source, "/B/Glow.inputs:scale"),
          "the app's stage is untouched");
}
