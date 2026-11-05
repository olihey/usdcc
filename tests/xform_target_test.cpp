// Tools acting on one specific xformOp (docs/ATTRIBUTE_EDITOR.md §7.4): two
// scale ops edited independently, a pivot sandwich, a target in the middle of
// a stack under a rotated and scaled parent, all six Euler orders, a
// single-axis rotate, orient, a transform (matrix) op, the target being
// cleared, and a tool that can't drive the target doing nothing.
#include "test_harness.h"

#include "usdcc/tools/move_tool.h"
#include "usdcc/tools/rotate_tool.h"
#include "usdcc/tools/scale_tool.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/xform_math.h"
#include "usdcc/usd/xform_stack.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>

#include <cmath>

using namespace PXR_NS;
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
    GfVec2d ndc(const GfVec3d& p) const {
        const GfVec3d n = (view * proj).Transform(p);
        return GfVec2d(n[0], n[1]);
    }
    GfRay ray(const GfVec2d& n) const { return frustum.ComputePickRay(n); }
};

ToolContext ctxFor(const UsdStageRefPtr& s, const char* prim, const Cam& c, const char* target) {
    ToolContext ctx;
    ctx.stage = s;
    ctx.selection = {SdfPath(prim)};
    ctx.viewMatrix = c.view;
    ctx.projMatrix = c.proj;
    ctx.cameraPos = c.pos;
    ctx.targetOpName = TfToken(target ? target : "");
    return ctx;
}

// The frame a targeted op is edited in, computed independently of the tools:
// Op_k · Outer · ParentToWorld.
GfMatrix4d opFrameMatrix(const UsdGeomXformable& x, size_t k) {
    bool resets = false;
    const auto ops = x.GetOrderedXformOps(&resets);
    return ops[k].GetOpTransform(UsdTimeCode::Default()) * usdcc::usd::composeOps(ops, 0, k) *
           x.ComputeParentToWorldTransform(UsdTimeCode::Default());
}

GfVec3d axisOf(const GfMatrix4d& m, int i) {
    GfVec3d e(0.0);
    e[i] = 1.0;
    return m.TransformDir(e).GetNormalized();
}

double gizmoScale(const Cam& c, const GfVec3d& origin) { return std::max(0.01, (c.pos - origin).GetLength() * 0.15); }

// Drags a Move/Scale axis handle: press halfway along the axis, end
// `factor` times as far along it (scale) or `distance` further (move).
bool dragAxis(usdcc::tools::Tool& tool, const ToolContext& ctx, const Cam& cam, const GfVec3d& origin,
              const GfVec3d& axis, const GfVec3d& end) {
    const GfVec3d from = origin + axis * (0.5 * gizmoScale(cam, origin));
    const auto press = tool.mousePress(ctx, cam.ndc(from), cam.ray(cam.ndc(from)));
    if (!press.startsEdit) {
        tool.mouseRelease();
        return false;
    }
    for (int i = 1; i <= 6; ++i) {
        const GfVec3d p = from + (end - from) * (i / 6.0);
        tool.mouseMove(ctx, cam.ndc(p), cam.ray(cam.ndc(p)));
    }
    tool.mouseRelease();
    return true;
}

// Drags ring `k` of the frame `m` by `degrees`.
bool dragRing(usdcc::tools::Tool& tool, const ToolContext& ctx, const Cam& cam, const GfMatrix4d& m, int k,
              double degrees, double a0 = 45.0) {
    const GfVec3d origin = m.ExtractTranslation();
    const GfVec3d u = axisOf(m, (k + 1) % 3), v = axisOf(m, (k + 2) % 3);
    const double s = gizmoScale(cam, origin);
    auto ringPt = [&](double aDeg) {
        const double a = GfDegreesToRadians(aDeg);
        return origin + (u * std::cos(a) + v * std::sin(a)) * s;
    };
    const auto press = tool.mousePress(ctx, cam.ndc(ringPt(a0)), cam.ray(cam.ndc(ringPt(a0))));
    if (!press.startsEdit) {
        tool.mouseRelease();
        return false;
    }
    const int steps = 12;
    for (int i = 1; i <= steps; ++i) {
        const GfVec2d p = cam.ndc(ringPt(a0 + degrees * i / steps));
        tool.mouseMove(ctx, p, cam.ray(p));
    }
    tool.mouseRelease();
    return true;
}

double maxDiff(const GfMatrix3d& a, const GfMatrix3d& b) {
    double d = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) d = std::max(d, std::abs(a[i][j] - b[i][j]));
    return d;
}

GfMatrix3d rotationOf(const UsdGeomXformOp& op) { return op.GetOpTransform(UsdTimeCode::Default()).ExtractRotationMatrix(); }

GfMatrix3d rot(int axis, double deg) {
    GfVec3d a(0.0);
    a[axis] = 1.0;
    return GfMatrix3d(GfRotation(a, deg));
}

GfVec3f vec3f(const UsdGeomXformOp& op) {
    GfVec3f v(0.0f);
    op.Get(&v);
    return v;
}

bool near3(const GfVec3d& a, const GfVec3d& b, double eps = 1e-3) { return (a - b).GetLength() < eps; }

std::string str(const GfVec3d& v) {
    char b[96];
    std::snprintf(b, sizeof b, "(%.4f, %.4f, %.4f)", v[0], v[1], v[2]);
    return b;
}

}  // namespace

USDCC_TEST(xform_target) {
    const Cam cam(GfVec3d(6, 5, 8));

    std::puts("[1] two scale ops, each edited independently");
    {
        UsdStageRefPtr s = UsdStage::CreateInMemory();
        UsdGeomXform x = UsdGeomXform::Define(s, SdfPath("/X"));
        UsdGeomXformOp first = x.AddScaleOp();
        UsdGeomXformOp second = x.AddScaleOp(UsdGeomXformOp::PrecisionFloat, TfToken("second"));
        first.Set(GfVec3f(1.0f));
        second.Set(GfVec3f(1.0f));
        usdcc::tools::ScaleTool tool;

        ToolContext ctx = ctxFor(s, "/X", cam, "xformOp:scale:second");
        GfMatrix4d f = opFrameMatrix(x, 1);
        GfVec3d o = f.ExtractTranslation(), ax = axisOf(f, 0);
        CHECK(dragAxis(tool, ctx, cam, o, ax, o + ax * (gizmoScale(cam, o))), "second: drag grabbed");
        CHECK(std::abs(vec3f(second)[0] - 2.0f) < 1e-3f && vec3f(second)[1] == 1.0f, "second scaled x2 in X (%g)",
              vec3f(second)[0]);
        CHECK(vec3f(first) == GfVec3f(1.0f), "first untouched");

        ctx = ctxFor(s, "/X", cam, "xformOp:scale");
        f = opFrameMatrix(x, 0);
        o = f.ExtractTranslation();
        ax = axisOf(f, 1);
        CHECK(dragAxis(tool, ctx, cam, o, ax, o + ax * (gizmoScale(cam, o) * 1.5)), "first: drag grabbed");
        CHECK(std::abs(vec3f(first)[1] - 3.0f) < 1e-3f, "first scaled x3 in Y (%g)", vec3f(first)[1]);
        CHECK(std::abs(vec3f(second)[0] - 2.0f) < 1e-3f, "second keeps its value");
    }

    std::puts("[2] pivot sandwich: rotate about the pivot");
    {
        UsdStageRefPtr s = UsdStage::CreateInMemory();
        UsdGeomXform x = UsdGeomXform::Define(s, SdfPath("/X"));
        x.AddTranslateOp().Set(GfVec3d(2, 0, 0));
        x.AddTranslateOp(UsdGeomXformOp::PrecisionFloat, TfToken("pivot")).Set(GfVec3f(1, 0, 0));
        UsdGeomXformOp r = x.AddRotateXYZOp();
        x.AddTranslateOp(UsdGeomXformOp::PrecisionFloat, TfToken("pivot"), true);
        r.Set(GfVec3f(0.0f));
        const GfMatrix4d f = opFrameMatrix(x, 2);
        CHECK(near3(f.ExtractTranslation(), GfVec3d(3, 0, 0)), "gizmo at the pivot %s", str(f.ExtractTranslation()).c_str());
        usdcc::tools::RotateTool tool;
        CHECK(dragRing(tool, ctxFor(s, "/X", cam, "xformOp:rotateXYZ"), cam, f, 1, 90.0), "ring grabbed");
        GfMatrix4d local;
        bool resets = false;
        x.GetLocalTransformation(&local, &resets);
        CHECK(near3(local.Transform(GfVec3d(1, 0, 0)), GfVec3d(3, 0, 0)), "the pivot point stays put: %s",
              str(local.Transform(GfVec3d(1, 0, 0))).c_str());
        CHECK(!near3(local.Transform(GfVec3d(0, 0, 0)), GfVec3d(2, 0, 0)), "the origin swung around it");
        CHECK(maxDiff(rotationOf(r), rot(1, 90)) < 1e-3, "rotated 90 about Y");
    }

    std::puts("[3] target in the middle of a 5-op stack under a rotated, scaled parent");
    {
        UsdStageRefPtr s = UsdStage::CreateInMemory();
        UsdGeomXform parent = UsdGeomXform::Define(s, SdfPath("/P"));
        parent.AddRotateXYZOp().Set(GfVec3f(0, 90, 0));
        parent.AddScaleOp().Set(GfVec3f(2, 2, 2));
        UsdGeomXform x = UsdGeomXform::Define(s, SdfPath("/P/X"));
        x.AddTranslateOp().Set(GfVec3d(1, 0, 0));
        x.AddRotateXYZOp().Set(GfVec3f(0, 30, 0));
        UsdGeomXformOp mid = x.AddTranslateOp(UsdGeomXformOp::PrecisionDouble, TfToken("mid"));
        mid.Set(GfVec3d(0.0));
        x.AddScaleOp().Set(GfVec3f(1, 2, 1));
        x.AddRotateXOp().Set(20.0f);

        usdcc::tools::MoveTool tool;  // Space: Local (the op's own axes)
        const ToolContext ctx = ctxFor(s, "/P/X", cam, "xformOp:translate:mid");
        const GfMatrix4d before = opFrameMatrix(x, 2);
        const GfVec3d o = before.ExtractTranslation(), ax = axisOf(before, 0);
        CHECK(dragAxis(tool, ctx, cam, o, ax, o + ax * (0.5 * gizmoScale(cam, o) + 1.5)), "drag grabbed");
        const GfMatrix4d after = opFrameMatrix(x, 2);
        CHECK(near3(after.ExtractTranslation() - o, ax * 1.5), "op origin moved exactly 1.5 along its own X: %s",
              str(after.ExtractTranslation() - o).c_str());
        bool resets = false;
        const auto ops = x.GetOrderedXformOps(&resets);
        GfVec3d t0;
        ops[0].Get(&t0);
        CHECK(t0 == GfVec3d(1, 0, 0), "the other translate is untouched");
    }

    std::puts("[4] all six Euler orders: ring drags compose about the op's own axes");
    for (UsdGeomXformOp::Type type :
         {UsdGeomXformOp::TypeRotateXYZ, UsdGeomXformOp::TypeRotateXZY, UsdGeomXformOp::TypeRotateYXZ,
          UsdGeomXformOp::TypeRotateYZX, UsdGeomXformOp::TypeRotateZXY, UsdGeomXformOp::TypeRotateZYX}) {
        const char* name = UsdGeomXformOp::GetOpTypeToken(type).GetText();
        UsdStageRefPtr s = UsdStage::CreateInMemory();
        UsdGeomXform x = UsdGeomXform::Define(s, SdfPath("/X"));
        UsdGeomXformOp op = x.AddXformOp(type, UsdGeomXformOp::PrecisionFloat);
        op.Set(GfVec3f(10, 20, 30));
        const GfMatrix3d r0 = usdcc::usd::eulerToMatrix(GfVec3d(10, 20, 30), type);
        const std::string target = "xformOp:" + std::string(name);
        usdcc::tools::RotateTool tool;
        CHECK(dragRing(tool, ctxFor(s, "/X", cam, target.c_str()), cam, opFrameMatrix(x, 0), 0, 30.0),
              "%s: X ring grabbed", name);
        CHECK(dragRing(tool, ctxFor(s, "/X", cam, target.c_str()), cam, opFrameMatrix(x, 0), 1, 20.0),
              "%s: Y ring grabbed", name);
        const GfMatrix3d expected = rot(1, 20) * rot(0, 30) * r0;
        CHECK(maxDiff(rotationOf(op), expected) < 2e-3, "%s: matches the exact composition (err %g)", name,
              maxDiff(rotationOf(op), expected));
    }

    std::puts("[5] rotateY: one ring only, angle added directly");
    {
        UsdStageRefPtr s = UsdStage::CreateInMemory();
        UsdGeomXform x = UsdGeomXform::Define(s, SdfPath("/X"));
        UsdGeomXformOp op = x.AddRotateYOp();
        op.Set(10.0f);
        usdcc::tools::RotateTool tool;
        const ToolContext ctx = ctxFor(s, "/X", cam, "xformOp:rotateY");
        const GfMatrix4d f = opFrameMatrix(x, 0);
        // Pressed at the top of where the X ring would be (well clear of the Y ring on screen).
    CHECK(!dragRing(tool, ctx, cam, f, 0, 30.0, 0.0), "the X ring isn't there to grab");
        float v = 0;
        op.Get(&v);
        CHECK(v == 10.0f, "value unchanged by the miss");
        CHECK(dragRing(tool, ctx, cam, f, 1, 25.0), "Y ring grabbed");
        op.Get(&v);
        CHECK(std::abs(v - 35.0f) < 1e-3f, "10 + 25 = %g", v);
        CHECK(tool.buildGizmo(ctx).lines.size() < usdcc::tools::RotateTool().buildGizmo(ctxFor(s, "/X", cam, nullptr)).lines.size(),
              "fewer rings drawn than in Auto mode");
    }

    std::puts("[6] orient: quaternion composed exactly");
    {
        UsdStageRefPtr s = UsdStage::CreateInMemory();
        UsdGeomXform x = UsdGeomXform::Define(s, SdfPath("/X"));
        UsdGeomXformOp op = x.AddOrientOp();
        op.Set(GfQuatf(1.0f));
        usdcc::tools::RotateTool tool;
        const ToolContext ctx = ctxFor(s, "/X", cam, "xformOp:orient");
        CHECK(dragRing(tool, ctx, cam, opFrameMatrix(x, 0), 2, 40.0), "Z ring grabbed");
        CHECK(maxDiff(rotationOf(op), rot(2, 40)) < 2e-3, "40 about Z");
        CHECK(dragRing(tool, ctx, cam, opFrameMatrix(x, 0), 0, 15.0), "X ring grabbed");
        CHECK(maxDiff(rotationOf(op), rot(0, 15) * rot(2, 40)) < 2e-3, "then 15 about its own X (err %g)",
              maxDiff(rotationOf(op), rot(0, 15) * rot(2, 40)));
    }

    std::puts("[7] the target is cleared when its op leaves the stack, or the selection moves");
    {
        usdcc::usd::StageManager manager;
        usdcc::usd::StageRefPtr stage = manager.createStage();
        UsdGeomXform x = UsdGeomXform::Define(stage->usdStage(), SdfPath("/X"));
        x.AddScaleOp();
        x.AddScaleOp(UsdGeomXformOp::PrecisionFloat, TfToken("second"));
        UsdGeomXform::Define(stage->usdStage(), SdfPath("/Other"));
        int changes = 0;
        auto c = stage->targetXformOpChanged.connect([&]() { ++changes; });

        manager.setSelectedPaths(stage, {SdfPath("/X")});
        stage->setTargetXformOp(SdfPath("/X"), TfToken("xformOp:scale:second"));
        CHECK(stage->targetXformOpFor(SdfPath("/X")) == TfToken("xformOp:scale:second"), "target set");
        CHECK(stage->targetXformOpFor(SdfPath("/Other")).IsEmpty(), "only for its prim");
        usdcc::usd::XformStack(x.GetPrim()).remove(0);  // a different op: target stays
        CHECK(stage->targetXformOp() == TfToken("xformOp:scale:second"), "removing another op keeps it");
        usdcc::usd::XformStack(x.GetPrim()).remove(0);  // the target op
        CHECK(stage->targetXformOp().IsEmpty(), "removing the target op clears it");

        x.AddTranslateOp();
        stage->setTargetXformOp(SdfPath("/X"), TfToken("xformOp:translate"));
        manager.setSelectedPaths(stage, {SdfPath("/X")});
        CHECK(!stage->targetXformOp().IsEmpty(), "reselecting the same prim keeps it");
        manager.setSelectedPaths(stage, {SdfPath("/Other")});
        CHECK(stage->targetXformOp().IsEmpty(), "selecting another prim clears it");
        CHECK(changes == 4, "one notification per change (%d)", changes);
    }

    std::puts("[8] a tool that can't drive the target edits nothing");
    {
        UsdStageRefPtr s = UsdStage::CreateInMemory();
        UsdGeomXform x = UsdGeomXform::Define(s, SdfPath("/X"));
        x.AddTranslateOp().Set(GfVec3d(0.0));
        x.AddScaleOp().Set(GfVec3f(1.0f));
        usdcc::tools::MoveTool tool;
        const ToolContext ctx = ctxFor(s, "/X", cam, "xformOp:scale");
        std::string before;
        s->GetRootLayer()->ExportToString(&before);
        const GfVec2d center = cam.ndc(GfVec3d(0.0));
        const auto press = tool.mousePress(ctx, center, cam.ray(center));
        CHECK(press.handled && !press.startsEdit, "press on the dimmed gizmo is swallowed, no edit");
        tool.mouseMove(ctx, center + GfVec2d(0.2, 0.1), cam.ray(center + GfVec2d(0.2, 0.1)));
        tool.mouseRelease();
        std::string after;
        s->GetRootLayer()->ExportToString(&after);
        CHECK(before == after, "stage unchanged");
        bool grey = true;
        for (const auto& line : tool.buildGizmo(ctx).lines) {
            grey = grey && std::abs(line.color[0] - line.color[1]) < 1e-6 && std::abs(line.color[1] - line.color[2]) < 1e-6;
        }
        CHECK(grey, "gizmo drawn grey");
        CHECK(usdcc::tools::toolIdForOpType(UsdGeomXformOp::TypeScale) == usdcc::tools::kScaleTool &&
                  usdcc::tools::toolIdForOpType(UsdGeomXformOp::TypeOrient) == usdcc::tools::kRotateTool &&
                  usdcc::tools::toolIdForOpType(UsdGeomXformOp::TypeTransform) == usdcc::tools::kMoveTool &&
                  usdcc::tools::toolIdForOpType(UsdGeomXformOp::TypeInvalid) == usdcc::tools::kSelectTool,
              "which tool drives which op");
        CHECK(usdcc::tools::toolCanDrive(usdcc::tools::kRotateTool, UsdGeomXformOp::TypeTransform) &&
                  usdcc::tools::toolCanDrive(usdcc::tools::kScaleTool, UsdGeomXformOp::TypeTransform) &&
                  !usdcc::tools::toolCanDrive(usdcc::tools::kSelectTool, UsdGeomXformOp::TypeTransform),
              "all three transform tools drive a matrix op");
    }

    std::puts("[9] a transform (matrix) op: each tool edits its own part of the matrix");
    {
        // A matrix with a rotation, a non-uniform scale and a translation,
        // after a translate op and under a rotated parent.
        UsdStageRefPtr s = UsdStage::CreateInMemory();
        UsdGeomXform parent = UsdGeomXform::Define(s, SdfPath("/P"));
        parent.AddRotateXYZOp().Set(GfVec3f(0, 0, 20));
        UsdGeomXform x = UsdGeomXform::Define(s, SdfPath("/P/X"));
        x.AddTranslateOp().Set(GfVec3d(0.5, 0, 0));
        UsdGeomXformOp matrixOp = x.AddTransformOp();
        GfMatrix4d m0(1.0);
        m0.SetRotate(GfRotation(GfVec3d(0, 1, 0), 30));
        GfMatrix4d scale(1.0);
        scale.SetScale(GfVec3d(1, 2, 3));
        m0 = scale * m0;  // scale first, then rotate (row vectors)
        m0.SetTranslateOnly(GfVec3d(1, 0, 0));
        matrixOp.Set(m0);
        auto matrix = [&]() {
            GfMatrix4d m;
            matrixOp.Get(&m);
            return m;
        };
        auto upper = [](const GfMatrix4d& m) { return m.ExtractRotationMatrix(); };
        auto rowsOrthogonal = [](const GfMatrix3d& a) {
            for (int i = 0; i < 3; ++i)
                for (int j = i + 1; j < 3; ++j) {
                    const GfVec3d ri(a[i][0], a[i][1], a[i][2]), rj(a[j][0], a[j][1], a[j][2]);
                    if (std::abs(GfDot(ri, rj)) > 1e-4) return false;
                }
            return true;
        };
        const ToolContext ctx = ctxFor(s, "/P/X", cam, "xformOp:transform");

        // Move: only the translation row changes, by the drag in world space.
        usdcc::tools::MoveTool move;
        GfMatrix4d f = opFrameMatrix(x, 1);
        GfVec3d o = f.ExtractTranslation(), ax = axisOf(f, 0);
        CHECK(dragAxis(move, ctx, cam, o, ax, o + ax * (0.5 * gizmoScale(cam, o) + 1.25)), "move: grabbed");
        CHECK(near3(opFrameMatrix(x, 1).ExtractTranslation() - o, ax * 1.25), "move: op origin moved 1.25 along its X: %s",
              str(opFrameMatrix(x, 1).ExtractTranslation() - o).c_str());
        CHECK(maxDiff(upper(matrix()), upper(m0)) < 1e-9, "move: rotation/scale part untouched");

        // Rotate: the 3x3 part turns about the drawn axis, translation stays,
        // and no shear appears despite the non-uniform scale.
        const GfMatrix4d beforeRotate = matrix();
        usdcc::tools::RotateTool rotate;
        CHECK(dragRing(rotate, ctx, cam, opFrameMatrix(x, 1), 1, 40.0), "rotate: Y ring grabbed");
        const GfMatrix3d a = upper(beforeRotate);
        GfVec3d axisY(a[1][0], a[1][1], a[1][2]);
        axisY.Normalize();
        const GfMatrix3d expected = a * GfMatrix3d(GfRotation(axisY, 40.0));
        CHECK(maxDiff(upper(matrix()), expected) < 2e-3, "rotate: 3x3 = A * Rot(drawn Y axis, 40) (err %g)",
              maxDiff(upper(matrix()), expected));
        CHECK(near3(matrix().ExtractTranslation(), beforeRotate.ExtractTranslation(), 1e-9), "rotate: translation stays");
        CHECK(rowsOrthogonal(upper(matrix())), "rotate: still a rotation times a scale (no shear)");
        GfVec3d row1(matrix()[1][0], matrix()[1][1], matrix()[1][2]);
        CHECK(std::abs(row1.GetLength() - 2.0) < 1e-3, "rotate: scale kept (|row Y| = %g)", row1.GetLength());

        // Scale: the grabbed axis's row is scaled, the others and the translation stay.
        const GfMatrix4d beforeScale = matrix();
        usdcc::tools::ScaleTool scaleTool;
        f = opFrameMatrix(x, 1);
        o = f.ExtractTranslation();
        ax = axisOf(f, 2);
        CHECK(dragAxis(scaleTool, ctx, cam, o, ax, o + ax * gizmoScale(cam, o)), "scale: Z handle grabbed");
        const GfMatrix4d after = matrix();
        for (int r = 0; r < 3; ++r) {
            const double factor = r == 2 ? 2.0 : 1.0;
            for (int c = 0; c < 3; ++c) {
                CHECK(std::abs(after[r][c] - beforeScale[r][c] * factor) < 1e-3, "scale: [%d][%d] = %g (expected %g)", r, c,
                      after[r][c], beforeScale[r][c] * factor);
            }
        }
        CHECK(near3(after.ExtractTranslation(), beforeScale.ExtractTranslation(), 1e-9), "scale: translation stays");

        // The translate op before it was never touched.
        GfVec3d t;
        bool resets = false;
        x.GetOrderedXformOps(&resets)[0].Get(&t);
        CHECK(t == GfVec3d(0.5, 0, 0), "the other op is untouched");
    }
}
