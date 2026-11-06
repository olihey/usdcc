"""What the RTX helper is given: the stage as a file with a camera and a render product
of its own, the camera as apertures, and edits as transforms written in place
(docs/RTX_RENDER.md section 3).
"""

import itertools
import math
import os
import shutil
import tempfile

from pxr import Gf, Sdf, Usd, UsdGeom

# Where the export puts its own prims; nothing in a user's stage is named like this.
ROOT = Sdf.Path("/__usdccRtx")
CAMERA = ROOT.AppendChild("Camera")
PRODUCT = ROOT.AppendChild("Product")
LDR_COLOR = PRODUCT.AppendChild("LdrColor")

# The camera's focal length in mm; its apertures carry the field of view.
FOCAL_LENGTH = 10.0

# The Mode menu, and ovrtx's omni:rtx:rendermode for each.
MODES = {
    "Real-Time": "RealTimePathTracing",
    "Path Tracing": "PathTracing",
    "Minimal": "MinimalRendering",
}

# How many exports to keep: the one loaded and the one loading.
KEEP = 2


def camera_fields(camera):
    """The helper's "camera" command for a usdcc.ui.RenderCamera: camera-to-world as 16
    numbers, row by row (USD's layout, which ovrtx's omni:xform takes too), and the
    apertures in mm that give its fields of view at FOCAL_LENGTH."""
    matrix = camera.transform
    values = [float(matrix[row][column]) for row in range(4) for column in range(4)]
    if camera.orthographic:
        # Apertures are in tenths of a scene unit.
        horizontal = camera.ortho_width * 10.0
        vertical = horizontal / max(camera.aspect, 1e-6)
    else:
        horizontal = 2.0 * FOCAL_LENGTH * math.tan(math.radians(camera.fov_x) / 2.0)
        vertical = 2.0 * FOCAL_LENGTH * math.tan(math.radians(camera.fov_y) / 2.0)
    return {"transform": values, "h_aperture": horizontal, "v_aperture": vertical}


def _is_transform(path):
    return path.IsPropertyPath() and (path.name.startswith("xformOp:") or path.name == UsdGeom.Tokens.xformOpOrder)


def in_place_edits(stage, resynced, changed):
    """The local transforms to write for an edit, [{path, matrix}], when all it changed
    is transforms; None when the stage has to be sent again (a prim added, removed or
    renamed, or anything but a transform changed)."""
    if resynced:
        return None
    prims = {}
    for path in changed:
        if not _is_transform(path):
            return None
        prim = stage.GetPrimAtPath(path.GetPrimPath())
        if not prim or not prim.IsA(UsdGeom.Xformable):
            return None
        prims[prim.GetPath()] = prim
    items = []
    for path, prim in sorted(prims.items()):
        matrix = UsdGeom.Xformable(prim).GetLocalTransformation(Usd.TimeCode.Default())
        items.append({"path": path.pathString,
                      "matrix": [float(matrix[row][column]) for row in range(4) for column in range(4)]})
    return items


class SceneFiles:
    """Exports a stage for the helper to a folder of its own, removed by ``remove()``."""

    def __init__(self):
        self.folder = tempfile.mkdtemp(prefix="usdcc-rtx-")
        self._numbers = itertools.count(1)
        self._files = []

    def export(self, stage, camera, size, mode):
        """Writes `stage` as it is now, unsaved edits and session layer included,
        flattened into one file, and returns its path. It adds a camera at `camera` (a
        usdcc.ui.RenderCamera) and a render product of `size` (width, height) in `mode`
        (a MODES key) rendering an LdrColor through it."""
        layer = stage.Flatten()
        export = Usd.Stage.Open(layer)
        _add_render_prims(export, camera, size, MODES.get(mode, MODES["Real-Time"]))
        path = os.path.join(self.folder, "scene_%d.usdc" % next(self._numbers))
        layer.Export(path)
        self._files.append(path)
        while len(self._files) > KEEP:
            _remove(self._files.pop(0))
        return path

    def remove(self):
        shutil.rmtree(self.folder, ignore_errors=True)
        self._files = []


def _add_render_prims(stage, camera, size, render_mode):
    UsdGeom.Scope.Define(stage, ROOT)
    camera_prim = UsdGeom.Camera.Define(stage, CAMERA)
    fields = camera_fields(camera)
    camera_prim.MakeMatrixXform().Set(Gf.Matrix4d(*fields["transform"]))
    camera_prim.CreateFocalLengthAttr(FOCAL_LENGTH)
    camera_prim.CreateHorizontalApertureAttr(fields["h_aperture"])
    camera_prim.CreateVerticalApertureAttr(fields["v_aperture"])
    camera_prim.CreateClippingRangeAttr(Gf.Vec2f(max(camera.near, 1e-4), max(camera.far, camera.near + 1.0)))
    if camera.orthographic:
        camera_prim.CreateProjectionAttr(UsdGeom.Tokens.orthographic)

    width, height = size
    product = stage.DefinePrim(PRODUCT, "RenderProduct")
    product.CreateAttribute("resolution", Sdf.ValueTypeNames.Int2, custom=False,
                            variability=Sdf.VariabilityUniform).Set(Gf.Vec2i(int(width), int(height)))
    product.CreateRelationship("camera", custom=False).SetTargets([CAMERA])
    product.CreateAttribute("omni:rtx:rendermode", Sdf.ValueTypeNames.Token).Set(render_mode)
    var = stage.DefinePrim(LDR_COLOR, "RenderVar")
    var.CreateAttribute("sourceName", Sdf.ValueTypeNames.String, custom=False,
                        variability=Sdf.VariabilityUniform).Set("LdrColor")
    product.CreateRelationship("orderedVars", custom=False).SetTargets([LDR_COLOR])


def _remove(path):
    try:
        os.remove(path)
    except OSError:
        pass  # still open in the helper; the folder goes at the end
