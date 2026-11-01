// MoveTool picking/dragging/snapping, ScaleTool center picking, and the
// SelectTool fallback of the transform tools, driven headlessly through a
// synthetic camera.
#include "test_harness.h"

#include "usdcc/tools/move_tool.h"
#include "usdcc/tools/rotate_tool.h"
#include "usdcc/tools/scale_tool.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>

#include <cmath>
#include <cstdio>

using namespace PXR_NS;
using usdcc::tools::MoveTool;
using usdcc::tools::ToolContext;

namespace {


static bool near3(const GfVec3d& a, const GfVec3d& b, double eps = 1e-4) { return (a - b).GetLength() < eps; }
static std::string str(const GfVec3d& v) { char b[96]; std::snprintf(b, sizeof b, "(%.4f, %.4f, %.4f)", v[0], v[1], v[2]); return b; }

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

static ToolContext ctxFor(const UsdStageRefPtr& s, std::vector<SdfPath> sel, const Cam& c) {
    ToolContext ctx;
    ctx.stage = s;
    ctx.selection = std::move(sel);
    ctx.viewMatrix = c.view;
    ctx.projMatrix = c.proj;
    ctx.cameraPos = c.pos;
    return ctx;
}

static GfVec3d worldPos(const UsdStageRefPtr& s, const char* path) {
    return UsdGeomXformable(s->GetPrimAtPath(SdfPath(path))).ComputeLocalToWorldTransform(UsdTimeCode::Default()).ExtractTranslation();
}
static GfVec3d localT(const UsdStageRefPtr& s, const char* path) {
    GfVec3d t(0.0); GfVec3f r; GfVec3f sc; GfVec3f piv; UsdGeomXformCommonAPI::RotationOrder ro;
    UsdGeomXformCommonAPI(s->GetPrimAtPath(SdfPath(path))).GetXformVectors(&t, &r, &sc, &piv, &ro, UsdTimeCode::Default());
    return t;
}
static GfVec3d rawTranslate(const UsdStageRefPtr& st, const char* path) {
    bool reset = false;
    for (auto& op : UsdGeomXformable(st->GetPrimAtPath(SdfPath(path))).GetOrderedXformOps(&reset)) {
        if (op.GetOpType() != UsdGeomXformOp::TypeTranslate) continue;
        if (op.GetPrecision() == UsdGeomXformOp::PrecisionFloat) { GfVec3f v(0.f); op.Get(&v); return GfVec3d(v); }
        GfVec3d v(0.0); op.Get(&v); return v;
    }
    return GfVec3d(0.0);
}
static size_t opCount(const UsdStageRefPtr& s, const char* path) {
    bool reset = false;
    return UsdGeomXformable(s->GetPrimAtPath(SdfPath(path))).GetOrderedXformOps(&reset).size();
}
static double gizmoScale(const Cam& c, const GfVec3d& origin) { return std::max(0.01, (c.pos - origin).GetLength() * 0.15); }

// Press at world point `from`, drag (in a few steps) to world point `to`, return the tool.
static bool drag(MoveTool& tool, const UsdStageRefPtr& s, const char* prim, const Cam& c, const GfVec3d& from, const GfVec3d& to, bool release = true) {
    ToolContext ctx = ctxFor(s, {SdfPath(prim)}, c);
    const auto press = tool.mousePress(ctx, c.ndc(from), c.ray(c.ndc(from)));
    if (!press.handled) return false;
    for (int i = 1; i <= 5; ++i) {
        const GfVec3d p = from + (to - from) * (i / 5.0);
        tool.mouseMove(ctx, c.ndc(p), c.ray(c.ndc(p)));
    }
    if (release) tool.mouseRelease();
    return true;
}

static UsdStageRefPtr newStage() {
    auto s = UsdStage::CreateInMemory();
    UsdGeomXform::Define(s, SdfPath("/World"));
    UsdGeomCube::Define(s, SdfPath("/World/Cube"));
    return s;
}

}  // namespace

USDCC_TEST(move_scale_select_tools) {
    const Cam cam(GfVec3d(6, 5, 8));
    const char* P = "/World/Cube";

    std::puts("[1] gizmo visibility");
    {
        auto s = newStage();
        MoveTool t;
        CHECK(!t.buildGizmo(ctxFor(s, {SdfPath(P)}, cam)).lines.empty(), "gizmo for 1 selected");
        CHECK(t.buildGizmo(ctxFor(s, {}, cam)).lines.empty(), "no gizmo with empty selection");
        CHECK(t.buildGizmo(ctxFor(s, {SdfPath(P), SdfPath("/World")}, cam)).lines.empty(), "no gizmo with 2 selected");
        ToolContext two = ctxFor(s, {SdfPath(P), SdfPath("/World")}, cam);
        const auto r2 = t.mousePress(two, cam.ndc(GfVec3d(0.0)), cam.ray(cam.ndc(GfVec3d(0.0))));
        CHECK(r2.newSelection && r2.newSelection->empty(), "with 2 selected a press is a selection click (picks nothing -> clears)");
    }

    std::puts("[2] axis drags (X, Y, Z)");
    {
        const double s0 = gizmoScale(cam, GfVec3d(0.0));
        struct Case { const char* name; GfVec3d axis; };
        for (const Case& c : {Case{"X", {1, 0, 0}}, Case{"Y", {0, 1, 0}}, Case{"Z", {0, 0, 1}}}) {
            auto s = newStage();
            MoveTool t;
            const GfVec3d from = c.axis * (0.5 * s0);
            CHECK(drag(t, s, P, cam, from, from + c.axis * 2.0), "%s axis picked", c.name);
            CHECK(near3(localT(s, P), c.axis * 2.0), "%s axis translate %s", c.name, str(localT(s, P)).c_str());
            CHECK(near3(worldPos(s, P), c.axis * 2.0), "%s axis world %s", c.name, str(worldPos(s, P)).c_str());
        }
    }

    std::puts("[3] negative direction + no accumulation across moves");
    {
        auto s = newStage(); MoveTool t;
        const double s0 = gizmoScale(cam, GfVec3d(0.0));
        ToolContext ctx = ctxFor(s, {SdfPath(P)}, cam);
        const GfVec3d from(0.5 * s0, 0, 0);
        CHECK(t.mousePress(ctx, cam.ndc(from), cam.ray(cam.ndc(from))).handled, "pick");
        for (double x : {2.0, 3.0, 1.0, -1.5}) {
            const GfVec3d p = from + GfVec3d(x, 0, 0);
            t.mouseMove(ctx, cam.ndc(p), cam.ray(cam.ndc(p)));
            CHECK(near3(localT(s, P), GfVec3d(x, 0, 0)), "move to %g -> %s", x, str(localT(s, P)).c_str());
        }
        t.mouseRelease();
        // After release a stray move must do nothing.
        const GfVec3d before = localT(s, P);
        t.mouseMove(ctx, cam.ndc(GfVec3d(9, 0, 0)), cam.ray(cam.ndc(GfVec3d(9, 0, 0))));
        CHECK(near3(localT(s, P), before), "move after release ignored");
    }

    std::puts("[4] plane drags");
    {
        const double s0 = gizmoScale(cam, GfVec3d(0.0));
        struct Case { const char* name; GfVec3d u, v; GfVec3d d; };
        for (const Case& c : {Case{"XY", {1, 0, 0}, {0, 1, 0}, {1, 2, 0}}, Case{"YZ", {0, 1, 0}, {0, 0, 1}, {0, 1, 2}},
                              Case{"XZ", {0, 0, 1}, {1, 0, 0}, {2, 0, 1}}}) {
            auto s = newStage(); MoveTool t;
            const GfVec3d from = (c.u + c.v) * (0.3 * s0);
            CHECK(drag(t, s, P, cam, from, from + c.d), "%s plane picked", c.name);
            CHECK(near3(localT(s, P), c.d), "%s plane translate %s", c.name, str(localT(s, P)).c_str());
        }
    }

    std::puts("[5] center (screen plane) drag");
    {
        auto s = newStage(); MoveTool t;
        const GfVec3d n = (GfVec3d(0.0) - cam.pos).GetNormalized();
        const GfVec3d d = GfCross(n, GfVec3d(0, 1, 0)).GetNormalized() * 1.5 + GfCross(n, GfCross(n, GfVec3d(0, 1, 0))).GetNormalized() * 0.7;
        CHECK(drag(t, s, P, cam, GfVec3d(0.0), d), "center picked");
        CHECK(near3(localT(s, P), d), "center translate %s expected %s", str(localT(s, P)).c_str(), str(d).c_str());
    }

    std::puts("[6] snapping");
    {
        const double s0 = gizmoScale(cam, GfVec3d(0.0));
        for (auto [drag_to, expect] : {std::pair{1.3, 1.5}, std::pair{1.2, 1.0}, std::pair{-0.8, -1.0}}) {
            auto s = newStage(); MoveTool t;
            t.setOption("snap", true);
            t.setOption("snapIncrement", 0.5);
            const GfVec3d from(0.5 * s0, 0, 0);
            drag(t, s, P, cam, from, from + GfVec3d(drag_to, 0, 0));
            CHECK(near3(localT(s, P), GfVec3d(expect, 0, 0)), "snap %g -> %s expected %g", drag_to, str(localT(s, P)).c_str(), expect);
        }
        // Snap on, drag below half an increment: no translate op may be authored.
        auto s = newStage(); MoveTool t;
        t.setOption("snap", true); t.setOption("snapIncrement", 0.5);
        const GfVec3d from(0.5 * s0, 0, 0);
        drag(t, s, P, cam, from, from + GfVec3d(0.1, 0, 0));
        CHECK(opCount(s, P) == 0, "sub-increment snapped drag authors nothing (ops=%zu)", opCount(s, P));
    }

    std::puts("[7] jitter / misses author nothing");
    {
        auto s = newStage(); MoveTool t;
        const double s0 = gizmoScale(cam, GfVec3d(0.0));
        const GfVec3d from(0.5 * s0, 0, 0);
        drag(t, s, P, cam, from, from);
        CHECK(opCount(s, P) == 0, "no-move drag authors nothing");
        ToolContext ctx = ctxFor(s, {SdfPath(P)}, cam);
        const GfVec2d farAway(0.9, -0.9);
        const auto miss = t.mousePress(ctx, farAway, cam.ray(farAway));
        CHECK(miss.newSelection && miss.newSelection->empty(), "press on empty space is a selection click");
        t.mouseRelease();
        CHECK(opCount(s, P) == 0, "miss authors nothing");
    }

    std::puts("[8] existing translate (double and float), offset origin");
    {
        for (bool useFloat : {false, true}) {
            auto s = newStage();
            UsdGeomXformable x(s->GetPrimAtPath(SdfPath(P)));
            auto op = x.AddTranslateOp(useFloat ? UsdGeomXformOp::PrecisionFloat : UsdGeomXformOp::PrecisionDouble);
            if (useFloat) op.Set(GfVec3f(5, 1, 0)); else op.Set(GfVec3d(5, 1, 0));
            MoveTool t;
            const GfVec3d o(5, 1, 0);
            const double sc = gizmoScale(cam, o);
            const GfVec3d from = o + GfVec3d(0, 0, 0.5 * sc);
            CHECK(drag(t, s, P, cam, from, from + GfVec3d(0, 0, -3)), "picked (%s)", useFloat ? "float" : "double");
            CHECK(near3(rawTranslate(s, P), GfVec3d(5, 1, -3)), "%s translate %s", useFloat ? "float" : "double", str(rawTranslate(s, P)).c_str());
            CHECK(near3(worldPos(s, P), GfVec3d(5, 1, -3)), "%s world %s", useFloat ? "float" : "double", str(worldPos(s, P)).c_str());
            CHECK(opCount(s, P) == 1, "still a single op");
        }
    }

    std::puts("[9] canonical TRS stack with rotation+scale on the prim");
    {
        auto s = newStage();
        UsdGeomXformCommonAPI api(s->GetPrimAtPath(SdfPath(P)));
        api.SetTranslate(GfVec3d(1, 2, 3));
        api.SetRotate(GfVec3f(30, 45, 60));
        api.SetScale(GfVec3f(3, 3, 3));
        MoveTool t;
        t.setOption("space", std::string("world"));
        const GfVec3d o = worldPos(s, P);
        const double sc = gizmoScale(cam, o);
        const GfVec3d from = o + GfVec3d(0.5 * sc, 0, 0);
        CHECK(drag(t, s, P, cam, from, from + GfVec3d(2, 0, 0)), "picked");
        CHECK(near3(worldPos(s, P), o + GfVec3d(2, 0, 0)), "world moved by exactly 2 along X: %s -> %s", str(o).c_str(), str(worldPos(s, P)).c_str());
        GfVec3d tr = localT(s, P);
        CHECK(near3(tr, GfVec3d(3, 2, 3)), "local translate %s", str(tr).c_str());
    }

    std::puts("[10] rotated + scaled parent, world and local space");
    for (const char* space : {"world", "local"}) {
        auto s = UsdStage::CreateInMemory();
        UsdGeomXform::Define(s, SdfPath("/Parent"));
        UsdGeomCube::Define(s, SdfPath("/Parent/Child"));
        UsdGeomXformCommonAPI pa(s->GetPrimAtPath(SdfPath("/Parent")));
        pa.SetTranslate(GfVec3d(1, 0, -2));
        pa.SetRotate(GfVec3f(0, 90, 0));
        pa.SetScale(GfVec3f(2, 2, 2));
        UsdGeomXformCommonAPI(s->GetPrimAtPath(SdfPath("/Parent/Child"))).SetTranslate(GfVec3d(1, 1, 0));
        MoveTool t;
        t.setOption("space", std::string(space));
        const char* C = "/Parent/Child";
        const GfVec3d o = worldPos(s, C);
        const double sc = gizmoScale(cam, o);
        const GfVec3d axisWorld = std::string(space) == "world" ? GfVec3d(1, 0, 0)
            : UsdGeomXformable(s->GetPrimAtPath(SdfPath(C))).ComputeLocalToWorldTransform(UsdTimeCode::Default()).TransformDir(GfVec3d(1, 0, 0)).GetNormalized();
        const GfVec3d from = o + axisWorld * (0.5 * sc);
        CHECK(drag(t, s, C, cam, from, from + axisWorld * 2.0), "[%s] picked", space);
        CHECK(near3(worldPos(s, C), o + axisWorld * 2.0), "[%s] child moved 2 world units along gizmo X: %s", space, str(worldPos(s, C) - o).c_str());
        const GfVec3d lt = localT(s, C);
        std::printf("  [%s] local translate now %s (parent scale 2 => world 2 == local 1 along the axis)\n", space, str(lt).c_str());
    }

    std::puts("[11] active-handle highlight while dragging");
    {
        auto s = newStage(); MoveTool t;
        const double s0 = gizmoScale(cam, GfVec3d(0.0));
        const ToolContext ctx = ctxFor(s, {SdfPath(P)}, cam);
        auto countYellow = [&] { int n = 0; for (auto& l : t.buildGizmo(ctx).lines) if (l.color[0] > 0.99f && l.color[1] > 0.85f) ++n; return n; };
        CHECK(countYellow() == 0, "no highlight idle");
        const GfVec3d from(0.5 * s0, 0, 0);
        t.mousePress(ctx, cam.ndc(from), cam.ray(cam.ndc(from)));
        CHECK(countYellow() > 0, "active axis highlighted");
        t.mouseRelease();
        CHECK(countYellow() == 0, "highlight cleared on release");
    }

    std::puts("[12] center handle wins when pressed slightly off the origin");
    {
        const GfVec3d n = (GfVec3d(0.0) - cam.pos).GetNormalized();
        const GfVec3d d = GfCross(n, GfVec3d(0, 1, 0)).GetNormalized() * 1.5 + GfCross(n, GfCross(n, GfVec3d(0, 1, 0))).GetNormalized() * 0.7;
        const GfVec2d o = cam.ndc(GfVec3d(0.0));
        for (GfVec2d off : {GfVec2d(0, 0.004), GfVec2d(0, -0.004), GfVec2d(0.004, 0), GfVec2d(-0.004, 0.003)}) {
            auto s = newStage(); MoveTool t;
            ToolContext ctx = ctxFor(s, {SdfPath(P)}, cam);
            const GfVec2d pressAt = o + off;
            CHECK(t.mousePress(ctx, pressAt, cam.ray(pressAt)).handled, "pressed");
            const GfVec2d target = cam.ndc(d) + off;
            t.mouseMove(ctx, target, cam.ray(target));
            t.mouseRelease();
            const GfVec3d got = localT(s, P);
            CHECK(near3(got, d, 0.02), "off=(%.3f,%.3f): free drag expected ~%s got %s", off[0], off[1], str(d).c_str(), str(got).c_str());
        }
    }

    std::puts("[13] Move/Rotate/Scale are SelectTools: gizmo first, otherwise select");
    {
        const double s0 = gizmoScale(cam, GfVec3d(0.0));
        const GfVec3d onAxis(0.5 * s0, 0, 0);
        const GfVec2d farAway(0.9, -0.9);
        auto pickCube = [](const GfVec2d&) { return SdfPath("/World/Cube"); };
        auto pickNothing = [](const GfVec2d&) { return SdfPath(); };

        usdcc::tools::MoveTool mv; usdcc::tools::RotateTool rt; usdcc::tools::ScaleTool sc;
        usdcc::tools::TransformGizmoTool* tools[] = {&mv, &rt, &sc};
        const char* names[] = {"Move", "Rotate", "Scale"};
        for (int i = 0; i < 3; ++i) {
            usdcc::tools::SelectTool* st = tools[i];   // is-a SelectTool
            auto s = newStage();
            // Nothing selected: a click picks the cube.
            ToolContext none = ctxFor(s, {}, cam);
            none.pickPrim = pickCube;
            auto r = st->mousePress(none, farAway, cam.ray(farAway));
            CHECK(r.handled && r.newSelection && r.newSelection->size() == 1 && r.newSelection->front() == SdfPath(P),
                  "%s: click with nothing selected selects the picked prim", names[i]);
            st->mouseRelease();

            // Cube selected, click empty space: New mode clears.
            ToolContext sel = ctxFor(s, {SdfPath(P)}, cam);
            sel.pickPrim = pickNothing;
            r = st->mousePress(sel, farAway, cam.ray(farAway));
            CHECK(r.newSelection && r.newSelection->empty(), "%s: click on empty space clears", names[i]);
            st->mouseRelease();

            // Cube selected, press the gizmo: a drag starts, selection untouched even though pickPrim would change it.
            sel.pickPrim = pickNothing;
            // Move/Scale: a point on the X axis. Rotate: a point on its X ring (radius = gizmo scale, in the YZ plane).
            const GfVec3d handlePt = i == 1 ? GfVec3d(0, 0.7071 * s0, 0.7071 * s0) : onAxis;
            r = st->mousePress(sel, cam.ndc(handlePt), cam.ray(cam.ndc(handlePt)));
            CHECK(r.handled && !r.newSelection, "%s: press on a gizmo handle drags, does not select", names[i]);
            st->mouseRelease();

            // Mode option is offered (after the gizmo's own) and honoured.
            const auto opts = st->options();
            CHECK(!opts.empty() && opts.back().id == "mode", "%s: Mode is the last option", names[i]);
            st->setOption("mode", std::string("add"));
            sel.pickPrim = [](const GfVec2d&) { return SdfPath("/World"); };
            r = st->mousePress(sel, farAway, cam.ray(farAway));
            CHECK(r.newSelection && r.newSelection->size() == 2, "%s: Add mode adds to the selection", names[i]);
            st->mouseRelease();
        }
    }

    std::puts("[14] Scale: center handle wins when pressed slightly off the origin");
    {
        const GfVec2d o = cam.ndc(GfVec3d(0.0));
        for (GfVec2d off : {GfVec2d(0, 0.004), GfVec2d(0, -0.004), GfVec2d(0.004, 0), GfVec2d(-0.004, 0.003), GfVec2d(0.012, 0.012)}) {
            auto s = newStage(); usdcc::tools::ScaleTool t;
            ToolContext ctx = ctxFor(s, {SdfPath(P)}, cam);
            const GfVec2d pressAt = o + off;
            CHECK(t.mousePress(ctx, pressAt, cam.ray(pressAt)).handled, "pressed");
            const GfVec2d target = pressAt + GfVec2d(0.1, 0.1);
            t.mouseMove(ctx, target, cam.ray(target));
            t.mouseRelease();
            GfVec3d tr(0.0); GfVec3f r, sc, piv; UsdGeomXformCommonAPI::RotationOrder ro;
            UsdGeomXformCommonAPI(s->GetPrimAtPath(SdfPath(P))).GetXformVectors(&tr, &r, &sc, &piv, &ro, UsdTimeCode::Default());
            const bool uniform = std::abs(sc[0] - sc[1]) < 1e-3 && std::abs(sc[1] - sc[2]) < 1e-3;
            CHECK(uniform && std::abs(sc[0] - 1.0f) > 1e-3, "off=(%.3f,%.3f): uniform scale expected, got (%.3f, %.3f, %.3f)", off[0], off[1], sc[0], sc[1], sc[2]);
        }
    }

}
