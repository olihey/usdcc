"""Generates the shapes the Material Editor's preview ships with
(docs/MATERIAL_EDITOR.md §7.3), into resources/preview/shapes.

Made here rather than downloaded, so they carry no licence of their own and stay
small. The HDRIs aren't made here any more: resources/preview/hdri holds Poly
Haven maps (§7.4).

A shape is a .usda whose defaultPrim holds its geometry, Y-up, in metres, with
UVs (primvars:st). When only some of it should show the material, the default
prim has a collection:preview listing those prims; the rest keep their own.

Standard library only. Run from the repo root:
    python tools/gen_preview_assets.py
"""

import math
from pathlib import Path

OUT_DIR = Path(__file__).resolve().parent.parent / "resources/preview"


# --- Meshes -----------------------------------------------------------------

class Mesh:
    def __init__(self):
        self.points = []
        self.normals = []
        self.st = []
        self.counts = []
        self.indices = []

    def add(self, other):
        offset = len(self.points)
        self.points += other.points
        self.normals += other.normals
        self.st += other.st
        self.counts += other.counts
        self.indices += [i + offset for i in other.indices]
        return self


def normalize(v):
    length = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / length for c in v)


def sphere(radius, center, segments=64, rings=32):
    """A UV sphere: u around, v from the bottom pole to the top one."""
    mesh = Mesh()
    for ring in range(rings + 1):
        v = ring / rings
        theta = math.pi * (1.0 - v)  # from the bottom pole
        for seg in range(segments + 1):
            u = seg / segments
            phi = 2.0 * math.pi * u
            n = (math.sin(theta) * math.cos(phi), math.cos(theta), -math.sin(theta) * math.sin(phi))
            mesh.points.append(tuple(center[i] + radius * n[i] for i in range(3)))
            mesh.normals.append(n)
            mesh.st.append((u, v))
    row = segments + 1
    for ring in range(rings):
        for seg in range(segments):
            a = ring * row + seg
            mesh.counts.append(4)
            mesh.indices += [a, a + 1, a + row + 1, a + row]
    return mesh


def cylinder(radius, y0, y1, segments=64):
    """A capped cylinder, its side and caps apart so they keep hard edges."""
    mesh = Mesh()
    for i in range(segments + 1):
        u = i / segments
        a = 2.0 * math.pi * u
        n = (math.cos(a), 0.0, -math.sin(a))
        mesh.points += [(radius * n[0], y0, radius * n[2]), (radius * n[0], y1, radius * n[2])]
        mesh.normals += [n, n]
        mesh.st += [(u, 0.0), (u, 1.0)]
    for i in range(segments):
        a = 2 * i
        mesh.counts.append(4)
        mesh.indices += [a, a + 2, a + 3, a + 1]
    for y, up in ((y1, 1.0), (y0, -1.0)):
        center = len(mesh.points)
        mesh.points.append((0.0, y, 0.0))
        mesh.normals.append((0.0, up, 0.0))
        mesh.st.append((0.5, 0.5))
        for i in range(segments + 1):
            a = 2.0 * math.pi * i / segments
            mesh.points.append((radius * math.cos(a), y, -radius * math.sin(a)))
            mesh.normals.append((0.0, up, 0.0))
            mesh.st.append((0.5 + 0.5 * math.cos(a), 0.5 + 0.5 * math.sin(a)))
        for i in range(segments):
            mesh.counts.append(3)
            ring = [center, center + 1 + i, center + 2 + i]
            mesh.indices += ring if up > 0 else [ring[0], ring[2], ring[1]]
    return mesh


def box(size, center=(0.0, 0.0, 0.0)):
    """A cube, each face with the whole of UV space."""
    mesh = Mesh()
    h = size / 2.0
    faces = [  # normal, u axis, v axis
        ((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
        ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
        ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0)),
    ]
    for n, u, v in faces:
        start = len(mesh.points)
        for su, sv in ((0, 0), (1, 0), (1, 1), (0, 1)):
            p = tuple(center[i] + h * (n[i] + (2 * su - 1) * u[i] + (2 * sv - 1) * v[i]) for i in range(3))
            mesh.points.append(p)
            mesh.normals.append(n)
            mesh.st.append((float(su), float(sv)))
        mesh.counts.append(4)
        mesh.indices += [start, start + 1, start + 2, start + 3]
    return mesh


def grid(size_x, size_z, divisions, height=lambda x, z: 0.0):
    """A grid over the ground, raised by `height`, with normals from its slopes."""
    mesh = Mesh()
    n = divisions
    pts = []
    for j in range(n + 1):
        for i in range(n + 1):
            x = (i / n - 0.5) * size_x
            z = (0.5 - j / n) * size_z
            pts.append((x, height(x, z), z))
            mesh.st.append((i / n, j / n))
    mesh.points = pts
    row = n + 1
    normals = [[0.0, 0.0, 0.0] for _ in pts]
    for j in range(n):
        for i in range(n):
            a = j * row + i
            quad = [a, a + 1, a + row + 1, a + row]
            mesh.counts.append(4)
            mesh.indices += quad
            p0, p1, p3 = pts[quad[0]], pts[quad[1]], pts[quad[3]]
            e1 = [p1[k] - p0[k] for k in range(3)]
            e2 = [p3[k] - p0[k] for k in range(3)]
            fn = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            for k in quad:
                for c in range(3):
                    normals[k][c] += fn[c]
    mesh.normals = [normalize(v) for v in normals]
    return mesh


def cloth_height(ball_radius, ball_y, floor_y):
    """Cloth laid over a ball: on it near the top, then hanging in folds to the floor."""
    rest = ball_radius * 0.82  # where it leaves the ball

    def height(x, z):
        d = math.hypot(x, z)
        top = ball_y + math.sqrt(max(ball_radius * ball_radius - min(d, rest) ** 2, 0.0)) + 0.012
        if d <= rest:
            return top
        angle = math.atan2(z, x)
        fall = d - rest
        folds = 0.045 * math.sin(7.0 * angle + 1.3) * min(fall / 0.25, 1.0)
        drop = (top - floor_y) * (1.0 - math.exp(-fall * 5.5))
        return max(top - drop + folds, floor_y + 0.004)

    return height


def fmt(v):
    return "(" + ", ".join("%.5g" % c for c in v) + ")"


def mesh_text(name, mesh, indent, material=None, extra=""):
    pad = " " * indent
    api = '\n%s    prepend apiSchemas = ["MaterialBindingAPI"]' % pad if material else ""
    lines = [
        '%sdef Mesh "%s" (%s%s\n%s)' % (pad, name, api, extra, pad),
        "%s{" % pad,
        "%s    uniform token subdivisionScheme = \"none\"" % pad,
        "%s    int[] faceVertexCounts = [%s]" % (pad, ", ".join(map(str, mesh.counts))),
        "%s    int[] faceVertexIndices = [%s]" % (pad, ", ".join(map(str, mesh.indices))),
        "%s    point3f[] points = [%s]" % (pad, ", ".join(fmt(p) for p in mesh.points)),
        "%s    normal3f[] normals = [%s] (\n%s        interpolation = \"vertex\"\n%s    )" % (
            pad, ", ".join(fmt(n) for n in mesh.normals), pad, pad),
        "%s    texCoord2f[] primvars:st = [%s] (\n%s        interpolation = \"vertex\"\n%s    )" % (
            pad, ", ".join(fmt(t) for t in mesh.st), pad, pad),
    ]
    xs = [p[0] for p in mesh.points]
    ys = [p[1] for p in mesh.points]
    zs = [p[2] for p in mesh.points]
    lines.append("%s    float3[] extent = [%s, %s]" % (pad, fmt((min(xs), min(ys), min(zs))), fmt((max(xs), max(ys), max(zs)))))
    if material:
        lines.append("%s    rel material:binding = <%s>" % (pad, material))
    lines.append("%s}" % pad)
    return "\n".join(lines)


GREY_MATERIAL = '''    def Material "Grey"
    {
        token outputs:surface.connect = </%s/Grey/Surface.outputs:surface>

        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.18, 0.18, 0.19)
            float inputs:roughness = 0.55
            token outputs:surface
        }
    }'''


def shape_file(name, prim, body, preview=None, comment=""):
    header = '#usda 1.0\n(\n    defaultPrim = "%s"\n    metersPerUnit = 1\n    upAxis = "Y"\n    doc = """%s"""\n)\n\n' % (prim, comment)
    if preview:
        api = ' (\n    prepend apiSchemas = ["CollectionAPI:preview"]\n)'
        rel = "    uniform token collection:preview:expansionRule = \"expandPrims\"\n    rel collection:preview:includes = [%s]\n\n" % (
            ", ".join("</%s/%s>" % (prim, p) for p in preview))
    else:
        api, rel = "", ""
    text = header + 'def Xform "%s"%s\n{\n%s%s\n}\n' % (prim, api, rel, body)
    path = OUT_DIR / "shapes" / (name + ".usda")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    print("wrote", path.relative_to(OUT_DIR.parent.parent), "%d KB" % (len(text) // 1024))


def make_shapes():
    shape_file("Sphere", "Sphere", mesh_text("Ball", sphere(0.5, (0, 0.5, 0)), 4),
               comment="A sphere of 1 m: all of it shows the material.")
    shape_file("Cube", "Cube", mesh_text("Box", box(0.9, (0, 0.45, 0)), 4),
               comment="A 90 cm cube, each face with the whole of UV space.")
    shape_file("Plane", "Plane", mesh_text("Swatch", grid(1.2, 1.2, 1), 4),
               comment="A 1.2 m square on the ground, for tiling.")
    root = "ShaderBall"
    stand = cylinder(0.24, 0.0, 0.1)
    shape_file("Shader Ball", root,
               mesh_text("Ball", sphere(0.42, (0, 0.52, 0)), 4) + "\n\n" +
               mesh_text("Stand", stand, 4, material="/%s/Grey" % root) + "\n\n" + GREY_MATERIAL % root,
               preview=["Ball"],
               comment="A ball on a stand: the ball shows the material, the stand stays grey.")
    root = "ClothOverBall"
    shape_file("Cloth over Ball", root,
               mesh_text("Cloth", grid(1.8, 1.8, 72, cloth_height(0.38, 0.4, 0.0)), 4) + "\n\n" +
               mesh_text("Ball", sphere(0.38, (0, 0.4, 0), 48, 24), 4, material="/%s/Grey" % root) + "\n\n" +
               GREY_MATERIAL % root,
               preview=["Cloth"],
               comment="Cloth over a ball, to see how a material folds: the cloth shows it.")


def main():
    make_shapes()


if __name__ == "__main__":
    main()
