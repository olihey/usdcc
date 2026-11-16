"""Generate an explicit standalone subset from the immutable Blender source.

Generated files retain Blender's GPL notices. We exclude application/serialization
entry points, never replace them with stubs. Algorithms are copied unchanged.
Selectors are intentionally specific to the pinned revision and fail on drift.
"""

import argparse
import hashlib
import json
import re
from pathlib import Path


def write_if_changed(path, text, encoding="utf-8"):
    if path.suffix == ".cc":
        text = (
            "// Modified for usdcc on 2026-11-15: generated standalone Blender subset.\n"
            "// Excludes application/serialization code; see extract_sources.py for all changes.\n"
            + text
        )
    if not path.is_file() or path.read_text(encoding=encoding) != text:
        path.write_text(text, encoding=encoding)


def index(text, marker):
    if text.count(marker) != 1:
        raise RuntimeError(f"Expected one source marker: {marker!r}")
    return text.index(marker)


def function(text, signature):
    start = index(text, signature)
    # Mask comments and literals without changing character positions.
    masked = re.sub(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda m: " " * len(m[0]),
        text,
        flags=re.DOTALL,
    )
    opening = masked.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return text[start:end]


def generate(source, output):
    source, output = Path(source), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    inventory = []

    def read(relative):
        text = (source / relative).read_text(encoding="utf-8")
        inventory.append(
            {"source": relative, "sha256": hashlib.sha256(text.encode()).hexdigest()}
        )
        return text

    def save(name, original, body):
        # Preserve upstream notices and includes. All excerpts remain GPL code.
        prefix = original[: index(original, "namespace blender {")]
        write_if_changed(
            output / name,
            prefix
            + "\n// Generated standalone subset; see extract_sources.py.\n"
            + "namespace blender {\n"
            + body
            + "\n} // namespace blender\n",
            encoding="utf-8",
        )

    kernel = "source/blender/blenkernel/intern/"
    bm = "source/blender/bmesh/intern/"
    text = read(kernel + "customdata.cc")
    body = text[
        index(text, "namespace blender {") + len("namespace blender {") : index(
            text,
            "/* -------------------------------------------------------------------- */\n/** \\name External Files",
        )
    ]
    for name in ["layerRead_mdisps", "layerWrite_mdisps", "layerFilesize_mdisps"]:
        signature = (
            ("static size_t " if name.startswith("layerFilesize") else "static bool ")
            + name
            + "("
        )
        body = body.replace(function(body, signature), "")
        member = {
            "layerRead_mdisps": "read",
            "layerWrite_mdisps": "write",
            "layerFilesize_mdisps": "filesize",
        }[name]
        assignment = f".{member} = {name},"
        index(body, assignment)
        body = body.replace(
            assignment,
            f".{member} = nullptr, // standalone: no external custom-data IO",
        )
    save("customdata_subset.cc", text, body)

    text = read(kernel + "mesh_normals.cc")
    save(
        "mesh_normals_subset.cc",
        text,
        text[
            index(text, "void BKE_lnor_spacearr_init(") : index(
                text, "namespace bke {\n\nnamespace mesh {"
            )
        ],
    )
    text = read(kernel + "deform.cc")
    save(
        "deform_subset.cc",
        text,
        text[
            index(text, "void BKE_defgroup_copy_list(") : index(
                text, "bool BKE_id_supports_vertex_groups("
            )
        ]
        + text[
            index(text, "float BKE_defvert_find_weight(") : index(
                text,
                "/* -------------------------------------------------------------------- */\n/** \\name Data Transfer",
            )
        ],
    )

    text = read(kernel + "curveprofile.cc")
    body = text[
        index(text, "namespace blender {") + len("namespace blender {") : text.rindex(
            "}  // namespace blender"
        )
    ]
    for name in ["BKE_curveprofile_blend_write", "BKE_curveprofile_blend_read"]:
        body = body.replace(function(body, "void " + name + "("), "")
    save("curveprofile_subset.cc", text, body)

    text = read(bm + "bmesh_construct.cc")
    start = index(text, "static CustomData &get_bmesh_custom_data(")
    end = index(text, "void BM_mesh_copy_init_customdata(BMesh")
    write_if_changed(
        output / "bmesh_construct_subset.cc",
        text[:start] + text[end:],
        encoding="utf-8",
    )
    text = read(bm + "bmesh_operators.cc")
    # Use the real operator engine with a deliberately small, explicit registry.
    # Definitions and algorithms are unchanged GPL excerpts, not API stubs.
    write_if_changed(output / "bmesh_operators_subset.cc", text, encoding="utf-8")
    text = read(bm + "bmesh_opdefines.cc")
    operators = ["find_doubles", "weld_verts", "recalc_face_normals"]
    body = "\n".join(
        function(text, f"static BMOpDefine bmo_{name}_def =") + ";"
        for name in operators
    )
    # Explicit enum conversions for strict C++20/MSVC aggregate initialization.
    for element in ["BM_VERT", "BM_FACE"]:
        body = body.replace(
            "{" + element + "}", "{eBMOpSlotSubType_Elem(" + element + ")}"
        )
    body += (
        "\nconst BMOpDefine *bmo_opdefines[] = {"
        + ",".join(f"&bmo_{name}_def" for name in operators)
        + "};\nconst int bmo_opdefines_total = ARRAY_SIZE(bmo_opdefines);\n"
    )
    save("bmesh_opdefines_subset.cc", text, body)

    text = read(kernel + "attribute_legacy_convert.cc")
    prefix = text[: index(text, "namespace blender::bke {")]
    body = "\n".join(
        function(text, signature)
        for signature in [
            "std::optional<AttrType> custom_data_type_to_attr_type(",
            "std::optional<eCustomDataType> attr_type_to_custom_data_type(",
        ]
    )
    write_if_changed(
        output / "attribute_types_subset.cc",
        prefix + "\nnamespace blender::bke {\n" + body + "\n}\n",
        encoding="utf-8",
    )

    support = """/* Derived from Blender Authors' GPL-2.0-or-later source.
 * Generated by extract_sources.py; original source paths are recorded below. */
#include "bmesh.hh"
#include "BKE_attribute.h"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_curve.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include <fmt/format.h>
#include "DNA_meshdata_types.h"
namespace blender {
"""
    for relative, signature in [
        (kernel + "mesh_evaluate.cc", "void BKE_mesh_mdisp_flip("),
        (kernel + "multires.cc", "void old_mdisps_bilinear("),
        (kernel + "multires.cc", "int multires_mdisp_corners("),
        (kernel + "attribute.cc", "StringRef BKE_uv_map_pin_name_get("),
        (kernel + "curve.cc", "void BKE_curve_forward_diff_bezier("),
    ]:
        text = read(relative)
        support += f"\n// Upstream: {relative}\n" + function(text, signature) + "\n"
    write_if_changed(
        output / "mesh_support_subset.cc",
        support + "} // namespace blender\n",
        encoding="utf-8",
    )
    write_if_changed(
        output / "source-excerpts.json",
        json.dumps(inventory, indent=2) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("output")
    args = parser.parse_args()
    generate(args.source, args.output)
