"""Final renders with MoonRay in a running usdcc (docs/MOONRAY.md section 7.8), by hand where the
moonray pack is:

    FR_OUT=/tmp/fr FR_SCENE=test_data/moonray/nodes.usda [FR_RENDERER=HdMoonrayRendererDebugPlugin] \
        usdcc --no-startup-scripts --script tests/moonray_final_render_check.py

A frame with color and depth, the same in 2 x 2 tiles, frames 1 to 3, a time limit at 4096 samples,
a cancel, and one more render after it. What came of each is in $FR_OUT/result.json.
"""
import json, os, shutil, sys, time, traceback
OUT = os.environ["FR_OUT"]
log = {"steps": []}
def note(**entry):
    log["steps"].append(entry)
    open(OUT + "/result.json", "w").write(json.dumps(log, indent=1))
try:
    import usdcc.app, usdcc.usd, usdcc.render as render
    from pxr import Sdf, Usd, UsdRender, Gf, Vt
    from PySide6.QtCore import QCoreApplication, QEventLoop
    renderer = os.environ.get("FR_RENDERER", "HdMoonrayRendererPlugin")
    log["renderers"] = [(r.id, r.name, r.problem, r.tiles) for r in render.renderers()]
    stage = usdcc.usd.open_stage(os.environ["FR_SCENE"])
    rs = UsdRender.Settings.Define(stage, "/Render/Settings")
    rs.CreateResolutionAttr(Gf.Vec2i(320, 200))
    rs.CreateCameraRel().SetTargets(["/World/camera"])
    product = UsdRender.Product.Define(stage, "/Render/Product")
    rs.CreateProductsRel().SetTargets([product.GetPath()])
    for name, source, kind in (("color", "color", "color4f"), ("depth", "depth", "float")):
        var = UsdRender.Var.Define(stage, "/Render/Vars/" + name)
        var.CreateSourceNameAttr(source); var.CreateDataTypeAttr(kind)
        product.CreateOrderedVarsRel().AddTarget(var.GetPath())
    prim = rs.GetPrim()
    def attr(name, type_name, value):
        prim.CreateAttribute(name, type_name, custom=True).Set(value)
    attr("usdcc:render:renderer", Sdf.ValueTypeNames.String, renderer)
    attr("usdcc:render:format", Sdf.ValueTypeNames.Token, "exr")
    stage.SetMetadata("renderSettingsPrimPath", "/Render/Settings")
    log["schema_note"] = "moonray settings on the prim"

    def wait(job_id, seconds, cancel_after=None):
        start = time.time()
        while time.time() - start < seconds:
            QCoreApplication.processEvents(QEventLoop.AllEvents, 50)
            j = render.job(job_id)
            if cancel_after is not None and time.time() - start > cancel_after and j.status in ("queued", "running"):
                render.cancel(job_id); cancel_after = None
            if j.status in ("done", "failed", "cancelled"):
                return j, time.time() - start
            time.sleep(0.02)
        return render.job(job_id), time.time() - start

    def run(name, output, seconds=240, cancel_after=None, animation=False, **extra):
        for key, (type_name, value) in extra.items():
            attr(key, type_name, value)
        attr("usdcc:render:output", Sdf.ValueTypeNames.String, OUT + "/" + output)
        job_id = (render.render_animation if animation else render.render_frame)(stage)
        j, took = wait(job_id, seconds, cancel_after)
        note(name=name, status=j.status, error=j.error, files=j.files, frames=list(j.frames), seconds=round(took, 1))
        return j

    run("frame with color and depth", "frame.exr")
    run("2x2 tiles", "tiles.exr", **{"usdcc:render:tiles": (Sdf.ValueTypeNames.Int2, Gf.Vec2i(2, 2))})
    attr("usdcc:render:tiles", Sdf.ValueTypeNames.Int2, Gf.Vec2i(1, 1))
    stage.SetStartTimeCode(1); stage.SetEndTimeCode(3)
    xf = stage.GetPrimAtPath("/World/plane")
    from pxr import UsdGeom
    op = UsdGeom.Xformable(xf).AddRotateZOp()
    for f in (1, 2, 3): op.Set(30.0 * f, f)
    run("sequence 1-3", "seq.####.exr", animation=True, **{"usdcc:render:frameStart": (Sdf.ValueTypeNames.Double, 1.0), "usdcc:render:frameEnd": (Sdf.ValueTypeNames.Double, 3.0)})
    ns = "" if renderer == "HdMoonrayRendererPlugin" else "moonrayDebug:"
    log["schema"] = [k for k in [a.GetName() for a in prim.GetAttributes()]]
    attr(ns + "moonray:sceneVariable:pixel_samples", Sdf.ValueTypeNames.Int, 4096)
    t = run("time limit 3 s, 4096 pixel samples", "limit.exr", seconds=120, **{"usdcc:render:timeLimit": (Sdf.ValueTypeNames.Double, 3.0)})
    attr("usdcc:render:timeLimit", Sdf.ValueTypeNames.Double, 0.0)
    run("cancelled after 1 s, 4096 pixel samples", "cancel.exr", seconds=60, cancel_after=1.0)
    attr(ns + "moonray:sceneVariable:pixel_samples", Sdf.ValueTypeNames.Int, 1)
    run("after the cancel, again, 1 pixel sample", "again.exr", seconds=240)
    import usdcc.settings as st
    log["settings"] = [k for k in st.keys("Renderers") if "Moonray" in k]
except Exception:
    log["error"] = traceback.format_exc()
open(OUT + "/result.json", "w").write(json.dumps(log, indent=1))
os._exit(0)
