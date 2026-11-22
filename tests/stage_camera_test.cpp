// Looking through a stage's camera and moving between it and the orbit camera
// (docs/VIEWPORT_CAMERA.md §2.3, §3): the pose and lens maths, and setting a camera prim
// from the view (Copy View to Camera, New Camera from View).
#include "test_harness.h"

#include "usdcc/usd/camera_framing.h"
#include "usdcc/usd/orbit_camera.h"
#include "usdcc/usd/stage_camera.h"

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/math.h>
#include <pxr/base/gf/ray.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace usdcc::usd;
using PXR_NS::GfCamera;
using PXR_NS::GfFrustum;
using PXR_NS::GfMatrix4d;
using PXR_NS::GfVec3d;

namespace {
bool closeTo(const GfVec3d& a, const GfVec3d& b, double tolerance = 1e-6) { return (a - b).GetLength() < tolerance; }
bool closeTo(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) < tolerance; }
bool closeTo(const GfMatrix4d& a, const GfMatrix4d& b, double tolerance = 1e-5) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::abs(a[r][c] - b[r][c]) > tolerance * std::max(1.0, std::abs(b[r][c]))) return false;
    return true;
}
bool sameWindow(const GfFrustum& a, const GfFrustum& b, double tolerance = 1e-5) {
    const PXR_NS::GfRange2d wa = a.GetWindow(), wb = b.GetWindow();
    return std::abs(wa.GetMin()[0] - wb.GetMin()[0]) < tolerance && std::abs(wa.GetMin()[1] - wb.GetMin()[1]) < tolerance &&
           std::abs(wa.GetMax()[0] - wb.GetMax()[0]) < tolerance && std::abs(wa.GetMax()[1] - wb.GetMax()[1]) < tolerance;
}
// The orbit camera's frustum, as HydraViewportWindow builds it.
GfFrustum orbitFrustum(const OrbitCameraAxes& axes, bool orthographic, double fovY, double orthoSize, double aspect) {
    GfFrustum frustum;
    frustum.SetPositionAndRotationFromMatrix(orbitCameraToWorld(axes));
    if (orthographic) {
        frustum.SetOrthographic(-orthoSize * aspect / 2, orthoSize * aspect / 2, -orthoSize / 2, orthoSize / 2, 0.1, 1000.0);
    } else {
        frustum.SetPerspective(fovY, true, aspect, 0.1, 1000.0);
    }
    return frustum;
}
PXR_NS::UsdStageRefPtr cameraStage(const std::string& body) {
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    layer->ImportFromString("#usda 1.0\n" + body);
    return PXR_NS::UsdStage::Open(layer);
}
GfMatrix4d worldOf(const PXR_NS::UsdStageRefPtr& stage, const char* path) {
    return PXR_NS::UsdGeomCamera(stage->GetPrimAtPath(PXR_NS::SdfPath(path))).GetCamera(PXR_NS::UsdTimeCode::Default()).GetTransform();
}
std::vector<std::string> opOrder(const PXR_NS::UsdStageRefPtr& stage, const char* path) {
    bool resets = false;
    std::vector<std::string> names;
    for (const auto& op : PXR_NS::UsdGeomCamera(stage->GetPrimAtPath(PXR_NS::SdfPath(path))).GetOrderedXformOps(&resets)) {
        names.push_back(op.GetOpName().GetString());
    }
    return names;
}
// A camera somewhere, turned every way, looking through a 50 mm lens.
GfCamera someCamera() {
    GfCamera camera;
    camera.SetFocalLength(50.0f);
    camera.SetTransform(GfMatrix4d(1.0).SetRotate(PXR_NS::GfRotation(GfVec3d(0.3, 1.0, 0.2), 37.0)) *
                        GfMatrix4d(1.0).SetTranslate(GfVec3d(4.0, 3.0, -2.0)));
    return camera;
}
}  // namespace

USDCC_TEST(orbit_camera_roll_turns_about_the_view) {
    for (bool zUp : {false, true}) {
        const OrbitCameraAxes level = orbitCameraAxes(GfVec3d(1, 2, 3), 5.0, 30.0, 20.0, zUp);
        const OrbitCameraAxes rolled = orbitCameraAxes(GfVec3d(1, 2, 3), 5.0, 30.0, 20.0, zUp, 90.0);
        CHECK(closeTo(rolled.eye, level.eye) && closeTo(rolled.forward, level.forward), "rolling keeps the eye and the view");
        CHECK(closeTo(rolled.up, level.right), "a quarter roll turns up onto right");
        CHECK(closeTo(PXR_NS::GfCross(rolled.right, rolled.up), -rolled.forward), "still right-handed");
    }
}

USDCC_TEST(stage_camera_frustum_fits_the_aperture_in_the_window) {
    GfCamera camera;  // 36 x 24 mm (3:2)
    camera.SetHorizontalAperture(36.0f);
    camera.SetVerticalAperture(24.0f);
    camera.SetFocalLength(36.0f);
    // A wider window: the frame's height fills it, with more at the sides.
    const PXR_NS::GfRange2d wide = stageCameraFrustum(camera, 2.0).GetWindow();
    CHECK(closeTo(wide.GetSize()[1], 24.0 / 36.0) && closeTo(wide.GetSize()[0], 2.0 * 24.0 / 36.0), "wide: %f x %f",
          wide.GetSize()[0], wide.GetSize()[1]);
    // A taller one: its width fills it, with more above and below.
    const PXR_NS::GfRange2d tall = stageCameraFrustum(camera, 1.0).GetWindow();
    CHECK(closeTo(tall.GetSize()[0], 1.0) && closeTo(tall.GetSize()[1], 1.0), "tall: %f x %f", tall.GetSize()[0], tall.GetSize()[1]);
}

// Detach (§2.1): the orbit camera takes the stage camera's pose and sees the same.
USDCC_TEST(orbit_pose_from_a_stage_camera_sees_the_same) {
    for (bool zUp : {false, true}) {
        for (double aspect : {0.6, 1.5, 2.4}) {
            const GfCamera camera = someCamera();
            const GfFrustum seen = stageCameraFrustum(camera, aspect);
            const OrbitPose pose = orbitPoseFromFrustum(seen, 7.0, 0.0, zUp);
            CHECK(!pose.orthographic && pose.fovY.has_value(), "perspective, with a field of view");
            CHECK(closeTo(pose.distance, 7.0), "the target where asked");
            const OrbitCameraAxes axes = orbitCameraAxes(pose.target, pose.distance, pose.yaw, pose.pitch, zUp, pose.roll);
            CHECK(closeTo(orbitCameraToWorld(axes), seen.ComputeViewInverse()), "the same placement, roll included: zUp %d aspect %f",
                  zUp, aspect);
            CHECK(sameWindow(orbitFrustum(axes, false, *pose.fovY, 0.0, aspect), seen), "the same window: aspect %f", aspect);
        }
    }
}

USDCC_TEST(orbit_pose_looking_straight_down_keeps_the_yaw) {
    GfCamera camera;
    // Looking down -Y from above, turned 30 degrees about Y.
    camera.SetTransform(GfMatrix4d(1.0).SetRotate(PXR_NS::GfRotation(GfVec3d(1, 0, 0), -90.0)) *
                        GfMatrix4d(1.0).SetRotate(PXR_NS::GfRotation(GfVec3d(0, 1, 0), 30.0)) *
                        GfMatrix4d(1.0).SetTranslate(GfVec3d(0, 10, 0)));
    const GfFrustum seen = stageCameraFrustum(camera, 1.5);
    const OrbitPose pose = orbitPoseFromFrustum(seen, 10.0, 12.0, false);
    CHECK(closeTo(pose.pitch, 90.0, 1e-6) && closeTo(pose.yaw, 12.0), "at the pole the yaw stays: %f", pose.yaw);
    const OrbitCameraAxes axes = orbitCameraAxes(pose.target, pose.distance, pose.yaw, pose.pitch, false, pose.roll);
    CHECK(closeTo(orbitCameraToWorld(axes), seen.ComputeViewInverse()), "and the roll makes up the rest");
    CHECK(closeTo(pose.target, GfVec3d(0, 0, 0)), "the target below it");
}

USDCC_TEST(orbit_pose_from_an_orthographic_camera) {
    GfCamera camera = someCamera();
    camera.SetProjection(GfCamera::Orthographic);
    camera.SetHorizontalAperture(200.0f);  // 20 x 10 units
    camera.SetVerticalAperture(100.0f);
    const GfFrustum seen = stageCameraFrustum(camera, 1.0);
    const OrbitPose pose = orbitPoseFromFrustum(seen, 0.0, 0.0, false);
    CHECK(pose.orthographic && !pose.fovY, "orthographic, keeping the field of view");
    CHECK(closeTo(pose.orthoSize, 20.0, 1e-5), "as high as the fitted window: %f", pose.orthoSize);
    CHECK(closeTo(pose.distance, kDefaultOrbitDistance), "no distance asked: the default");
}

// A camera under a scaled rig (a millimetre stage's "cm" variant): its lengths are in its
// own space, so the world has them scaled. Detach's orbit target is its focus distance.
USDCC_TEST(scaled_camera_lengths_are_in_its_own_space) {
    const auto stage = cameraStage(R"(
def Xform "cameras"
{
    float3 xformOp:scale = (0.1, 0.1, 0.1)
    uniform token[] xformOpOrder = ["xformOp:scale"]
    def Camera "default"
    {
        float focusDistance = 651
        float2 clippingRange = (1, 10000)
        matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, -651, 78, 1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }
}
)");
    const std::optional<GfCamera> camera = stageCameraAt(stage->GetPrimAtPath(PXR_NS::SdfPath("/cameras/default")), {});
    CHECK(camera.has_value(), "a camera");
    if (!camera) return;
    CHECK(closeTo(stageCameraScale(*camera), GfVec3d(0.1)), "the rig's scale");
    const std::optional<double> focus = stageCameraFocusDistance(*camera);
    CHECK(focus && closeTo(*focus, 65.1, 1e-4), "the focus distance in the world: %f", focus ? *focus : -1.0);
    const GfFrustum seen = stageCameraFrustum(*camera, 1.5);
    CHECK(closeTo(seen.GetNearFar().GetMin(), 0.1, 1e-6) && closeTo(seen.GetNearFar().GetMax(), 1000.0, 1e-3),
          "the clipping range in the world: %f %f", seen.GetNearFar().GetMin(), seen.GetNearFar().GetMax());
    // The focus is on the rig's origin: there the orbit target lands.
    const OrbitPose pose = orbitPoseFromFrustum(seen, *focus, 0.0, false);
    CHECK(closeTo(pose.target, GfVec3d(0.0, 0.0, 7.8), 1e-3), "the target at the focus: %f %f %f", pose.target[0], pose.target[1],
          pose.target[2]);

    GfCamera none;
    none.SetFocusDistance(0.0f);
    CHECK(!stageCameraFocusDistance(none), "no focus distance, none");
}

USDCC_TEST(scaled_orthographic_camera_sees_the_view) {
    GfCamera camera = someCamera();
    camera.SetTransform(GfMatrix4d(1.0).SetScale(0.1) * camera.GetTransform());
    camera.SetProjection(GfCamera::Orthographic);
    camera.SetHorizontalAperture(200.0f);  // 20 x 10 units of its own: 2 x 1 in the world
    camera.SetVerticalAperture(100.0f);
    CHECK(closeTo(stageCameraFrustum(camera, 2.0).GetWindow().GetSize()[1], 1.0, 1e-5), "its window in the world");

    // Copy View: the apertures in its own space, which its scale (kept) brings back.
    const OrbitCameraAxes axes = orbitCameraAxes(GfVec3d(1, 0, 2), 6.0, 40.0, 25.0, false);
    OrbitView view;
    view.cameraToWorld = orbitCameraToWorld(axes);
    view.orthographic = true;
    view.orthoSize = 8.0;
    GfCamera seeing = cameraSeeingView(camera, view, 1.5);
    seeing.SetTransform(GfMatrix4d(1.0).SetScale(0.1) * seeing.GetTransform());
    CHECK(sameWindow(stageCameraFrustum(seeing, 1.5), orbitFrustum(axes, true, 0.0, 8.0, 1.5)), "seeing the same, scaled");
}

// Copy View to Camera and New Camera from View: a camera set to see what the orbit view does.
USDCC_TEST(camera_seeing_the_view_sees_it) {
    const OrbitCameraAxes axes = orbitCameraAxes(GfVec3d(1, 0, 2), 6.0, 40.0, 25.0, false, 10.0);
    for (double aspect : {0.5, 1.0, 1.5, 3.0}) {
        for (bool orthographic : {false, true}) {
            OrbitView view;
            view.cameraToWorld = orbitCameraToWorld(axes);
            view.orthographic = orthographic;
            view.fovY = 35.0;
            view.orthoSize = 8.0;
            GfCamera camera;  // the default 3:2-ish aperture
            const GfCamera seeing = cameraSeeingView(camera, view, aspect);
            const GfFrustum frustum = stageCameraFrustum(seeing, aspect);
            CHECK(closeTo(frustum.ComputeViewInverse(), view.cameraToWorld), "placed as the view");
            CHECK(sameWindow(frustum, orbitFrustum(axes, orthographic, 35.0, 8.0, aspect), 1e-5),
                  "seeing the same: aspect %f orthographic %d", aspect, orthographic);
            CHECK(closeTo(seeing.GetHorizontalAperture() / seeing.GetVerticalAperture(),
                          camera.GetHorizontalAperture() / camera.GetVerticalAperture(), 1e-5),
                  "the aperture keeps its shape");
        }
    }
}

USDCC_TEST(set_camera_from_view_keeps_its_ops) {
    const auto stage = cameraStage(R"(
def Xform "Rig" { double3 xformOp:translate = (10, 0, 0)
    float3 xformOp:rotateXYZ = (0, 45, 0)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    def Camera "Cam" {
        double3 xformOp:translate = (0, 1, 5)
        float3 xformOp:rotateZXY = (0, 0, 0)
        float3 xformOp:scale = (2, 2, 2)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateZXY", "xformOp:scale"]
    }
}
)");
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath("/Rig/Cam"));
    CHECK(whyNotSetCamera(prim).empty(), "it can be set: %s", whyNotSetCamera(prim).c_str());
    GfCamera wanted = someCamera();
    wanted.SetFocalLength(28.0f);
    const std::string problem = setCameraFromView(prim, wanted, PXR_NS::UsdTimeCode::Default());
    CHECK(problem.empty(), "set: %s", problem.c_str());
    const GfMatrix4d world = worldOf(stage, "/Rig/Cam");
    GfMatrix4d unscaled = world;
    for (int r = 0; r < 3; ++r) {
        const GfVec3d row(world[r][0], world[r][1], world[r][2]);
        CHECK(closeTo(row.GetLength(), 2.0, 1e-5), "its own scale kept");
        for (int c = 0; c < 3; ++c) unscaled[r][c] = world[r][c] / row.GetLength();
    }
    CHECK(closeTo(unscaled, wanted.GetTransform(), 1e-5), "where the view is, under its moved and turned parent");
    CHECK((opOrder(stage, "/Rig/Cam") == std::vector<std::string>{"xformOp:translate", "xformOp:rotateZXY", "xformOp:scale"}),
          "the same ops in the same order");
    float focal = 0.0f;
    PXR_NS::UsdGeomCamera(prim).GetFocalLengthAttr().Get(&focal);
    CHECK(closeTo(focal, 28.0, 1e-5), "and the lens: %f", focal);
}

USDCC_TEST(set_camera_from_view_with_a_matrix_or_no_ops) {
    const auto stage = cameraStage(R"(
def Camera "Matrix" {
    matrix4d xformOp:transform = ((1,0,0,0),(0,1,0,0),(0,0,1,0),(3,3,3,1))
    uniform token[] xformOpOrder = ["xformOp:transform"]
}
def Camera "Bare" {}
)");
    const GfCamera wanted = someCamera();
    for (const char* path : {"/Matrix", "/Bare"}) {
        const std::string problem = setCameraFromView(stage->GetPrimAtPath(PXR_NS::SdfPath(path)), wanted, PXR_NS::UsdTimeCode::Default());
        CHECK(problem.empty(), "%s set: %s", path, problem.c_str());
        CHECK(closeTo(worldOf(stage, path), wanted.GetTransform(), 1e-5), "%s where the view is", path);
    }
    CHECK((opOrder(stage, "/Matrix") == std::vector<std::string>{"xformOp:transform"}), "the matrix stays one matrix");
    CHECK(!opOrder(stage, "/Bare").empty(), "the bare one gets ops");
}

USDCC_TEST(set_camera_from_view_refuses) {
    const auto stage = cameraStage(R"(
def Camera "Animated" {
    double3 xformOp:translate.timeSamples = {
        1: (0, 0, 0),
        10: (0, 0, 10),
    }
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
def Camera "Zoom" {
    float focalLength.timeSamples = {
        1: 35,
        10: 85,
    }
}
def Camera "Pivot" {
    double3 xformOp:translate:pivot = (1, 0, 0)
    float3 xformOp:rotateXYZ = (0, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate:pivot", "xformOp:rotateXYZ", "!invert!xformOp:translate:pivot"]
}
def Xform "NotACamera" {}
)");
    const auto why = [&](const char* path) { return whyNotSetCamera(stage->GetPrimAtPath(PXR_NS::SdfPath(path))); };
    CHECK(why("/Animated").find("animated") != std::string::npos, "an animated transform: %s", why("/Animated").c_str());
    CHECK(why("/Zoom").find("lens") != std::string::npos, "an animated lens: %s", why("/Zoom").c_str());
    CHECK(why("/Pivot").find("ops") != std::string::npos, "a pivot: %s", why("/Pivot").c_str());
    CHECK(!why("/NotACamera").empty(), "not a camera");
    const GfMatrix4d before = worldOf(stage, "/Pivot");
    CHECK(!setCameraFromView(stage->GetPrimAtPath(PXR_NS::SdfPath("/Pivot")), someCamera(), PXR_NS::UsdTimeCode::Default()).empty() &&
              closeTo(worldOf(stage, "/Pivot"), before),
          "refused, it's left as it was");
}

USDCC_TEST(set_camera_from_view_sees_a_stronger_layer) {
    // The root layer moves the camera; the edit target is a weaker sublayer.
    PXR_NS::SdfLayerRefPtr weak = PXR_NS::SdfLayer::CreateAnonymous("weak.usda");
    weak->ImportFromString("#usda 1.0\ndef Camera \"Cam\" {}\n");
    PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous("root.usda");
    root->ImportFromString(R"(#usda 1.0
over "Cam" {
    double3 xformOp:translate = (0, 0, 9)
    float3 xformOp:rotateXYZ = (0, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
}
)");
    root->InsertSubLayerPath(weak->GetIdentifier());
    const auto stage = PXR_NS::UsdStage::Open(root);
    stage->SetEditTarget(PXR_NS::UsdEditTarget(weak));
    const std::string problem = setCameraFromView(stage->GetPrimAtPath(PXR_NS::SdfPath("/Cam")), someCamera(), PXR_NS::UsdTimeCode::Default());
    CHECK(problem.find("stronger") != std::string::npos, "the edit wouldn't show: %s", problem.c_str());
}

USDCC_TEST(define_camera_from_view) {
    const auto stage = cameraStage(R"(
def Xform "Set" {
    double3 xformOp:translate = (0, 0, -4)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
def Camera "Camera" {
}
)");
    GfCamera wanted = someCamera();
    wanted.SetClippingRange(PXR_NS::GfRange1f(0.5f, 500.0f));
    const PXR_NS::SdfPath atRoot = defineCameraFromView(stage, PXR_NS::SdfPath::AbsoluteRootPath(), wanted, PXR_NS::UsdTimeCode::Default());
    CHECK(atRoot == PXR_NS::SdfPath("/Camera1"), "named after the one there: %s", atRoot.GetText());
    const PXR_NS::SdfPath underSet = defineCameraFromView(stage, PXR_NS::SdfPath("/Set"), wanted, PXR_NS::UsdTimeCode::Default());
    CHECK(underSet == PXR_NS::SdfPath("/Set/Camera"), "under a prim: %s", underSet.GetText());
    for (const PXR_NS::SdfPath& path : {atRoot, underSet}) {
        const GfCamera made = PXR_NS::UsdGeomCamera(stage->GetPrimAtPath(path)).GetCamera(PXR_NS::UsdTimeCode::Default());
        CHECK(closeTo(made.GetTransform(), wanted.GetTransform(), 1e-5), "%s where the view is", path.GetText());
        CHECK(closeTo(made.GetFocalLength(), 50.0, 1e-5) && closeTo(made.GetClippingRange().GetMin(), 0.5, 1e-6),
              "%s with the lens and clipping", path.GetText());
    }
    CHECK((stageCameraPaths(stage) == std::vector<PXR_NS::SdfPath>{PXR_NS::SdfPath("/Set/Camera"), PXR_NS::SdfPath("/Camera"),
                                                                    PXR_NS::SdfPath("/Camera1")}),
          "the stage's cameras, in order");
}

// The resolution gate and safe frames (docs/VIEWPORT_CAMERA.md §4, §6): parsing, where the gate
// sits in wide and tall windows, safe and aspect rects, and the frustum widened past the gate.
USDCC_TEST(viewport_gate_math) {
    const auto same = [](double a, double b) { return std::abs(a - b) < 1e-6; };
    const Resolution hd{1920, 1080}, small{1280, 720}, scope{2048, 858};
    CHECK(parseResolution("1920x1080") == hd && parseResolution("1280 x 720") == small &&
              parseResolution("2048\xC3\x97" "858") == scope,
          "sizes parse, with spaces or a multiplication sign");
    CHECK(!parseResolution("fill") && !parseResolution("8x8") && !parseResolution("20000x10") && !parseResolution("12x") &&
              !parseResolution("x12") && !parseResolution("1920x1080p"),
          "and nothing else");
    CHECK((resolutionName({1920, 1080}) == "1920x1080"), "and back");

    const ViewRect wide = gateRect(1000, 400, Resolution{1920, 1080});
    CHECK(same(wide.height, 400) && same(wide.width, 400 * 16.0 / 9.0) && same(wide.y, 0) &&
              same(wide.x, (1000 - wide.width) / 2.0),
          "a wide window: the gate as tall as it, centred");
    const ViewRect tall = gateRect(400, 900, Resolution{1920, 1080});
    CHECK(same(tall.width, 400) && same(tall.height, 225) && same(tall.y, (900 - 225) / 2.0), "a tall one: as wide");
    CHECK((gateRect(640, 480, std::nullopt) == ViewRect{0, 0, 640, 480}), "Fill Viewport: the window");
    const ViewRect safe = safeRect(ViewRect{0, 0, 200, 100}, 90.0);
    CHECK(same(safe.x, 10) && same(safe.y, 5) && same(safe.width, 180) && same(safe.height, 90), "90% safe");
    const ViewRect guide = aspectRect(ViewRect{0, 0, 1920, 1080}, 2.39);
    CHECK(same(guide.width, 1920) && same(guide.height, 1920 / 2.39) && same(guide.x, 0), "a 2.39 guide in 16:9");

    // A frustum that sees the gate, widened: the gate's corner lands where the gate is.
    PXR_NS::GfFrustum seen;
    seen.SetPerspective(40.0, true, tall.aspect(), 0.1, 100.0);
    const PXR_NS::GfFrustum widened = overscanFrustum(seen, tall, 400, 900);
    const PXR_NS::GfMatrix4d project = widened.ComputeViewMatrix() * widened.ComputeProjectionMatrix();
    // A point at the gate's top right corner, through the narrow frustum's own corner ray.
    const PXR_NS::GfRay corner = seen.ComputeRay(PXR_NS::GfVec2d(1.0, 1.0));
    const PXR_NS::GfVec3d point = corner.GetPoint(10.0);
    const PXR_NS::GfVec3d ndc = project.Transform(point);
    const double px = (ndc[0] + 1.0) / 2.0 * 400.0;
    const double py = (1.0 - ndc[1]) / 2.0 * 900.0;
    CHECK(std::abs(px - (tall.x + tall.width)) < 1e-3 && std::abs(py - tall.y) < 1e-3,
          "the gate's corner in the widened view: %g %g (gate %g %g)", px, py, tall.x + tall.width, tall.y);
}
