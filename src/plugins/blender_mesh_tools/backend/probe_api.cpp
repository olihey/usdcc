// SPDX-License-Identifier: BSD-3-Clause
// This adapter links GPL Blender code; see licenses/README.md.
#include "probe_api.h"
#include "worker.h"
#include "bmesh.hh"
#include "tools/bmesh_bevel.hh"
#include "tools/bmesh_triangulate.hh"
#include "DNA_modifier_types.h"
#include "MEM_guardedalloc.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {
using Mesh = std::unique_ptr<blender::BMesh, decltype(&blender::BM_mesh_free)>;
Mesh makeMesh() {
    blender::BMeshCreateParams params{};
    return Mesh(blender::BM_mesh_create(&blender::bm_mesh_allocsize_default, &params), blender::BM_mesh_free);
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void triangulate() {
    auto mesh = makeMesh();
    const float points[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    blender::BMVert* vertices[4];
    for (int i = 0; i != 4; ++i)
        vertices[i] = blender::BM_vert_create(mesh.get(), points[i], nullptr, blender::BM_CREATE_NOP);
    require(blender::BM_face_create_verts(mesh.get(), vertices, 4, nullptr, blender::BM_CREATE_NOP, true), "create quad");
    blender::BM_mesh_triangulate(mesh.get(), blender::MOD_TRIANGULATE_QUAD_FIXED,
                                blender::MOD_TRIANGULATE_NGON_EARCLIP, 4, false, nullptr, nullptr, nullptr);
    require(mesh->totvert == 4 && mesh->totface == 2 && mesh->totedge == 5, "quad triangulation topology");
}
void bevel() {
    auto mesh = makeMesh();
    const float points[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                              {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    const int faces[6][4] = {{3, 2, 1, 0}, {4, 5, 6, 7}, {0, 1, 5, 4},
                             {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    blender::BMVert* vertices[8];
    for (int i = 0; i != 8; ++i)
        vertices[i] = blender::BM_vert_create(mesh.get(), points[i], nullptr, blender::BM_CREATE_NOP);
    for (const auto& face : faces) {
        blender::BMVert* corners[4];
        for (int i = 0; i != 4; ++i) corners[i] = vertices[face[i]];
        require(blender::BM_face_create_verts(mesh.get(), corners, 4, nullptr, blender::BM_CREATE_NOP, true), "create cube face");
    }
    blender::BM_mesh_normals_update(mesh.get());
    blender::BMIter iterator;
    blender::BMEdge* edge;
    BM_ITER_MESH(edge, &iterator, mesh.get(), blender::BM_EDGES_OF_MESH) {
        require(blender::BM_edge_is_manifold(edge), "cube input must be manifold");
        blender::BM_elem_flag_enable(edge, blender::BM_ELEM_TAG);
        blender::BM_elem_flag_enable(edge->v1, blender::BM_ELEM_TAG);
        blender::BM_elem_flag_enable(edge->v2, blender::BM_ELEM_TAG);
    }
    blender::BM_mesh_bevel(mesh.get(), 0.1f, blender::MOD_BEVEL_AMT_OFFSET, blender::MOD_BEVEL_PROFILE_SUPERELLIPSE,
                           1, 0.5f, blender::MOD_BEVEL_AFFECT_EDGES, false, true,
                           nullptr, -1, -1, true, false, false, false, blender::MOD_BEVEL_FACE_STRENGTH_NONE,
                           blender::MOD_BEVEL_MITER_SHARP, blender::MOD_BEVEL_MITER_SHARP,
                           0.1f, nullptr, blender::MOD_BEVEL_VMESH_ADJ, -1, -1);
    require(mesh->totvert == 24 && mesh->totface == 26 && mesh->totedge == 48, "beveled cube topology");
    BM_ITER_MESH(edge, &iterator, mesh.get(), blender::BM_EDGES_OF_MESH) {
        require(blender::BM_edge_is_manifold(edge), "beveled cube must remain manifold");
    }
    blender::BMVert* vertex;
    BM_ITER_MESH(vertex, &iterator, mesh.get(), blender::BM_VERTS_OF_MESH) {
        for (float coordinate : vertex->co)
            require(std::isfinite(coordinate) && std::abs(coordinate) <= 1.001f, "bevel coordinates");
    }
}
} // namespace

const char* usdcc_blender_mesh_build() { return USDCC_BLENDER_BUILD; }
int usdcc_blender_mesh_probe(unsigned iterations, char* report, size_t capacity) {
    if (!report || !capacity) return 1;
    report[0] = '\0';
    if (capacity < 2) return 1;
    try {
        if (!iterations || iterations > 10000) throw std::invalid_argument("iterations must be in [1, 10000]");
        usdcc::blender_mesh::onWorker([&] {
            const auto before = MEM_get_memory_blocks_in_use();
            for (unsigned i = 0; i < iterations; ++i) {
                triangulate();
                bevel();
                require(MEM_get_memory_blocks_in_use() == before, "BMesh guarded allocation leak");
            }
        });
        const int length = std::snprintf(report, capacity,
            "{\"iterations\":%u,\"triangles\":2,\"bevel_vertices\":24,\"bevel_faces\":26,\"allocator_blocks_delta\":0}", iterations);
        return length >= 0 && static_cast<size_t>(length) < capacity ? 0 : 1;
    } catch (const std::exception& error) {
        std::snprintf(report, capacity, "%s", error.what());
    } catch (...) {
        std::snprintf(report, capacity, "unknown native backend failure");
    }
    return 1;
}
