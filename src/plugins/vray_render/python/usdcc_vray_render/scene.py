"""What the V-Ray helper is given: the stage as a file, and the camera as V-Ray frames it
(docs/VRAY_RENDER.md section 3).

V-Ray reads USD in USD's own space: a Y-up stage stays Y-up, metersPerUnit becomes
its unit scale, and a RenderView's transform is the camera's camera-to-world matrix.
Its field of view is horizontal, in radians.
"""

import itertools
import math
import os
import shutil
import tempfile

from pxr import Gf, Sdf, Usd, UsdGeom

# Where the export puts its own prims; nothing in a user's stage is named like this.
ROOT = Sdf.Path("/__usdccVRay")
CAMERA = ROOT.AppendChild("Camera")
SETTINGS = ROOT.AppendChild("RenderSettings")

# How many exports to keep: the one V-Ray has loaded and the one it is loading.
KEEP = 2


class Camera:
    """A camera as the helper takes it: camera-to-world, a horizontal field of
    view in radians at the render's aspect ratio, and for an orthographic camera
    the width it sees, in scene units."""

    __slots__ = ("fov", "ortho", "ortho_width", "transform")

    def __init__(self, transform, fov, ortho=False, ortho_width=1.0):
        self.transform = Gf.Matrix4d(transform)
        self.fov = fov
        self.ortho = ortho
        self.ortho_width = ortho_width

    @classmethod
    def from_render(cls, camera):
        """A usdcc.ui.RenderCamera, which the app's render view panel gives at the
        image's aspect ratio, as V-Ray takes it."""
        if camera.orthographic:
            return cls(camera.transform, 1.0, ortho=True, ortho_width=camera.ortho_width)
        return cls(camera.transform, math.radians(camera.fov_x))

    def command_fields(self):
        """The fields of the helper's "camera" command: the matrix's three axes
        and its position as 12 numbers, as V-Ray's Transform takes them."""
        rows = [self.transform.GetRow(i) for i in range(4)]
        values = [float(v) for row in rows for v in (row[0], row[1], row[2])]
        return {"transform": values, "fov": self.fov, "ortho": self.ortho, "ortho_width": self.ortho_width}


class SceneFiles:
    """Exports a stage for V-Ray to a folder of its own, removed by ``remove()``."""

    def __init__(self):
        self.folder = tempfile.mkdtemp(prefix="usdcc-vray-")
        self._numbers = itertools.count(1)
        self._files = []

    def export(self, stage, camera, resolution):
        """Writes `stage` as it is now, unsaved edits and session layer included,
        flattened into one file, and returns its path. It adds a camera at
        `camera` (a Camera) and render settings using it at `resolution`
        (width, height): V-Ray reads a USD file's prims only through its render
        settings, and the camera only seeds the image's framing, which the
        helper sets anyway."""
        layer = stage.Flatten()
        export = Usd.Stage.Open(layer)
        _add_render_prims(export, camera, resolution)
        path = os.path.join(self.folder, "scene_%d.usdc" % next(self._numbers))
        layer.Export(path)
        self._files.append(path)
        while len(self._files) > KEEP:
            _remove(self._files.pop(0))
        return path

    def remove(self):
        shutil.rmtree(self.folder, ignore_errors=True)
        self._files = []


def _add_render_prims(stage, camera, resolution):
    UsdGeom.Scope.Define(stage, ROOT)
    camera_prim = UsdGeom.Camera.Define(stage, CAMERA)
    camera_prim.MakeMatrixXform().Set(camera.transform)
    width, height = resolution
    focal = 35.0
    horizontal = 2.0 * focal * math.tan(camera.fov / 2.0)
    camera_prim.CreateFocalLengthAttr(focal)
    camera_prim.CreateHorizontalApertureAttr(horizontal)
    camera_prim.CreateVerticalApertureAttr(horizontal * height / max(1, width))
    if camera.ortho:
        camera_prim.CreateProjectionAttr(UsdGeom.Tokens.orthographic)
        camera_prim.CreateHorizontalApertureAttr(camera.ortho_width * 10.0)

    settings = stage.DefinePrim(SETTINGS, "RenderSettings")
    settings.CreateRelationship("camera", custom=False).SetTargets([CAMERA])
    settings.CreateAttribute("resolution", Sdf.ValueTypeNames.Int2, custom=False, variability=Sdf.VariabilityUniform).Set(
        Gf.Vec2i(int(width), int(height))
    )
    stage.SetMetadata("renderSettingsPrimPath", SETTINGS.pathString)


def _remove(path):
    try:
        os.remove(path)
    except OSError:
        pass  # still open in V-Ray; the folder goes at the end
