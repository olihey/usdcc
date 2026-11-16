// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "mesh_api.h"
#include <pxr/base/vt/value.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/stage.h>
#include <string>
#include <vector>

namespace usdcc::blender_mesh {
struct Channel {
    std::string name;
    int domain = USDCC_MESH_POINT;
    int components = 1;
    std::vector<float> values;
};
struct MeshData {
    std::vector<float> points;
    std::vector<int> counts, indices, materials;
    std::vector<Channel> channels;
    bool leftHanded = false;
};
struct Primvar {
    PXR_NS::TfToken name, interpolation;
    PXR_NS::SdfValueTypeName type;
    PXR_NS::VtValue values;
    PXR_NS::VtIntArray indices;
    int elementSize = 1;
    int unauthoredIndex = -1;
    bool indexed = false;
    int channel = -1; // constant values stay in USD, outside BMesh
};
struct Subset {
    PXR_NS::SdfPath path;
    PXR_NS::VtIntArray indices;
};
// Owned data only: no stage, prim, attribute, or layer handles reach a worker.
struct Snapshot {
    PXR_NS::SdfPath path;
    MeshData mesh;
    std::vector<Primvar> primvars;
    std::vector<Subset> subsets;
    PXR_NS::TfToken normalsInterpolation;
    int normalsChannel = -1;
};
struct Result {
    MeshData mesh;
    std::vector<int> sourcePoints, sourceFaces, sourceCorners;
    std::vector<float> normals;
    int operation = 0; // zero is the strict, unmodified round trip
    std::string diagnostic;
    double importMs = 0, operationMs = 0, exportMs = 0;
};
// Capture on the caller's USD/UI thread. All failures leave output untouched.
bool capture(const PXR_NS::UsdStageRefPtr&, const PXR_NS::SdfPath&, Snapshot&, std::string& error);
bool roundTrip(const Snapshot&, Result&, std::string& error);
bool validateResult(const Snapshot&, const Result&, std::string& error);
UsdccMeshOptions defaultOptions(int operation);
bool execute(const MeshData&, const UsdccMeshOptions&, Result&, std::string& error);
bool changed(const Snapshot&, const Result&);
bool sameSnapshot(const Snapshot&, const Snapshot&);
// Caller owns a fresh UndoManager session and must roll back on failure.
bool author(const PXR_NS::UsdStageRefPtr&, const Snapshot&, const Result&, std::string& error);
bool verify(const PXR_NS::UsdStageRefPtr&, const Snapshot&, const Result&, std::string& error);
// Validated anonymous override used by the commit path and composition tests.
PXR_NS::SdfLayerRefPtr resultLayer(const Snapshot&, const Result&, std::string& error);
}
