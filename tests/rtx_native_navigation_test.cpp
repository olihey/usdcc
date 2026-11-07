#include "navigation.h"
#include "stage_bridge.h"
#include "test_harness.h"
#include "usdcc/usd/stage_manager.h"
#include <pxr/base/gf/rotation.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <cmath>

USDCC_TEST(rtx_native_navigation_camera) {
    using namespace usdcc::rtx;
    using namespace PXR_NS;
    usdcc::usd::RenderCamera source;
    source.cameraToWorld.SetRotate(GfRotation(GfVec3d(0, 1, 0), 35) * GfRotation(GfVec3d(0, 0, 1), 23));
    source.cameraToWorld.SetTranslateOnly(GfVec3d(7, 4, 10));
    source.aspect = 2;
    source.fovY = 45;
    source = source.atAspect(2);
    source.prim = SdfPath("/AuthoredCamera");
    NavigationCamera camera;
    camera.seed(source, GfRange3d(GfVec3d(-2), GfVec3d(2)));
    const auto seeded = camera.camera(2);
    double error = 0;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        error += std::abs(seeded.cameraToWorld[r][c] - source.cameraToWorld[r][c]);
    CHECK(error < 1e-8, "entering Free Camera preserves the authored camera's position, orientation, and roll");
    CHECK(seeded.prim.IsEmpty(), "the viewing camera is detached from the authored camera");
    auto other = camera;
    camera.beginOrbit(); camera.orbit(QPointF(.1, .2)); camera.pan(QPointF(.02, -.03)); camera.zoom(1);
    CHECK(camera.camera(2).cameraToWorld != seeded.cameraToWorld, "orbit, pan and zoom change the local camera");
    CHECK(other.camera(2).cameraToWorld == seeded.cameraToWorld, "a duplicated camera moves independently");
    camera.frame(GfRange3d(GfVec3d(-100), GfVec3d(100)));
    const auto framed = camera.camera(2);
    CHECK(framed.farClip > 100 && framed.nearClip > 0, "scene framing scales clipping to the scene");
    camera.reset();
    auto reset = camera.camera(1);
    CHECK(!reset.orthographic && std::abs(reset.cameraToWorld.ExtractTranslation().GetLength() - 10) < 1e-8,
          "Reset Camera uses the viewport's default orbit distance");
    source.orthographic = true; source.orthoWidth = 8;
    camera.seed(source, std::nullopt); camera.zoom(1);
    CHECK(std::abs(camera.camera(2).orthoWidth - 7.2) < 1e-8, "orthographic zoom changes the viewing width");
}

USDCC_TEST(rtx_native_navigation_input_letterboxing) {
    using namespace usdcc::rtx;
    QWindow window;
    window.resize(800, 600);
    NavigationInput input(&window, &window);
    input.imageSize = [] { return QSize(1600, 800); }; // image occupies y=100..500
    int begins = 0, moves = 0;
    QPointF normalized;
    input.begin = [&](bool) { ++begins; };
    input.orbit = [&](QPointF delta) { ++moves; normalized = delta; };
    auto mouse = [&](QEvent::Type type, QPointF p, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, p, QPointF(300, 300), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &event);
    };
    mouse(QEvent::MouseButtonPress, {400, 20}, Qt::LeftButton, Qt::LeftButton);
    CHECK(begins == 0, "letterbox bars do not start navigation");
    mouse(QEvent::MouseButtonPress, {400, 300}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, {440, 320}, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {440, 320}, Qt::LeftButton, Qt::NoButton);
    CHECK(begins == 1 && moves == 1 && normalized == QPointF(.1, .05),
          "navigation uses logical image coordinates independent of device pixel ratio");
}

USDCC_TEST(rtx_native_selection_geometry_and_generations) {
    using namespace usdcc::rtx;
    using namespace PXR_NS;
    usdcc::usd::StageManager stages;
    auto wrapped = stages.createStage();
    auto stage = wrapped->usdStage();
    UsdGeomCube::Define(stage, SdfPath("/Group/Cube"));
    UsdGeomSphere::Define(stage, SdfPath("/Group/Sphere"));
    UsdGeomCamera::Define(stage, SdfPath("/Group/Camera"));
    std::string before; stage->GetRootLayer()->ExportToString(&before);
    const auto paths = selectionGeometry(wrapped, {SdfPath("/Group"), SdfPath("/Group/Cube"), SdfPath("/Missing")});
    CHECK(paths == QStringList({"/Group/Cube", "/Group/Sphere"}), "containers expand to unique renderable descendants");
    CHECK(selectionGeometry(wrapped, {}).isEmpty(), "deselection clears the outline paths");
    std::string after; stage->GetRootLayer()->ExportToString(&after);
    CHECK(before == after && stages.selectedPaths(wrapped).empty(), "highlight expansion changes neither source nor selection");
    stage->RemovePrim(SdfPath("/Group/Sphere"));
    CHECK(selectionGeometry(wrapped, {SdfPath("/Group")}) == QStringList({"/Group/Cube"}), "deleted prims are skipped");
    Schedule schedule;
    schedule.active = true;
    schedule.replace(std::make_shared<Snapshot>(), 1);
    const auto request = schedule.desired;
    ++schedule.desired.selectionRevision;
    CHECK(schedule.accepts(request.generation, request.revision) &&
          !schedule.acceptsFrame(request.generation, request.revision, request.selectionRevision),
          "a selection update rejects stale highlights without invalidating scene loading");
    schedule.visible = false;
    CHECK(!schedule.acceptsFrame(schedule.desired.generation, schedule.desired.revision,
                                schedule.desired.selectionRevision), "hidden views defer selection refreshes");
}
