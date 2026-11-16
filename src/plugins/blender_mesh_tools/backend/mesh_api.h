// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "probe_api.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum UsdccMeshDomain { USDCC_MESH_POINT = 0, USDCC_MESH_FACE = 1, USDCC_MESH_CORNER = 2 };
enum { USDCC_MESH_ABI = 1, USDCC_MESH_MAX_POINTS = 1000000,
       USDCC_MESH_MAX_FACES = 1000000, USDCC_MESH_MAX_CORNERS = 4000000,
       USDCC_MESH_MAX_ATTRIBUTES = 64, USDCC_MESH_MAX_FACE_SIZE = 4096 };
typedef struct UsdccMeshAttribute {
    int domain;                 // point, face, or corner
    int components;             // 1..4 floats per domain element
    const float* values;
    size_t value_count;         // number of floats, not elements
} UsdccMeshAttribute;
typedef struct UsdccMeshInput {
    uint32_t abi;
    size_t point_count, face_count, corner_count, attribute_count;
    const float* points;        // tightly packed xyz, mesh-local coordinates
    const int* face_counts;
    const int* corner_points;
    const int* face_materials;   // categorical IDs; -1 means no subset
    const UsdccMeshAttribute* attributes;
    int left_handed;            // reverse loops into BMesh, restore on output
} UsdccMeshInput;
typedef struct UsdccMeshOutput {
    // All storage belongs to the caller. Capacity is implicit in input counts;
    // roundtrip never changes counts. Failed calls leave output unspecified.
    float* points;
    int* face_counts;
    int* corner_points;
    int* face_materials;
    float** attribute_values;
    int* source_points;
    int* source_faces;
    int* source_corners;
} UsdccMeshOutput;
// Validation is also used before starting the worker. No Blender types or owned
// allocations cross this C ABI. Diagnostics are NUL-terminated when capacity > 0.
USDCC_BLENDER_API int usdcc_blender_mesh_validate(const UsdccMeshInput*, char*, size_t);
USDCC_BLENDER_API int usdcc_blender_mesh_roundtrip(const UsdccMeshInput*, UsdccMeshOutput*, char*, size_t);
enum UsdccMeshOperation { USDCC_MESH_TRIANGULATE = 1, USDCC_MESH_MERGE = 2,
    USDCC_MESH_DISSOLVE = 3, USDCC_MESH_NORMALS = 4, USDCC_MESH_DECIMATE = 5, USDCC_MESH_BEVEL = 6 };
typedef struct UsdccMeshOptions {
    uint32_t abi;
    int operation;
    float distance;             // merge distance / bevel width, mesh-local units
    float angle;                // radians; dissolve threshold / bevel minimum angle
    float ratio;                // decimation target ratio
    float profile;              // bevel [0,1]
    int segments;               // bevel [1,16]
    int quad_method;            // 0 fixed, 1 alternate, 2 shortest, 3 longest, 4 beauty
    int ngon_method;            // 0 beauty, 1 ear clip
    int clamp_overlap;
    int bevel_material;         // -1 adjacent; otherwise existing categorical material ID
    int (*cancelled)(void*);    // optional; polled between safe stages, never inside unsafe native calls
    void* cancellation;
} UsdccMeshOptions;
typedef struct UsdccMeshResult UsdccMeshResult;
typedef struct UsdccMeshResultView {
    UsdccMeshInput mesh;
    const int *source_points, *source_faces, *source_corners;
    const float* normals;       // 3 floats per corner, right-handed shading normals
    const char* diagnostic;
} UsdccMeshResultView;
// Result buffers are immutable and valid until result_free. Allocate and free
// inside this DLL; the caller copies plain data before freeing the handle.
USDCC_BLENDER_API UsdccMeshResult* usdcc_blender_mesh_execute(const UsdccMeshInput*, const UsdccMeshOptions*, char*, size_t);
USDCC_BLENDER_API int usdcc_blender_mesh_result_view(const UsdccMeshResult*, UsdccMeshResultView*);
USDCC_BLENDER_API int usdcc_blender_mesh_result_timings(const UsdccMeshResult*, double* import_ms, double* operation_ms, double* export_ms);
USDCC_BLENDER_API void usdcc_blender_mesh_result_free(UsdccMeshResult*);
USDCC_BLENDER_API void usdcc_blender_mesh_shutdown(void);
#ifdef __cplusplus
}
#endif
