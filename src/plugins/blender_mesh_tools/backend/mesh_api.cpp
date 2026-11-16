// SPDX-License-Identifier: BSD-3-Clause
// Original adapter; linked Blender implementation retains its GPL license.
#include "mesh_api.h"
#include "worker.h"
#include "bmesh.hh"
#include "BKE_customdata.hh"
#include "DNA_modifier_types.h"
#include "tools/bmesh_bevel.hh"
#include "tools/bmesh_decimate.hh"
#include "tools/bmesh_triangulate.hh"
#include "BLI_math_vector.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <chrono>

struct UsdccMeshResult {
    std::vector<float> points, normals;
    std::vector<int> counts, indices, materials, sourcePoints, sourceFaces, sourceCorners;
    std::vector<std::vector<float>> values;
    std::vector<UsdccMeshAttribute> attributes;
    std::string diagnostic;
    bool leftHanded = false;
    double importMs = 0, operationMs = 0, exportMs = 0;
};

namespace {
using namespace blender;
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
size_t domainSize(const UsdccMeshInput& in, int domain) {
    switch (domain) {
    case USDCC_MESH_POINT: return in.point_count;
    case USDCC_MESH_FACE: return in.face_count;
    case USDCC_MESH_CORNER: return in.corner_count;
    default: throw std::runtime_error("Unknown attribute domain");
    }
}
void validate(const UsdccMeshInput* input) {
    require(input && input->abi == USDCC_MESH_ABI, "Mesh ABI mismatch");
    const auto& in = *input;
    require(in.point_count && in.point_count <= USDCC_MESH_MAX_POINTS &&
            in.face_count && in.face_count <= USDCC_MESH_MAX_FACES &&
            in.corner_count && in.corner_count <= USDCC_MESH_MAX_CORNERS &&
            in.attribute_count <= USDCC_MESH_MAX_ATTRIBUTES, "Empty or oversized mesh");
    require(in.points && in.face_counts && in.corner_points && in.face_materials &&
            (!in.attribute_count || in.attributes), "Missing mesh storage");
    require(in.left_handed == 0 || in.left_handed == 1, "Invalid orientation");
    for (size_t i = 0; i < in.point_count * 3; ++i)
        require(std::isfinite(in.points[i]), "Non-finite point");
    size_t start = 0;
    std::set<std::vector<int>> faces;
    for (size_t f = 0; f < in.face_count; ++f) {
        const int count = in.face_counts[f];
        require(count >= 3 && count <= USDCC_MESH_MAX_FACE_SIZE &&
                size_t(count) <= in.corner_count - start, "Invalid polygon count");
        require(in.face_materials[f] >= -1, "Invalid material ID");
        std::vector<int> key;
        double normal[3]{};
        for (int c = 0; c < count; ++c) {
            const int v = in.corner_points[start + c];
            const int next = in.corner_points[start + (c + 1) % count];
            require(v >= 0 && size_t(v) < in.point_count && next >= 0 &&
                    size_t(next) < in.point_count, "Polygon index out of range");
            key.push_back(v);
            const float* a = in.points + size_t(v) * 3;
            const float* b = in.points + size_t(next) * 3;
            require(a[0] != b[0] || a[1] != b[1] || a[2] != b[2], "Zero-length polygon edge");
            for (int j = 0; j < 3; ++j)
                normal[j] += (double(a[(j + 1) % 3]) - b[(j + 1) % 3]) *
                             (double(a[(j + 2) % 3]) + b[(j + 2) % 3]);
        }
        std::sort(key.begin(), key.end());
        require(std::adjacent_find(key.begin(), key.end()) == key.end(), "Repeated polygon vertex");
        require(faces.insert(std::move(key)).second, "Duplicate polygon vertex set");
        require(normal[0] != 0 || normal[1] != 0 || normal[2] != 0, "Zero-area polygon");
        start += count;
    }
    require(start == in.corner_count, "Polygon counts do not match corner storage");
    size_t floats = in.point_count * 3;
    for (size_t i = 0; i < in.attribute_count; ++i) {
        const auto& a = in.attributes[i];
        require(a.components >= 1 && a.components <= 4, "Invalid attribute component count");
        require(a.value_count == domainSize(in, a.domain) * a.components && a.values,
                "Attribute cardinality mismatch");
        floats += a.value_count;
        require(floats <= 64000000, "Mesh attribute budget exceeds 256 MB");
        for (size_t j = 0; j < a.value_count; ++j)
            require(std::isfinite(a.values[j]), "Non-finite attribute");
    }
}
int addLayer(BMesh* mesh, CustomData* data, eCustomDataType type, const std::string& name) {
    BM_data_layer_add_named(mesh, data, type, name);
    const int offset = CustomData_get_offset_named(data, type, name);
    require(offset >= 0, "Cannot allocate BMesh custom-data layer");
    return offset;
}
CustomData* domainData(BMesh* mesh, int domain) {
    return domain == USDCC_MESH_POINT ? &mesh->vdata : domain == USDCC_MESH_FACE ? &mesh->pdata : &mesh->ldata;
}
template<class Element> float* floatData(Element* element, int offset) {
    return static_cast<float*>(BM_ELEM_CD_GET_VOID_P(element, offset));
}
void operate(blender::BMesh*, const UsdccMeshInput&, const UsdccMeshOptions&,
             const std::vector<int>&, int, int, int, UsdccMeshResult&);
void poll(const UsdccMeshOptions&);
void convert(const UsdccMeshInput& in, UsdccMeshOutput& out,
             const UsdccMeshOptions* options = nullptr, UsdccMeshResult* result = nullptr) {
    const auto importStart = Clock::now();
    require(options || (out.points && out.face_counts && out.corner_points && out.face_materials &&
            out.source_points && out.source_faces && out.source_corners &&
            (!in.attribute_count || out.attribute_values)), "Missing output storage");
    for (size_t a = 0; !options && a < in.attribute_count; ++a)
        require(out.attribute_values[a], "Missing attribute output storage");
    BMeshCreateParams params{};
    params.use_toolflags = options != nullptr;
    std::unique_ptr<BMesh, decltype(&BM_mesh_free)> mesh(BM_mesh_create(&bm_mesh_allocsize_default, &params), BM_mesh_free);
    require(bool(mesh), "Cannot allocate BMesh");
    addLayer(mesh.get(), &mesh->vdata, CD_PROP_INT32, ".usdcc_point");
    addLayer(mesh.get(), &mesh->pdata, CD_PROP_INT32, ".usdcc_face");
    addLayer(mesh.get(), &mesh->ldata, CD_PROP_INT32, ".usdcc_corner");
    addLayer(mesh.get(), &mesh->pdata, CD_PROP_INT32, ".usdcc_material");
    const eCustomDataType types[] = {CD_PROP_FLOAT, CD_PROP_FLOAT2, CD_PROP_FLOAT3, CD_PROP_FLOAT4};
    for (size_t a = 0; a < in.attribute_count; ++a)
        addLayer(mesh.get(), domainData(mesh.get(), in.attributes[a].domain),
                 types[in.attributes[a].components - 1], ".usdcc_attr_" + std::to_string(a));
    // CustomData sorts layers by type. Adding a new type can relocate earlier
    // layers, so resolve every offset only after the complete layout is built.
    const int vp = CustomData_get_offset_named(&mesh->vdata, CD_PROP_INT32, ".usdcc_point");
    const int fp = CustomData_get_offset_named(&mesh->pdata, CD_PROP_INT32, ".usdcc_face");
    const int lp = CustomData_get_offset_named(&mesh->ldata, CD_PROP_INT32, ".usdcc_corner");
    const int mp = CustomData_get_offset_named(&mesh->pdata, CD_PROP_INT32, ".usdcc_material");
    std::vector<int> offsets;
    for (size_t a = 0; a < in.attribute_count; ++a)
        offsets.push_back(CustomData_get_offset_named(domainData(mesh.get(), in.attributes[a].domain),
                          types[in.attributes[a].components - 1], ".usdcc_attr_" + std::to_string(a)));
    auto writeAttributes = [&](auto* element, int domain, size_t index) {
        for (size_t a = 0; a < in.attribute_count; ++a) {
            const auto& attr = in.attributes[a];
            if (attr.domain == domain)
                std::copy_n(attr.values + index * attr.components, attr.components, floatData(element, offsets[a]));
        }
    };
    auto readAttributes = [&](auto* element, int domain, size_t index) {
        for (size_t a = 0; a < in.attribute_count; ++a) {
            const auto& attr = in.attributes[a];
            if (attr.domain == domain)
                std::copy_n(floatData(element, offsets[a]), attr.components, out.attribute_values[a] + index * attr.components);
        }
    };
    std::vector<BMVert*> vertices(in.point_count);
    for (size_t v = 0; v < in.point_count; ++v) {
        if (options && v % 1024 == 0) poll(*options);
        vertices[v] = BM_vert_create(mesh.get(), in.points + v * 3, nullptr, BM_CREATE_NOP);
        require(vertices[v], "Cannot create BMesh point");
        BM_ELEM_CD_SET_INT(vertices[v], vp, int(v));
        writeAttributes(vertices[v], USDCC_MESH_POINT, v);
    }
    size_t start = 0;
    for (size_t f = 0; f < in.face_count; ++f) {
        if (options && f % 256 == 0) poll(*options);
        const size_t n = in.face_counts[f];
        std::vector<BMVert*> corners(n);
        for (size_t c = 0; c < n; ++c)
            corners[c] = vertices[in.corner_points[start + (in.left_handed ? n - 1 - c : c)]];
        BMFace* face = BM_face_create_verts(mesh.get(), corners.data(), int(n), nullptr, BM_CREATE_NOP, true);
        require(face, "Cannot create BMesh polygon");
        BM_ELEM_CD_SET_INT(face, fp, int(f));
        BM_ELEM_CD_SET_INT(face, mp, in.face_materials[f]);
        require(in.face_materials[f] <= 32765, "Too many material subsets for Blender operations");
        face->mat_nr = short(in.face_materials[f] + 1);
        writeAttributes(face, USDCC_MESH_FACE, f);
        BMLoop* loop = BM_FACE_FIRST_LOOP(face);
        for (size_t c = 0; c < n; ++c, loop = loop->next) {
            const size_t source = start + (in.left_handed ? n - 1 - c : c);
            BM_ELEM_CD_SET_INT(loop, lp, int(source));
            writeAttributes(loop, USDCC_MESH_CORNER, source);
        }
        start += n;
    }
    if (options) {
        result->importMs = milliseconds(importStart);
        operate(mesh.get(), in, *options, offsets, vp, fp, lp, *result);
        return;
    }
    // Recover order from explicit provenance layers, never BMesh table indices.
    BMIter it;
    BMVert* vertex;
    BM_ITER_MESH(vertex, &it, mesh.get(), BM_VERTS_OF_MESH) {
        const int v = BM_ELEM_CD_GET_INT(vertex, vp);
        require(v >= 0 && size_t(v) < in.point_count, "Invalid BMesh point provenance");
        out.source_points[v] = v;
        std::copy_n(vertex->co, 3, out.points + size_t(v) * 3);
        readAttributes(vertex, USDCC_MESH_POINT, v);
    }
    std::vector<size_t> starts(in.face_count);
    for (size_t f = 1; f < in.face_count; ++f) starts[f] = starts[f - 1] + in.face_counts[f - 1];
    BMFace* face;
    BM_ITER_MESH(face, &it, mesh.get(), BM_FACES_OF_MESH) {
        const int f = BM_ELEM_CD_GET_INT(face, fp);
        require(f >= 0 && size_t(f) < in.face_count && face->len == in.face_counts[f], "Invalid BMesh face provenance");
        out.source_faces[f] = f;
        out.face_counts[f] = face->len;
        out.face_materials[f] = BM_ELEM_CD_GET_INT(face, mp);
        readAttributes(face, USDCC_MESH_FACE, f);
        BMLoop* loop = BM_FACE_FIRST_LOOP(face);
        for (int c = 0; c < face->len; ++c, loop = loop->next) {
            const size_t index = starts[f] + (in.left_handed ? face->len - 1 - c : c);
            const int source = BM_ELEM_CD_GET_INT(loop, lp);
            const int point = BM_ELEM_CD_GET_INT(loop->v, vp);
            require(source >= 0 && size_t(source) < in.corner_count && point >= 0 && size_t(point) < in.point_count,
                    "Invalid BMesh corner provenance");
            out.source_corners[index] = source;
            out.corner_points[index] = point;
            readAttributes(loop, USDCC_MESH_CORNER, index);
        }
    }
}
void poll(const UsdccMeshOptions& options) {
    require(!options.cancelled || !options.cancelled(options.cancellation), "Mesh operation cancelled");
}
struct Operator {
    BMesh* mesh;
    BMOperator value{};
    Operator(BMesh* bm, const char* name) : mesh(bm) { BMO_op_init(bm, &value, BMO_FLAG_DEFAULTS, name); }
    ~Operator() { BMO_op_finish(mesh, &value); }
    void all(const char* slot, char type) { BMO_slot_buffer_from_all(mesh, &value, value.slots_in, slot, type); }
    void run() { BMO_op_exec(mesh, &value); }
};
void operate(BMesh* mesh, const UsdccMeshInput& in, const UsdccMeshOptions& options,
             const std::vector<int>& offsets, int vp, int fp, int lp, UsdccMeshResult& out) {
    const auto operationStart = Clock::now();
    poll(options);
    // Direct tools (notably bevel) use operator flags when use_toolflags is set.
    // BMO_op_exec normally initializes these, but direct calls bypass it.
    BM_mesh_elem_toolflags_ensure(mesh);
    // Decimate indexes its quadrics by vertex index; the modifier normally
    // supplies indexed meshes. A freshly constructed BMesh has dirty indices.
    BM_mesh_elem_index_ensure(mesh, BM_ALL);
    BM_mesh_normals_update(mesh);
    BMIter iterator;
    BMEdge* edge;
    // Protect every discontinuity in corner/uniform data, material membership,
    // and sharp normals. The delimiters work with generic CustomData layers.
    auto discontinuous = [&](BMEdge* e) {
        if (!BM_edge_is_manifold(e)) return true;
        const auto* a = e->l; const auto* b = a->radial_next;
        if (a->f->mat_nr != b->f->mat_nr) return true;
        for (size_t i = 0; i < in.attribute_count; ++i) {
            const auto& c = in.attributes[i];
            if (c.domain == USDCC_MESH_FACE) {
                const auto* x = floatData(a->f, offsets[i]); const auto* y = floatData(b->f, offsets[i]);
                if (!std::equal(x, x + c.components, y)) return true;
            } else if (c.domain == USDCC_MESH_CORNER) {
                for (auto* v : {e->v1, e->v2}) {
                    auto* x = floatData(a->v == v ? a : a->next, offsets[i]);
                    auto* y = floatData(b->v == v ? b : b->next, offsets[i]);
                    if (!std::equal(x, x + c.components, y)) return true;
                }
            }
        }
        return false;
    };
    bool protectedEdges = false, ambiguousNormals = false;
    BM_ITER_MESH(edge, &iterator, mesh, BM_EDGES_OF_MESH) {
        const bool protect = discontinuous(edge);
        protectedEdges |= protect;
        ambiguousNormals |= !BM_edge_is_manifold(edge);
        BM_elem_flag_set(edge, BM_ELEM_SEAM, protect);
        BM_elem_flag_set(edge, BM_ELEM_SMOOTH, !protect);
    }
    switch (options.operation) {
    case USDCC_MESH_TRIANGULATE:
        require(in.corner_count * 3 - in.face_count * 6 <= USDCC_MESH_MAX_CORNERS &&
                in.corner_count - in.face_count * 2 <= USDCC_MESH_MAX_FACES, "Triangulation exceeds output limits");
        {
            const int methods[] = {MOD_TRIANGULATE_QUAD_FIXED, MOD_TRIANGULATE_QUAD_ALTERNATE,
                MOD_TRIANGULATE_QUAD_SHORTEDGE, MOD_TRIANGULATE_QUAD_LONGEDGE, MOD_TRIANGULATE_QUAD_BEAUTY};
            BM_mesh_triangulate(mesh, methods[options.quad_method], options.ngon_method, 4, false, nullptr, nullptr, nullptr);
        }
        break;
    case USDCC_MESH_MERGE: {
        // Exact boundary signatures prevent welding across discontinuities.
        // Vertex data is kept from the target, never averaged categorically.
        std::unordered_map<BMVert*, std::vector<float>> signatures;
        BMVert* vertex;
        BM_ITER_MESH(vertex, &iterator, mesh, BM_VERTS_OF_MESH) {
            auto& signature = signatures[vertex];
            for (size_t i = 0; i < in.attribute_count; ++i) if (in.attributes[i].domain == USDCC_MESH_POINT) {
                const auto* data = floatData(vertex, offsets[i]);
                signature.insert(signature.end(), data, data + in.attributes[i].components);
            }
            std::vector<std::vector<float>> corners;
            BMIter loops; BMLoop* loop;
            BM_ITER_ELEM(loop, &loops, vertex, BM_LOOPS_OF_VERT) {
                std::vector<float> values{float(loop->f->mat_nr)};
                for (size_t i = 0; i < in.attribute_count; ++i) if (in.attributes[i].domain != USDCC_MESH_POINT) {
                    const auto* data = in.attributes[i].domain == USDCC_MESH_FACE ? floatData(loop->f, offsets[i]) : floatData(loop, offsets[i]);
                    values.insert(values.end(), data, data + in.attributes[i].components);
                }
                corners.push_back(std::move(values));
            }
            std::sort(corners.begin(), corners.end());
            corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
            signature.push_back(float(corners.size()));
            for (const auto& c : corners) signature.insert(signature.end(), c.begin(), c.end());
        }
        Operator find(mesh, "find_doubles"), weld(mesh, "weld_verts");
        find.all("verts", BM_VERT);
        BMO_slot_float_set(find.value.slots_in, "dist", options.distance);
        find.run();
        BMOIter it; BMVert* v;
        BMO_ITER(v, &it, find.value.slots_out, "targetmap.out", BM_VERT) {
            auto* target = static_cast<BMVert*>(BMO_slot_map_elem_get(BMO_slot_get(find.value.slots_out, "targetmap.out"), v));
            if (target && signatures.at(v) == signatures.at(target))
                BMO_slot_map_elem_insert(&weld.value, BMO_slot_get(weld.value.slots_in, "targetmap"), v, target);
        }
        poll(options);
        weld.run();
        out.diagnostic = "Protected UV/material/attribute boundaries; kept target vertex data.";
        break;
    }
    case USDCC_MESH_DISSOLVE:
        BM_mesh_decimate_dissolve(mesh, options.angle, false,
            BMO_Delimit(BMO_DELIM_SEAM | BMO_DELIM_SHARP | BMO_DELIM_MATERIAL));
        break;
    case USDCC_MESH_NORMALS: {
        Operator normals(mesh, "recalc_face_normals"); normals.all("faces", BM_FACE); normals.run();
        if (ambiguousNormals) out.diagnostic = "Open/non-manifold components have consistent local winding where possible; outside orientation is ambiguous.";
        break;
    }
    case USDCC_MESH_DECIMATE:
        require(!protectedEdges, "Decimate requires a closed manifold mesh without UV, sharp-normal, material, or uniform-data boundaries");
        if (options.ratio < 1) {
            // The kernel counts temporary triangles. Convert the requested
            // polygon-face target to that domain and retain triangles on output.
            // This also avoids the optional quad rejoin path entirely.
            size_t triangles = in.corner_count - in.face_count * 2;
            // Closed surfaces need at least four triangles; the collapse loop
            // removes two at a time and must not overshoot into doubled faces.
            const float factor = std::min(1.0f, std::max(4.0f, float(in.face_count) * options.ratio) / float(triangles));
            BM_mesh_decimate_collapse(mesh, factor, nullptr, 0, true, -1, 0.00001f);
        }
        break;
    case USDCC_MESH_BEVEL: {
        require(in.corner_count * size_t(options.segments + 1) * (options.segments + 1) * 8 <= USDCC_MESH_MAX_CORNERS,
                "Bevel exceeds conservative output budget; reduce segments or input size");
        BM_ITER_MESH(edge, &iterator, mesh, BM_EDGES_OF_MESH) {
            require(BM_edge_is_manifold(edge), "Bevel requires a closed manifold mesh");
            const float cosine = std::clamp(dot_v3v3(edge->l->f->no, edge->l->radial_next->f->no), -1.0f, 1.0f);
            const bool tag = std::acos(cosine) >= options.angle;
            BM_elem_flag_set(edge, BM_ELEM_TAG, tag);
            if (tag) { BM_elem_flag_enable(edge->v1, BM_ELEM_TAG); BM_elem_flag_enable(edge->v2, BM_ELEM_TAG); }
        }
        if (options.bevel_material >= 0) {
            bool exists = false;
            for (size_t f = 0; f < in.face_count; ++f) exists |= in.face_materials[f] == options.bevel_material;
            require(exists, "Bevel material must be an existing subset ID");
        }
        BM_mesh_bevel(mesh, options.distance, MOD_BEVEL_AMT_OFFSET, MOD_BEVEL_PROFILE_SUPERELLIPSE,
            options.segments, options.profile, MOD_BEVEL_AFFECT_EDGES, false, options.clamp_overlap != 0,
            nullptr, -1, options.bevel_material < 0 ? -1 : options.bevel_material + 1,
            true, false, false, false, MOD_BEVEL_FACE_STRENGTH_NONE, MOD_BEVEL_MITER_SHARP, MOD_BEVEL_MITER_SHARP,
            0.1f, nullptr, MOD_BEVEL_VMESH_ADJ, -1, -1);
        break;
    }
    default: throw std::runtime_error("Unknown mesh operation");
    }
    poll(options);
    BM_mesh_normals_update(mesh);
    require(mesh->totvert > 0 && mesh->totface > 0 && mesh->totvert <= USDCC_MESH_MAX_POINTS &&
            mesh->totface <= USDCC_MESH_MAX_FACES && mesh->totloop <= USDCC_MESH_MAX_CORNERS, "Operation produced empty or oversized geometry");
    out.operationMs = milliseconds(operationStart);
    const auto exportStart = Clock::now();
    out.leftHanded = in.left_handed != 0;
    out.values.resize(in.attribute_count);
    std::vector<int> cornerFace(in.corner_count);
    size_t begin = 0;
    for (size_t f = 0; f < in.face_count; ++f) {
        std::fill_n(cornerFace.begin() + begin, in.face_counts[f], int(f)); begin += in.face_counts[f];
    }
    auto readAttributes = [&](auto* element, int domain) {
        for (size_t i = 0; i < in.attribute_count; ++i) if (in.attributes[i].domain == domain) {
            const auto* data = floatData(element, offsets[i]);
            out.values[i].insert(out.values[i].end(), data, data + in.attributes[i].components);
        }
    };
    std::unordered_map<BMVert*, int> pointIndex;
    BMVert* vertex;
    BM_ITER_MESH(vertex, &iterator, mesh, BM_VERTS_OF_MESH) {
        pointIndex[vertex] = int(out.points.size() / 3);
        out.points.insert(out.points.end(), vertex->co, vertex->co + 3);
        int source = BM_ELEM_CD_GET_INT(vertex, vp);
        if (source < 0 || size_t(source) >= in.point_count ||
            !std::equal(vertex->co, vertex->co + 3, in.points + size_t(source) * 3)) source = -1;
        out.sourcePoints.push_back(source);
        readAttributes(vertex, USDCC_MESH_POINT);
    }
    BMFace* face;
    BM_ITER_MESH(face, &iterator, mesh, BM_FACES_OF_MESH) {
        require(face->len <= USDCC_MESH_MAX_FACE_SIZE, "Result polygon exceeds corner limit");
        out.counts.push_back(face->len);
        out.materials.push_back(int(face->mat_nr) - 1);
        int sourceFace = BM_ELEM_CD_GET_INT(face, fp);
        if (sourceFace < 0 || size_t(sourceFace) >= in.face_count) sourceFace = -1;
        readAttributes(face, USDCC_MESH_FACE);
        auto* loop = in.left_handed ? BM_FACE_FIRST_LOOP(face)->prev : BM_FACE_FIRST_LOOP(face);
        for (int c = 0; c < face->len; ++c, loop = in.left_handed ? loop->prev : loop->next) {
            const int v = pointIndex.at(loop->v);
            out.indices.push_back(v);
            int source = BM_ELEM_CD_GET_INT(loop, lp);
            if (source < 0 || size_t(source) >= in.corner_count || out.sourcePoints[v] != in.corner_points[source]) source = -1;
            if (source < 0 || cornerFace[source] != sourceFace) sourceFace = -1;
            out.sourceCorners.push_back(source);
            out.normals.insert(out.normals.end(), face->no, face->no + 3);
            readAttributes(loop, USDCC_MESH_CORNER);
        }
        out.sourceFaces.push_back(sourceFace);
    }
    if (options.operation == USDCC_MESH_DECIMATE)
        out.diagnostic = "Achieved " + std::to_string(out.counts.size()) + " faces from " + std::to_string(in.face_count) + ".";
    poll(options);
    for (size_t i = 0; i < in.attribute_count; ++i)
        out.attributes.push_back({in.attributes[i].domain, in.attributes[i].components, out.values[i].data(), out.values[i].size()});
    out.exportMs = milliseconds(exportStart);
}
template<class Function> int guarded(char* error, size_t capacity, Function&& function) {
    if (!error || !capacity) return 1;
    error[0] = '\0';
    try { function(); return 0; }
    catch (const std::exception& e) { std::snprintf(error, capacity, "%s", e.what()); }
    catch (...) { std::snprintf(error, capacity, "Unknown mesh conversion failure"); }
    return 1;
}
}
int usdcc_blender_mesh_validate(const UsdccMeshInput* input, char* error, size_t capacity) {
    return guarded(error, capacity, [&] { validate(input); });
}
int usdcc_blender_mesh_roundtrip(const UsdccMeshInput* input, UsdccMeshOutput* output, char* error, size_t capacity) {
    return guarded(error, capacity, [&] {
        validate(input);
        require(output, "Missing output descriptor");
        usdcc::blender_mesh::onWorker([&] {
            const auto before = MEM_get_memory_blocks_in_use();
            convert(*input, *output);
            require(before == MEM_get_memory_blocks_in_use(), "BMesh conversion leaked guarded allocations");
        });
    });
}
UsdccMeshResult* usdcc_blender_mesh_execute(const UsdccMeshInput* input, const UsdccMeshOptions* options,
                                          char* error, size_t capacity) {
    std::unique_ptr<UsdccMeshResult> result;
    const int status = guarded(error, capacity, [&] {
        validate(input);
        require(options && options->abi == USDCC_MESH_ABI, "Operation ABI mismatch");
        require(options->operation >= USDCC_MESH_TRIANGULATE && options->operation <= USDCC_MESH_BEVEL &&
                std::isfinite(options->distance) && options->distance >= 0 &&
                std::isfinite(options->angle) && options->angle >= 0 && options->angle <= 3.141593f &&
                std::isfinite(options->ratio) && options->ratio > 0 && options->ratio <= 1 &&
                std::isfinite(options->profile) && options->profile >= 0.01f && options->profile <= 0.99f &&
                options->segments >= 1 && options->segments <= 16 && options->quad_method >= 0 && options->quad_method <= 4 &&
                options->ngon_method >= 0 && options->ngon_method <= 1 && options->bevel_material >= -1,
                "Invalid mesh operation parameters");
        poll(*options);
        auto value = std::make_unique<UsdccMeshResult>();
        usdcc::blender_mesh::onWorker([&] {
            const auto before = MEM_get_memory_blocks_in_use();
            UsdccMeshOutput unused{};
            convert(*input, unused, options, value.get());
            require(before == MEM_get_memory_blocks_in_use(), "Mesh operation leaked guarded allocations");
        });
        UsdccMeshResultView view{};
        usdcc_blender_mesh_result_view(value.get(), &view);
        validate(&view.mesh);
        for (float normal : value->normals) require(std::isfinite(normal), "Invalid result normals");
        poll(*options);
        result = std::move(value);
    });
    return status == 0 ? result.release() : nullptr;
}
int usdcc_blender_mesh_result_view(const UsdccMeshResult* result, UsdccMeshResultView* view) {
    if (!result || !view) return 1;
    const auto& r = *result;
    view->mesh = {USDCC_MESH_ABI, r.points.size() / 3, r.counts.size(), r.indices.size(), r.attributes.size(),
        r.points.data(), r.counts.data(), r.indices.data(), r.materials.data(), r.attributes.data(), r.leftHanded};
    view->source_points = r.sourcePoints.data(); view->source_faces = r.sourceFaces.data(); view->source_corners = r.sourceCorners.data();
    view->normals = r.normals.data(); view->diagnostic = r.diagnostic.c_str();
    return 0;
}
void usdcc_blender_mesh_result_free(UsdccMeshResult* result) { delete result; }
int usdcc_blender_mesh_result_timings(const UsdccMeshResult* result, double* import_ms, double* operation_ms, double* export_ms) {
    if (!result || !import_ms || !operation_ms || !export_ms) return 1;
    *import_ms = result->importMs; *operation_ms = result->operationMs; *export_ms = result->exportMs;
    return 0;
}
void usdcc_blender_mesh_shutdown() { usdcc::blender_mesh::executor().stop(); }
