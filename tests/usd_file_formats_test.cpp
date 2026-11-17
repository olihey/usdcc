// The file formats the USD plugin packs add (docs/USD_PLUGINS.md): each sample in
// test_data/formats opens as a stage holding its one triangle. A format this
// OpenUSD install has no plugin for is skipped with a line saying so, so the
// tests pass against a plain OpenUSD; CI's has the packs (tools/fetch_usd.py),
// and a release's deploy is checked for them by tools/ci/smoke_test.py.
#include "test_harness.h"

#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>

#include <cstdio>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

USDCC_TEST(usd_plugin_pack_file_formats) {
    int checked = 0;
    for (const std::string extension : {"gltf", "obj", "ply", "stl"}) {
        if (!SdfFileFormat::FindByExtension(extension)) {
            std::printf("  skipped .%s: this OpenUSD install has no file format for it\n", extension.c_str());
            continue;
        }
        ++checked;
        const std::string file = std::string(USDCC_TEST_DATA_DIR) + "/formats/triangle." + extension;
        const UsdStageRefPtr stage = UsdStage::Open(file);
        CHECK(stage, "triangle.%s opens", extension.c_str());
        if (!stage) continue;

        int meshes = 0;
        VtIntArray counts;
        VtVec3fArray points;
        for (const UsdPrim& prim : stage->Traverse()) {
            if (!prim.IsA<UsdGeomMesh>()) continue;
            ++meshes;
            UsdGeomMesh(prim).GetFaceVertexCountsAttr().Get(&counts);
            UsdGeomMesh(prim).GetPointsAttr().Get(&points);
        }
        CHECK(meshes == 1, "triangle.%s has one mesh: %d", extension.c_str(), meshes);
        CHECK(counts.size() == 1 && counts[0] == 3 && points.size() == 3,
              "triangle.%s's mesh is one triangle: %zu face(s), %zu point(s)", extension.c_str(), counts.size(), points.size());
    }
    std::printf("  %d of 4 file formats checked\n", checked);
}
