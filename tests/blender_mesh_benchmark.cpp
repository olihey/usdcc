// SPDX-License-Identifier: BSD-3-Clause
#include "conversion.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <QCoreApplication>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <atomic>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#endif

namespace bm = usdcc::blender_mesh;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point begin) { return std::chrono::duration<double, std::milli>(Clock::now() - begin).count(); }
size_t peakMemory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info{};
    GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
    return info.PeakWorkingSetSize;
#else
    std::ifstream status("/proc/self/status"); std::string word;
    while (status >> word) if (word == "VmHWM:") { size_t kb; status >> kb; return kb * 1024; }
    return 0;
#endif
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    PXR_NAMESPACE_USING_DIRECTIVE
    const int requested = argc > 1 ? std::stoi(argv[1]) : 10000;
    if (requested < 2 || requested > 1000000) return 2;
    const int n = int(std::floor(std::sqrt(requested / 2.0)));
    const size_t faces = size_t(n) * n * 2;
    auto usdStage = UsdStage::CreateInMemory();
    auto mesh = UsdGeomMesh::Define(usdStage, SdfPath("/Mesh"));
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
    VtVec3fArray points(size_t(n + 1) * (n + 1));
    for (int y = 0; y <= n; ++y) for (int x = 0; x <= n; ++x) points[size_t(y) * (n + 1) + x] = GfVec3f(float(x), float(y), 0);
    VtIntArray counts(faces, 3), indices(faces * 3);
    size_t c = 0;
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        const int v = y * (n + 1) + x;
        for (int index : {v, v+1, v+n+2, v, v+n+2, v+n+1}) indices[c++] = index;
    }
    mesh.CreatePointsAttr().Set(points); mesh.CreateFaceVertexCountsAttr().Set(counts); mesh.CreateFaceVertexIndicesAttr().Set(indices);
    usdcc::usd::StageManager stages; auto stage = stages.adoptStage(usdStage); usdcc::usd::UndoManager undo;
    bm::Snapshot snapshot; bm::Result result; std::string error;
    const auto total = Clock::now(); auto start = total;
    if (!bm::capture(usdStage, mesh.GetPath(), snapshot, error)) { std::cerr << error; return 1; }
    const double snapshotMs = ms(start); start = Clock::now();
    if (argc > 2 && std::string(argv[2]) == "cancel") {
        std::atomic_bool cancel = false;
        auto options = bm::defaultOptions(USDCC_MESH_NORMALS);
        options.cancellation = &cancel;
        options.cancelled = [](void* p) { return static_cast<std::atomic_bool*>(p)->load() ? 1 : 0; };
        bool success = false;
        std::thread worker([&] { success = bm::execute(snapshot.mesh, options, result, error); });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        start = Clock::now(); cancel = true; worker.join();
        const double latency = ms(start);
        usdcc_blender_mesh_shutdown();
        if (success || error.find("cancel") == std::string::npos || undo.stack(stage)->count()) return 1;
        std::cout << "{\"faces\":" << faces << ",\"cancel_latency_ms\":" << latency << ",\"peak_process_bytes\":" << peakMemory() << "}\n";
        return 0;
    }
    if (!bm::execute(snapshot.mesh, bm::defaultOptions(USDCC_MESH_NORMALS), result, error) || !bm::validateResult(snapshot, result, error)) {
        std::cerr << error; return 1;
    }
    const double backendMs = ms(start); start = Clock::now();
    auto prepared = bm::resultLayer(snapshot, result, error);
    if (!prepared) { std::cerr << error; return 1; }
    const double preparationMs = ms(start); start = Clock::now();
    undo.beginEdit(stage, "Benchmark normals");
    if (!bm::author(usdStage, snapshot, result, error)) { undo.cancelEdit(stage); std::cerr << error; return 1; }
    if (!undo.endEdit(stage)) { std::cerr << "No undo record"; return 1; }
    const double commitMs = ms(start);
    std::cout << "{\"requested_faces\":" << requested << ",\"faces\":" << faces << ",\"points\":" << points.size()
        << ",\"snapshot_ms\":" << snapshotMs << ",\"backend_and_validation_ms\":" << backendMs
        << ",\"native_import_ms\":" << result.importMs << ",\"native_operation_ms\":" << result.operationMs << ",\"native_export_ms\":" << result.exportMs
        << ",\"preparation_ms\":" << preparationMs << ",\"commit_and_verify_ms\":" << commitMs
        << ",\"total_ms\":" << ms(total) << ",\"peak_process_bytes\":" << peakMemory()
        << ",\"backend\":\"" << usdcc_blender_mesh_build() << "\"}\n";
    usdcc_blender_mesh_shutdown();
}
