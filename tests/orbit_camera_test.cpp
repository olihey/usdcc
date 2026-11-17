// The viewport's orbit camera: free to turn any way, with no flip or breakdown at the
// poles, and a drag that doesn't end at the screen's edge.
#include "test_harness.h"

#include "usdcc/usd/orbit_camera.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range3d.h>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace usdcc::usd;
using PXR_NS::GfVec3d;

namespace {
bool closeTo(const GfVec3d& a, const GfVec3d& b, double tolerance = 1e-9) { return (a - b).GetLength() < tolerance; }
bool closeTo(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) < tolerance; }
}  // namespace

USDCC_TEST(orbit_camera_axes_are_a_frame_at_every_angle) {
    const GfVec3d target(1, 2, 3);
    for (double pitch = -720.0; pitch <= 720.0; pitch += 7.5) {
        for (double yaw = -720.0; yaw <= 720.0; yaw += 45.0) {
            const OrbitCameraAxes axes = orbitCameraAxes(target, 5.0, yaw, pitch);
            CHECK(closeTo(axes.forward.GetLength(), 1.0) && closeTo(axes.right.GetLength(), 1.0) && closeTo(axes.up.GetLength(), 1.0),
                  "unit axes, at the poles too: yaw %f pitch %f", yaw, pitch);
            CHECK(closeTo(PXR_NS::GfDot(axes.right, axes.up), 0.0) && closeTo(PXR_NS::GfDot(axes.right, axes.forward), 0.0) &&
                      closeTo(PXR_NS::GfDot(axes.up, axes.forward), 0.0),
                  "at right angles: yaw %f pitch %f", yaw, pitch);
            CHECK(closeTo(PXR_NS::GfCross(axes.right, axes.up), -axes.forward), "right-handed: yaw %f pitch %f", yaw, pitch);
            CHECK(closeTo((axes.eye - target).GetLength(), 5.0), "at its distance");
            CHECK(closeTo(target - axes.eye, axes.forward * 5.0), "looking at the target");
        }
    }
}

USDCC_TEST(orbit_camera_turns_smoothly_over_the_poles) {
    // The same as the old look-at with world up, wherever that worked.
    for (double pitch : {-89.0, -20.0, 0.0, 45.0, 89.0}) {
        const OrbitCameraAxes axes = orbitCameraAxes(GfVec3d(0), 1.0, 30.0, pitch);
        GfVec3d right = PXR_NS::GfCross(axes.forward, GfVec3d(0, 1, 0));
        right.Normalize();
        CHECK(closeTo(axes.right, right), "the same right as with world up: pitch %f", pitch);
        CHECK(closeTo(axes.up, PXR_NS::GfCross(right, axes.forward)), "and the same up");
        CHECK(axes.up[1] > 0.0, "the right way up");
    }

    // Small steps in pitch are small steps in every axis, all the way round.
    OrbitCameraAxes last = orbitCameraAxes(GfVec3d(0), 1.0, 30.0, -360.0);
    for (double pitch = -360.0 + 0.5; pitch <= 360.0; pitch += 0.5) {
        const OrbitCameraAxes axes = orbitCameraAxes(GfVec3d(0), 1.0, 30.0, pitch);
        const double step = PXR_NS::GfDegreesToRadians(0.5) * 1.001;
        CHECK((axes.up - last.up).GetLength() < step && (axes.right - last.right).GetLength() < step &&
                  (axes.forward - last.forward).GetLength() < step,
              "no jump at pitch %f", pitch);
        last = axes;
    }

    const OrbitCameraAxes over = orbitCameraAxes(GfVec3d(0), 1.0, 0.0, 135.0);
    CHECK(over.up[1] < 0.0, "past the pole it's upside down");
    const OrbitCameraAxes round = orbitCameraAxes(GfVec3d(0), 1.0, 30.0, 360.0 + 20.0);
    const OrbitCameraAxes same = orbitCameraAxes(GfVec3d(0), 1.0, 30.0, 20.0);
    CHECK(closeTo(round.eye, same.eye) && closeTo(round.up, same.up), "and a whole turn comes back");
}

USDCC_TEST(orbit_camera_starts_above_the_ground) {
    const OrbitCameraAxes axes = orbitCameraAxes(GfVec3d(0), kDefaultOrbitDistance, kDefaultOrbitYaw, kDefaultOrbitPitch);
    CHECK(axes.eye[1] > 0.0, "the eye is above the ground: y %f", axes.eye[1]);
    CHECK(axes.forward[1] < 0.0, "looking down at the target");
    CHECK(axes.up[1] > 0.0 && !orbitIsUpsideDown(kDefaultOrbitPitch), "the right way up");
}

// A Z-up stage (docs/WORKSPACES.md §3.5.2): the same camera, turned onto its ground.
USDCC_TEST(orbit_camera_follows_a_z_up_stage) {
    const GfVec3d target(1, 2, 3);
    for (double pitch = -720.0; pitch <= 720.0; pitch += 7.5) {
        for (double yaw = -720.0; yaw <= 720.0; yaw += 45.0) {
            const OrbitCameraAxes axes = orbitCameraAxes(target, 5.0, yaw, pitch, /*zUp=*/true);
            CHECK(closeTo(axes.forward.GetLength(), 1.0) && closeTo(axes.right.GetLength(), 1.0) && closeTo(axes.up.GetLength(), 1.0),
                  "unit axes: yaw %f pitch %f", yaw, pitch);
            CHECK(closeTo(PXR_NS::GfCross(axes.right, axes.up), -axes.forward), "right-handed: yaw %f pitch %f", yaw, pitch);
            CHECK(closeTo(target - axes.eye, axes.forward * 5.0), "looking at the target from its distance");
            // What the Y-up camera has along Y, this one has along Z; its front (+Z) is on -Y.
            const OrbitCameraAxes yUp = orbitCameraAxes(target, 5.0, yaw, pitch);
            const auto turned = [](const GfVec3d& v) { return GfVec3d(v[0], -v[2], v[1]); };
            CHECK(closeTo(axes.eye - target, turned(yUp.eye - target)) && closeTo(axes.right, turned(yUp.right)) &&
                      closeTo(axes.up, turned(yUp.up)),
                  "the Y-up camera, turned: yaw %f pitch %f", yaw, pitch);
            CHECK(closeTo(axes.right[2], 0.0), "a level horizon: yaw %f pitch %f", yaw, pitch);
        }
    }

    const OrbitCameraAxes start = orbitCameraAxes(GfVec3d(0), kDefaultOrbitDistance, kDefaultOrbitYaw, kDefaultOrbitPitch, true);
    CHECK(start.eye[2] > 0.0 && start.forward[2] < 0.0 && start.up[2] > 0.0, "it starts above the XY ground, Z up the screen");
    for (double yaw = -180.0; yaw <= 180.0; yaw += 30.0) {
        const OrbitCameraAxes axes = orbitCameraAxes(GfVec3d(0), kDefaultOrbitDistance, yaw, kDefaultOrbitPitch, true);
        CHECK(closeTo(axes.eye[2], start.eye[2]) && axes.up[2] > 0.0, "turning keeps its height: yaw %f", yaw);
    }

    // And back, from where the eye is.
    for (double pitch : {-80.0, -20.0, 0.0, 35.0, 80.0}) {
        for (double yaw : {-150.0, -45.0, 0.0, 60.0, 170.0}) {
            const OrbitAngles angles = orbitAnglesLookingFrom(orbitCameraAxes(GfVec3d(0), 3.0, yaw, pitch, true).eye, 0.0, true);
            CHECK(closeTo(angles.yaw, yaw, 1e-9) && closeTo(angles.pitch, pitch, 1e-9), "the angles of yaw %f pitch %f", yaw, pitch);
        }
    }
    const OrbitAngles above = orbitAnglesLookingFrom(GfVec3d(0, 0, 4), 33.0, true);
    CHECK(closeTo(above.pitch, 90.0) && closeTo(above.yaw, 33.0), "straight above is +Z, and keeps the yaw");
    const OrbitAngles front = orbitAnglesLookingFrom(GfVec3d(0, -1, 0), 33.0, true);
    CHECK(closeTo(front.pitch, 0.0) && closeTo(front.yaw, 0.0), "the front is -Y");
}

USDCC_TEST(orbit_camera_upside_down) {
    CHECK(!orbitIsUpsideDown(0.0) && !orbitIsUpsideDown(89.0) && !orbitIsUpsideDown(-89.0), "the right way up");
    CHECK(orbitIsUpsideDown(91.0) && orbitIsUpsideDown(180.0) && orbitIsUpsideDown(-135.0), "upside down");
    CHECK(!orbitIsUpsideDown(360.0) && orbitIsUpsideDown(540.0), "however far round");
}

USDCC_TEST(orbit_camera_wraps_angles) {
    CHECK(closeTo(wrapDegrees(0.0), 0.0) && closeTo(wrapDegrees(179.0), 179.0) && closeTo(wrapDegrees(-179.0), -179.0), "inside stays");
    CHECK(closeTo(wrapDegrees(180.0), 180.0) && closeTo(wrapDegrees(-180.0), 180.0), "one half-turn is one angle");
    CHECK(closeTo(wrapDegrees(361.0), 1.0) && closeTo(wrapDegrees(-361.0), -1.0) && closeTo(wrapDegrees(720.0), 0.0), "whole turns go");
    CHECK(closeTo(wrapDegrees(100000.5), wrapDegrees(100000.5 - 360.0)), "however large");
    for (double degrees = -2000.0; degrees < 2000.0; degrees += 13.0) {
        const double wrapped = wrapDegrees(degrees);
        CHECK(wrapped > -180.0 && wrapped <= 180.0, "in range: %f", wrapped);
        CHECK(closeTo(std::sin(PXR_NS::GfDegreesToRadians(wrapped)), std::sin(PXR_NS::GfDegreesToRadians(degrees)), 1e-9), "the same angle");
    }
}

USDCC_TEST(orbit_camera_cursor_wraps_at_screen_edges) {
    // A screen that isn't at the origin, as a second monitor can be.
    constexpr int left = 100, top = 50, right = 1099, bottom = 849;
    CHECK(!wrapCursorAtEdges(500, 400, left, top, right, bottom), "in the middle: nothing");
    CHECK(!wrapCursorAtEdges(left + 1, top + 1, left, top, right, bottom), "just inside: nothing");
    CHECK(!wrapCursorAtEdges(right - 1, bottom - 1, left, top, right, bottom), "on either side");

    auto at = wrapCursorAtEdges(right, 400, left, top, right, bottom);
    CHECK(at && at->x == left + 2 && at->y == 400, "the right edge goes to the left, on the same line");
    at = wrapCursorAtEdges(left, 400, left, top, right, bottom);
    CHECK(at && at->x == right - 2 && at->y == 400, "the left edge to the right");
    at = wrapCursorAtEdges(500, bottom, left, top, right, bottom);
    CHECK(at && at->x == 500 && at->y == top + 2, "the bottom to the top");
    at = wrapCursorAtEdges(500, top, left, top, right, bottom);
    CHECK(at && at->x == 500 && at->y == bottom - 2, "the top to the bottom");
    at = wrapCursorAtEdges(right, bottom, left, top, right, bottom);
    CHECK(at && at->x == left + 2 && at->y == top + 2, "a corner, both ways");
    at = wrapCursorAtEdges(right + 300, 400, left, top, right, bottom);
    CHECK(at && at->x == left + 2, "a quick move past the edge, onto another screen");

    // It lands where it won't wrap straight back.
    for (const auto& [x, y] : {std::pair{right, 400}, std::pair{left, 400}, std::pair{500, top}, std::pair{500, bottom}}) {
        const auto first = wrapCursorAtEdges(x, y, left, top, right, bottom);
        CHECK(first && !wrapCursorAtEdges(first->x, first->y, left, top, right, bottom), "no ping-pong");
    }
}

namespace {
// The frustum the viewport builds for an orbit camera (HydraViewportWindow::currentFrustum()).
PXR_NS::GfFrustum orbitFrustum(const OrbitFraming& framing, double yaw, double pitch, double fovY, double aspect) {
    const auto [eye, forward, right, up] = orbitCameraAxes(framing.target, framing.distance, yaw, pitch);
    PXR_NS::GfMatrix4d cameraToWorld(1.0);
    cameraToWorld.SetRow(0, PXR_NS::GfVec4d(right[0], right[1], right[2], 0.0));
    cameraToWorld.SetRow(1, PXR_NS::GfVec4d(up[0], up[1], up[2], 0.0));
    cameraToWorld.SetRow(2, PXR_NS::GfVec4d(-forward[0], -forward[1], -forward[2], 0.0));
    cameraToWorld.SetRow(3, PXR_NS::GfVec4d(eye[0], eye[1], eye[2], 1.0));
    PXR_NS::GfFrustum frustum;
    frustum.SetPositionAndRotationFromMatrix(cameraToWorld);
    const OrbitClipping clipping = orbitClipping(framing.distance);
    frustum.SetPerspective(fovY, true, aspect, clipping.nearClip, clipping.farClip);
    return frustum;
}
}  // namespace

USDCC_TEST(orbit_camera_frames_a_box) {
    const PXR_NS::GfRange3d box(GfVec3d(-1, 0, 2), GfVec3d(3, 2, 4));
    for (double aspect : {0.4, 1.0, 1.6, 3.0}) {
        for (double size : {0.001, 1.0, 1000.0}) {
            const PXR_NS::GfRange3d scaled(box.GetMin() * size, box.GetMax() * size);
            const OrbitFraming framing = frameOrbitCamera(scaled, 45.0, aspect, 10.0);
            CHECK(closeTo(framing.target, scaled.GetMidpoint()), "aimed at the middle");
            const PXR_NS::GfMatrix4d toNdc =
                orbitFrustum(framing, 30.0, 20.0, 45.0, aspect).ComputeViewMatrix() *
                orbitFrustum(framing, 30.0, 20.0, 45.0, aspect).ComputeProjectionMatrix();
            double widest = 0.0;
            for (int corner = 0; corner < 8; ++corner) {
                const GfVec3d ndc = toNdc.Transform(scaled.GetCorner(corner));
                widest = std::max({widest, std::abs(ndc[0]), std::abs(ndc[1])});
                CHECK(std::abs(ndc[0]) <= 1.0 && std::abs(ndc[1]) <= 1.0 && ndc[2] > -1.0 && ndc[2] < 1.0,
                      "every corner in view and between the clipping planes: aspect %f size %f", aspect, size);
            }
            CHECK(widest > 0.5, "and it fills the view, rather than being lost in it: %f", widest);
        }
    }

    const OrbitFraming square = frameOrbitCamera(box, 45.0, 1.0, 10.0);
    const double radius = box.GetSize().GetLength() / 2.0;
    CHECK(closeTo(square.distance, radius / std::sin(PXR_NS::GfDegreesToRadians(22.5))), "the sphere touches the view");
    CHECK(closeTo(frameOrbitCamera(box, 45.0, 2.0, 10.0).distance, square.distance), "a wide view is fitted by its height");
    CHECK(frameOrbitCamera(box, 45.0, 0.5, 10.0).distance > square.distance * 1.5, "a tall one by its width");

    const OrbitFraming point = frameOrbitCamera(PXR_NS::GfRange3d(GfVec3d(1, 2, 3), GfVec3d(1, 2, 3)), 45.0, 1.0, 7.0);
    CHECK(closeTo(point.target, GfVec3d(1, 2, 3)) && closeTo(point.distance, 7.0), "a point: aimed at, from where it was");
}

USDCC_TEST(orbit_camera_clipping_follows_the_distance) {
    const OrbitClipping atDefault = orbitClipping(kDefaultOrbitDistance);
    CHECK(closeTo(atDefault.nearClip, 0.1) && closeTo(atDefault.farClip, 10000.0), "0.1 to 10000 at the default distance");
    const OrbitClipping further = orbitClipping(kDefaultOrbitDistance * 100.0);
    CHECK(closeTo(further.nearClip, 10.0) && closeTo(further.farClip, 1e6), "further back, both scale");
    const OrbitClipping closer = orbitClipping(kDefaultOrbitDistance / 100.0);
    CHECK(closeTo(closer.nearClip, 0.001) && closeTo(closer.farClip, 10000.0), "closer in, only the near plane");
}

// The named views (docs/WORKSPACES.md §3.5.2): the eye on the side the table says, for
// both up axes, and their names both ways.
USDCC_TEST(orbit_camera_named_views) {
    for (bool zUp : {false, true}) {
        for (CameraView view : kCameraViews) {
            CHECK(cameraViewFromName(cameraViewName(view)) == view, "%s by its name", cameraViewName(view));
            if (view == CameraView::Persp) continue;
            const OrbitAngles angles = orbitAnglesForView(view, zUp);
            const OrbitCameraAxes axes = orbitCameraAxes(GfVec3d(0), 1.0, angles.yaw, angles.pitch, zUp);
            // About the up axis, the angles don't depend on which it is.
            const OrbitAngles other = orbitAnglesForView(view, !zUp);
            CHECK(closeTo(angles.yaw, other.yaw) && closeTo(angles.pitch, other.pitch), "%s: the same angles on either up axis",
                  cameraViewName(view));
            CHECK(closeTo(axes.eye, cameraViewEyeDirection(view, zUp), 1e-9), "%s's eye where the table says (z-up %d)",
                  cameraViewName(view), zUp);
            CHECK(cameraViewLookingFrom(cameraViewEyeDirection(view, zUp), zUp) == view, "and found from it");
        }
    }
    CHECK(!cameraViewFromName("sideways"), "no such view");
    CHECK(closeTo(cameraViewEyeDirection(CameraView::Top, false), GfVec3d(0, 1, 0)) &&
              closeTo(cameraViewEyeDirection(CameraView::Top, true), GfVec3d(0, 0, 1)),
          "Top is above the ground on either up axis");
    CHECK(closeTo(cameraViewEyeDirection(CameraView::Front, false), GfVec3d(0, 0, 1)) &&
              closeTo(cameraViewEyeDirection(CameraView::Front, true), GfVec3d(0, -1, 0)),
          "Front on +Z, or on -Y for Z-up, as Maya");
    // Y-up Top: X to the right, -Z up the screen. Z-up: Z up the screen from every side, and Top has Y up it.
    OrbitAngles top = orbitAnglesForView(CameraView::Top, false);
    CHECK(closeTo(orbitCameraAxes(GfVec3d(0), 1, top.yaw, top.pitch).right, GfVec3d(1, 0, 0)), "Top: X to the right");
    CHECK(closeTo(orbitCameraAxes(GfVec3d(0), 1, top.yaw, top.pitch).up, GfVec3d(0, 0, -1)), "Top: -Z up");
    for (CameraView side : {CameraView::Front, CameraView::Back, CameraView::Right, CameraView::Left}) {
        const OrbitAngles angles = orbitAnglesForView(side, true);
        CHECK(closeTo(orbitCameraAxes(GfVec3d(0), 1, angles.yaw, angles.pitch, true).up, GfVec3d(0, 0, 1)), "Z-up %s: Z up",
              cameraViewName(side));
    }
    top = orbitAnglesForView(CameraView::Top, true);
    const OrbitCameraAxes zTop = orbitCameraAxes(GfVec3d(0), 1, top.yaw, top.pitch, true);
    CHECK(closeTo(zTop.right, GfVec3d(1, 0, 0)) && closeTo(zTop.up, GfVec3d(0, 1, 0)), "Z-up Top: X to the right, Y up");
    const OrbitAngles persp = orbitAnglesForView(CameraView::Persp, true);
    CHECK(persp.yaw == kDefaultOrbitYaw && persp.pitch == kDefaultOrbitPitch, "Persp: the default angles");
}

USDCC_TEST(orbit_camera_orthographic_framing) {
    const OrbitAngles front = orbitAnglesForView(CameraView::Front, false);
    const OrbitCameraAxes axes = orbitCameraAxes(GfVec3d(0), 10.0, front.yaw, front.pitch);
    // A box 4 wide, 1 high and 9 deep, seen from the front in a view twice as wide as high.
    const PXR_NS::GfRange3d box(GfVec3d(-2, 0, -4.5), GfVec3d(2, 1, 4.5));
    OrthographicFraming framing = frameOrthographic(box, axes, 2.0, 7.0);
    CHECK(closeTo(framing.target, GfVec3d(0, 0.5, 0)), "at the box's centre");
    CHECK(closeTo(framing.size, 2.2, 1e-9), "its width fits (4 / 2), with a tenth more: %f", framing.size);
    framing = frameOrthographic(box, axes, 8.0, 7.0);
    CHECK(closeTo(framing.size, 1.1, 1e-9), "or its height in a wide view: %f", framing.size);
    framing = frameOrthographic(PXR_NS::GfRange3d(GfVec3d(1), GfVec3d(1)), axes, 1.0, 7.0);
    CHECK(framing.size == 7.0, "a point keeps the size");
    CHECK(closeTo(perspectiveDistanceFor(orthographicSizeFor(8.0, 45.0), 45.0), 8.0, 1e-9), "size and distance, both ways");
}

USDCC_TEST(orbit_camera_which_views_tumble) {
    CHECK(cameraViewTumbles(CameraView::Persp, false), "Persp tumbles");
    CHECK(cameraViewTumbles(CameraView::Persp, true), "so does Persp switched to Orthographic");
    CHECK(cameraViewTumbles(CameraView::Top, false), "and Top (Persp)");
    CHECK(!cameraViewTumbles(CameraView::Top, true), "but an orthographic Top stays a Top view");
    CHECK(!cameraViewTumbles(CameraView::Front, true), "as does an orthographic Front");
}
