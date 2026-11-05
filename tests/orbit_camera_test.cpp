// The viewport's orbit camera: free to turn any way, with no flip or breakdown at the
// poles, and a drag that doesn't end at the screen's edge.
#include "test_harness.h"

#include "usdcc/usd/orbit_camera.h"

#include <pxr/base/gf/math.h>

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
