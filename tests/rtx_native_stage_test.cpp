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
