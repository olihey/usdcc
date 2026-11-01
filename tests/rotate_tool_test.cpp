// RotateTool: ring drags compared against the exact composition of the
// rotations, in every axis order, under parents, at gimbal lock.
#include "test_harness.h"

#include "usdcc/tools/rotate_tool.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>

#include <cmath>
#include <cstdio>

using namespace PXR_NS;
using usdcc::tools::RotateTool;
using usdcc::tools::ToolContext;

namespace {


struct Cam {
    GfFrustum frustum;
    GfMatrix4d view, proj;
    GfVec3d pos;
    explicit Cam(GfVec3d eye, GfVec3d center = GfVec3d(0.0)) {
        GfMatrix4d v;
        v.SetLookAt(eye, center, GfVec3d(0, 1, 0));
        frustum.SetPerspective(60.0, 1.0, 0.1, 1000.0);
        frustum.SetPositionAndRotationFromMatrix(v.GetInverse());
        view = frustum.ComputeViewMatrix();
        proj = frustum.ComputeProjectionMatrix();
        pos = frustum.GetPosition();
    }
    GfVec2d ndc(const GfVec3d& p) const { const GfVec3d n = (view * proj).Transform(p); return GfVec2d(n[0], n[1]); }
    GfRay ray(const GfVec2d& n) const { return frustum.ComputePickRay(n); }
};

static ToolContext ctxFor(const UsdStageRefPtr& s, const char* prim, const Cam& c) {
    ToolContext ctx;
    ctx.stage = s;
    ctx.selection = {SdfPath(prim)};
    ctx.viewMatrix = c.view;
    ctx.projMatrix = c.proj;
    ctx.cameraPos = c.pos;
    return ctx;
}

static GfMatrix4d worldXf(const UsdStageRefPtr& s, const char* path) {
    return UsdGeomXformable(s->GetPrimAtPath(SdfPath(path))).ComputeLocalToWorldTransform(UsdTimeCode::Default());
}
static GfMatrix3d rotOf(const GfMatrix4d& m) {
    // Strip scale from the 3x3 part.
    GfMatrix3d r = m.ExtractRotationMatrix();
    for (int i = 0; i < 3; ++i) {
        GfVec3d row(r[i][0], r[i][1], r[i][2]);
        row.Normalize();
        for (int j = 0; j < 3; ++j) r[i][j] = row[j];
    }
    return r;
}
static double maxDiff(const GfMatrix3d& a, const GfMatrix3d& b) {
    double d = 0;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) d = std::max(d, std::abs(a[i][j] - b[i][j]));
    return d;
}
static GfMatrix3d rotAbout(const GfVec3d& axis, double deg) { return GfMatrix3d(GfRotation(axis, deg)); }
static GfVec3d eulerOf(const UsdStageRefPtr& s, const char* path) {
    bool reset = false;
    for (auto& op : UsdGeomXformable(s->GetPrimAtPath(SdfPath(path))).GetOrderedXformOps(&reset)) {
        if (op.GetOpType() != UsdGeomXformOp::TypeRotateXYZ) continue;
        if (op.GetPrecision() == UsdGeomXformOp::PrecisionFloat) { GfVec3f v(0.f); op.Get(&v); return GfVec3d(v); }
        GfVec3d v(0.0); op.Get(&v); return v;
    }
    return GfVec3d(0.0);
}
static std::string str(const GfVec3d& v) { char b[96]; std::snprintf(b, sizeof b, "(%.3f, %.3f, %.3f)", v[0], v[1], v[2]); return b; }

static UsdStageRefPtr newStage() {
    auto s = UsdStage::CreateInMemory();
    UsdGeomXform::Define(s, SdfPath("/World"));
    UsdGeomCube::Define(s, SdfPath("/World/Cube"));
    return s;
}

// Drags ring `k` (0=X,1=Y,2=Z of the prim's own current axes) by `deg` degrees,
// in `steps` mouse moves. Returns false if the ring could not be grabbed.
// If `eulerStepOut` is given, receives the largest single-move change of any Euler component.
static bool dragRing(RotateTool& t, const UsdStageRefPtr& s, const char* prim, const Cam& cam, int k, double deg,
                     int steps = 12, double* eulerStepOut = nullptr) {
    const GfMatrix4d w = worldXf(s, prim);
    const GfVec3d origin = w.ExtractTranslation();
    GfVec3d ax[3];
    for (int i = 0; i < 3; ++i) {
        GfVec3d e(0.0); e[i] = 1.0;
        ax[i] = w.TransformDir(e).GetNormalized();
    }
    const GfVec3d u = ax[(k + 1) % 3], v = ax[(k + 2) % 3];
    const double scale = std::max(0.01, (cam.pos - origin).GetLength() * 0.15);
    auto ringPt = [&](double aDeg) {
        const double a = GfDegreesToRadians(aDeg);
        return origin + (u * std::cos(a) + v * std::sin(a)) * scale;
    };
    ToolContext ctx = ctxFor(s, prim, cam);
    const double a0 = 45.0;
    const GfVec2d p0 = cam.ndc(ringPt(a0));
    if (!t.mousePress(ctx, p0, cam.ray(p0)).handled) return false;
    GfVec3d prev = eulerOf(s, prim);
    double worst = 0;
    for (int i = 1; i <= steps; ++i) {
        const GfVec2d p = cam.ndc(ringPt(a0 + deg * i / steps));
        t.mouseMove(ctx, p, cam.ray(p));
        const GfVec3d e = eulerOf(s, prim);
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(e[c] - prev[c]));
        prev = e;
    }
    t.mouseRelease();
    if (eulerStepOut) *eulerStepOut = worst;
    return true;
}

}  // namespace

USDCC_TEST(rotate_tool) {
    const Cam cam(GfVec3d(6, 5, 8));
    const char* P = "/World/Cube";

    std::puts("[1] single-axis rotations are exact");
    for (int k = 0; k < 3; ++k) {
        auto s = newStage(); RotateTool t;
        GfVec3d e(0.0); e[k] = 1.0;
        CHECK(dragRing(t, s, P, cam, k, 40), "ring %d grabbed", k);
        const double d = maxDiff(rotOf(worldXf(s, P)), rotAbout(e, 40));
        CHECK(d < 1e-3, "axis %d: |R - expected| = %g, euler=%s", k, d, str(eulerOf(s, P)).c_str());
    }

    std::puts("[2] three successive ring drags: X then Y then Z (the reported failure)");
    {
        auto s = newStage(); RotateTool t;
        GfMatrix3d expected(1.0);
        const double degs[3] = {30, 40, 50};
        for (int k = 0; k < 3; ++k) {
            GfVec3d e(0.0); e[k] = 1.0;
            expected = rotAbout(e, degs[k]) * expected;   // about the prim's own k-th axis, applied first (row vectors)
            CHECK(dragRing(t, s, P, cam, k, degs[k]), "step %d ring grabbed", k);
            const double d = maxDiff(rotOf(worldXf(s, P)), expected);
            CHECK(d < 1e-3, "after drag %d (%s by %g): |R - expected| = %g, euler=%s", k, "XYZ"[k] == 'X' ? "X" : (k == 1 ? "Y" : "Z"), degs[k], d, str(eulerOf(s, P)).c_str());
        }
    }

    std::puts("[3] all orderings of the three axes, twice around");
    {
        const int perms[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
        for (auto& p : perms) {
            auto s = newStage(); RotateTool t;
            GfMatrix3d expected(1.0);
            bool ok = true;
            for (int round = 0; round < 2; ++round) {
                for (int i = 0; i < 3; ++i) {
                    const int k = p[i];
                    GfVec3d e(0.0); e[k] = 1.0;
                    const double deg = 25 + 10 * i + 5 * round;
                    expected = rotAbout(e, deg) * expected;
                    ok = dragRing(t, s, P, cam, k, deg) && ok;
                }
            }
            const double d = maxDiff(rotOf(worldXf(s, P)), expected);
            CHECK(ok && d < 1e-3, "order %c%c%c: grabbed=%d |R - expected| = %g, euler=%s", "XYZ"[p[0]], "XYZ"[p[1]], "XYZ"[p[2]], ok, d, str(eulerOf(s, P)).c_str());
        }
    }

    std::puts("[4] no sudden Euler jumps while dragging a compound rotation");
    {
        auto s = newStage(); RotateTool t;
        dragRing(t, s, P, cam, 0, 30);
        dragRing(t, s, P, cam, 1, 35);
        double worst = 0;
        CHECK(dragRing(t, s, P, cam, 2, 120, 60, &worst), "grabbed");
        CHECK(worst < 10.0, "largest per-move Euler change %g deg (expected ~2 deg/step)", worst);
    }

    std::puts("[5] snapping applies to the swept angle, about the right axis");
    {
        auto s = newStage(); RotateTool t;
        t.setOption("snap", true); t.setOption("snapIncrement", 15.0);
        dragRing(t, s, P, cam, 0, 20);   // -> 15 about X
        dragRing(t, s, P, cam, 1, 37);   // -> 30 about (new) Y
        GfMatrix3d expected = rotAbout(GfVec3d(0, 1, 0), 30) * rotAbout(GfVec3d(1, 0, 0), 15);
        const double d = maxDiff(rotOf(worldXf(s, P)), expected);
        CHECK(d < 1e-3, "|R - expected| = %g, euler=%s", d, str(eulerOf(s, P)).c_str());
    }

    std::puts("[6] under a rotated parent");
    {
        auto s = UsdStage::CreateInMemory();
        UsdGeomXform::Define(s, SdfPath("/Parent"));
        UsdGeomCube::Define(s, SdfPath("/Parent/Child"));
        UsdGeomXformCommonAPI(s->GetPrimAtPath(SdfPath("/Parent"))).SetRotate(GfVec3f(10, 50, 20));
        const char* C = "/Parent/Child";
        RotateTool t;
        GfMatrix3d expectedLocal(1.0);
        const GfMatrix3d parent = rotOf(worldXf(s, "/Parent"));
        for (int k : {0, 1, 2}) {
            GfVec3d e(0.0); e[k] = 1.0;
            expectedLocal = rotAbout(e, 30) * expectedLocal;
            CHECK(dragRing(t, s, C, cam, k, 30), "ring %d", k);
        }
        const double d = maxDiff(rotOf(worldXf(s, C)), expectedLocal * parent);
        CHECK(d < 1e-3, "|R - expected| = %g, euler=%s", d, str(eulerOf(s, C)).c_str());
    }

    std::puts("[7] pre-existing compound Euler value (set in the file, not by the tool)");
    {
        auto s = newStage();
        UsdGeomXformCommonAPI(s->GetPrimAtPath(SdfPath(P))).SetRotate(GfVec3f(20, 35, 50));
        RotateTool t;
        const GfMatrix3d start = rotOf(worldXf(s, P));
        GfMatrix3d expected = start;
        for (int k : {2, 0, 1}) {
            GfVec3d e(0.0); e[k] = 1.0;
            expected = rotAbout(e, 25) * expected;
            CHECK(dragRing(t, s, P, cam, k, 25), "ring %d", k);
        }
        const double d = maxDiff(rotOf(worldXf(s, P)), expected);
        CHECK(d < 1e-3, "|R - expected| = %g, euler=%s", d, str(eulerOf(s, P)).c_str());
    }

    std::puts("[8] gimbal lock (Y at 90 degrees) and large sweeps");
    {
        auto s = newStage(); RotateTool t;
        GfMatrix3d expected(1.0);
        struct Step { int k; double deg; };
        for (Step st : {Step{1, 90}, Step{0, 20}, Step{2, 30}, Step{1, -25}, Step{0, 200}, Step{2, 170}, Step{1, 300}}) {
            GfVec3d e(0.0); e[st.k] = 1.0;
            expected = rotAbout(e, st.deg) * expected;
            double worst = 0;
            const bool ok = dragRing(t, s, P, cam, st.k, st.deg, 90, &worst);
            const double d = maxDiff(rotOf(worldXf(s, P)), expected);
            CHECK(ok && d < 1e-3, "%c by %g: grabbed=%d |R - expected| = %g, euler=%s", "XYZ"[st.k], st.deg, ok, d, str(eulerOf(s, P)).c_str());
        }
    }

}
