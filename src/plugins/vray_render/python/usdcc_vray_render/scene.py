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

    def command_fields(self):
        """The fields of the helper's "camera" command: the matrix's three axes
        and its position as 12 numbers, as V-Ray's Transform takes them."""
        rows = [self.transform.GetRow(i) for i in range(4)]
        values = [float(v) for row in rows for v in (row[0], row[1], row[2])]
        return {"transform": values, "fov": self.fov, "ortho": self.ortho, "ortho_width": self.ortho_width}


def horizontal_fov(vertical_degrees, aspect):
    """The horizontal field of view, in radians, for a vertical one in degrees."""
    return 2.0 * math.atan(math.tan(math.radians(vertical_degrees) / 2.0) * aspect)


def from_viewport(camera, aspect):
    """A usdcc.app.ViewportCamera as rendered at `aspect` (width / height): the
    vertical field of view is kept, so a wider image shows more at the sides."""
    return Camera(camera.transform, horizontal_fov(camera.fov_y, aspect))


def from_prim(prim, aspect, time=None):
    """A UsdGeom.Camera prim as rendered at `aspect` (the default time unless
    `time` is given). Its horizontal aperture decides the framing, as V-Ray's own
    USD import does."""
    gf_camera = UsdGeom.Camera(prim).GetCamera(Usd.TimeCode.Default() if time is None else time)
    transform = gf_camera.transform
    if gf_camera.projection == Gf.Camera.Orthographic:
        # Apertures are in tenths of a scene unit.
        return Camera(transform, 1.0, ortho=True, ortho_width=gf_camera.horizontalAperture / 10.0)
    return Camera(transform, math.radians(gf_camera.GetFieldOfView(Gf.Camera.FOVHorizontal)))


def camera_prims(stage):
    """The paths of the stage's cameras, in the order they appear."""
    if stage is None:
        return []
    return [prim.GetPath() for prim in stage.Traverse() if prim.IsA(UsdGeom.Camera) and not prim.GetPath().HasPrefix(ROOT)]


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
