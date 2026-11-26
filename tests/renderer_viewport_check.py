"""A render delegate in usdcc's viewport, checked from its pictures (docs/USD_PLUGINS.md section 7.1),
run by hand where the renderer is:

    RV_OUT=<folder> RV_RENDERER=HdCyclesPlugin usdcc --no-startup-scripts --script tests/renderer_viewport_check.py

Writes a scene to RV_OUT (a ground mesh; a sphere, cube, cylinder and cone, which are implicit
shapes; a distant and a dome light; a textured UsdPreviewSurface, a red one, and display colors)
and takes the viewport's own picture (Viewport.grab()) after each step: the first converged
frame, a camera move, an edit, a resize, a switch to Storm and back. Each picture is
RV_OUT/<n>-<step>.png; RV_OUT/result.json says how long each took and what the pictures hold
(the mean and the share of red pixels, which only the red material makes). The app quits at the
end, which is the exit check.
"""

import json
import os
import time
import traceback

OUT = os.environ["RV_OUT"]
RENDERER = os.environ.get("RV_RENDERER", "HdCyclesPlugin")
WAIT = float(os.environ.get("RV_WAIT", "120"))
log = {"renderer": RENDERER, "steps": []}


def save():
    with open(os.path.join(OUT, "result.json"), "w") as file:
        json.dump(log, file, indent=1)


SCENE = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Ground" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Looks/Checker>
        uniform token subdivisionScheme = "none"
        float3[] extent = [(-6, 0, -6), (6, 0, 6)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        normal3f[] normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)] (interpolation = "vertex")
        point3f[] points = [(-6, 0, 6), (6, 0, 6), (6, 0, -6), (-6, 0, -6)]
        texCoord2f[] primvars:st = [(0, 0), (4, 0), (4, 4), (0, 4)] (interpolation = "vertex")
    }
    def Sphere "Sphere" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Looks/Red>
        double radius = 1
        double3 xformOp:translate = (-2.5, 1, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Cube "Cube"
    {
        double size = 1.6
        color3f[] primvars:displayColor = [(0.2, 0.4, 0.9)]
        double3 xformOp:translate = (0, 0.8, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Cylinder "Cylinder"
    {
        double radius = 0.6
        double height = 2
        color3f[] primvars:displayColor = [(0.9, 0.8, 0.2)]
        double3 xformOp:translate = (2.5, 1, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Cone "Cone"
    {
        double radius = 0.7
        double height = 1.6
        color3f[] primvars:displayColor = [(0.2, 0.8, 0.3)]
        double3 xformOp:translate = (0, 0.8, 2.5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def DistantLight "Sun"
    {
        float inputs:intensity = 2
        float inputs:angle = 1
        float3 xformOp:rotateXYZ = (-50, 30, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }
    def DomeLight "Sky"
    {
        float inputs:intensity = 0.4
    }
    def Camera "Camera"
    {
        float focalLength = 30
        float horizontalAperture = 36
        float verticalAperture = 24
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 4, 11)
        float3 xformOp:rotateXYZ = (-18, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
    def Scope "Looks"
    {
        def Material "Red"
        {
            token outputs:surface.connect = </World/Looks/Red/Surface.outputs:surface>
            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.85, 0.05, 0.05)
                float inputs:roughness = 0.4
                token outputs:surface
            }
        }
        def Material "Checker"
        {
            token outputs:surface.connect = </World/Looks/Checker/Surface.outputs:surface>
            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </World/Looks/Checker/Tex.outputs:rgb>
                float inputs:roughness = 0.8
                token outputs:surface
            }
            def Shader "Tex"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @./checker.png@
                float2 inputs:st.connect = </World/Looks/Checker/UV.outputs:result>
                token inputs:wrapS = "repeat"
                token inputs:wrapT = "repeat"
                float3 outputs:rgb
            }
            def Shader "UV"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "st"
                float2 outputs:result
            }
        }
    }
}
"""

try:
    import usdcc.app
    import usdcc.usd
    from PySide6.QtCore import QCoreApplication, QEventLoop
    from PySide6.QtGui import QColor, QImage
    from pxr import Gf, UsdGeom

    os.makedirs(OUT, exist_ok=True)
    checker = QImage(64, 64, QImage.Format_RGB32)
    for y in range(64):
        for x in range(64):
            checker.setPixelColor(x, y, QColor(230, 230, 230) if (x // 32 + y // 32) % 2 else QColor(40, 40, 40))
    checker.save(os.path.join(OUT, "checker.png"))
    scene = os.path.join(OUT, "scene.usda")
    with open(scene, "w") as file:
        file.write(SCENE)

    def pump(seconds):
        end = time.time() + seconds
        while time.time() < end:
            QCoreApplication.processEvents(QEventLoop.AllEvents, 20)

    def describe(path):
        image = QImage(path)
        if image.isNull():
            return {"picture": None}
        image = image.convertToFormat(QImage.Format_RGB32).scaled(160, 90)
        total, red = 0, 0
        for y in range(image.height()):
            for x in range(image.width()):
                c = image.pixelColor(x, y)
                total += c.red() + c.green() + c.blue()
                red += c.red() > 120 and c.green() < 80 and c.blue() < 80
        count = image.width() * image.height()
        return {"mean": round(total / (count * 765.0), 3), "red": round(red / count, 4)}

    shot = [0]

    def step(name, viewport, wait=WAIT):
        start = time.time()
        pump(0.5)  # the change reaches the renderer
        while time.time() - start < wait and not viewport.is_converged():
            pump(0.1)
        shot[0] += 1
        path = os.path.join(OUT, f"{shot[0]:02d}-{name}.png")
        grabbed = viewport.grab(path)
        entry = {"step": name, "renderer": viewport.renderer(), "converged": viewport.is_converged(),
                 "seconds": round(time.time() - start, 1), "grabbed": grabbed, **describe(path)}
        log["steps"].append(entry)
        save()
        return entry

    stage = usdcc.usd.open_stage(scene)
    pump(2)
    viewport = usdcc.app.active_viewport() or usdcc.app.viewports()[0]
    viewport.camera = "/World/Camera"
    log["renderers"] = viewport.renderers()
    log["switched"] = viewport.set_renderer(RENDERER)
    step("first", viewport)
    camera = UsdGeom.Xformable(stage.GetPrimAtPath("/World/Camera"))
    camera.GetOrderedXformOps()[0].Set(Gf.Vec3d(-4, 3, 9))
    step("camera-moved", viewport)
    stage.GetPrimAtPath("/World/Cube").GetAttribute("primvars:displayColor").Set([Gf.Vec3f(0.95, 0.05, 0.05)])
    UsdGeom.Xformable(stage.GetPrimAtPath("/World/Sphere")).GetOrderedXformOps()[0].Set(Gf.Vec3d(-2.5, 2.5, 0))
    step("edited", viewport)
    window = usdcc.app.main_window()
    size = window.size()
    window.resize(size.width() - 300, size.height() - 200)
    step("resized", viewport)
    window.resize(size)
    viewport.set_renderer("HdStormRendererPlugin")
    step("storm", viewport, wait=10)
    viewport.set_renderer(RENDERER)
    step("back", viewport)
    log["done"] = True
except Exception:
    log["error"] = traceback.format_exc()
save()
os._exit(0)
