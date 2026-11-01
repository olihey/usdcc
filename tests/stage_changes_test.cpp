// Stage::changed: UsdNotice::ObjectsChanged re-emitted as plain data, and the
// StageChanges helpers panels use to decide between "update a value" and
// "rebuild".
#include "test_harness.h"

#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/xform.h>

#include <QUndoStack>

using namespace PXR_NS;
using usdcc::usd::StageChanges;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;

USDCC_TEST(stage_changes) {
    StageManager manager;
    StageRefPtr stage = manager.createStage();
    const UsdStageRefPtr usd = stage->usdStage();
    UsdGeomXform::Define(usd, SdfPath("/World"));
    UsdGeomCube cube = UsdGeomCube::Define(usd, SdfPath("/World/Cube"));
    UsdGeomCube::Define(usd, SdfPath("/World/Other"));
    cube.GetSizeAttr().Set(1.0);

    std::vector<StageChanges> received;
    auto connection = stage->changed.connect([&](const StageChanges& c) { received.push_back(c); });

    const SdfPath cubePath("/World/Cube");
    const SdfPath otherPath("/World/Other");

    cube.GetSizeAttr().Set(2.0);
    CHECK(received.size() == 1, "one notice per edit (got %zu)", received.size());
    if (!received.empty()) {
        const StageChanges& c = received.back();
        CHECK(c.changedInfoOnlyPaths.size() == 1 && c.changedInfoOnlyPaths[0] == SdfPath("/World/Cube.size"),
              "value change reported as info-only on the attribute");
        CHECK(c.affectsPrim(cubePath), "affects the cube");
        CHECK(!c.affectsPrim(otherPath), "does not affect a sibling");
        CHECK(!c.resyncsPrim(cubePath), "a value change is not a resync");
    }

    received.clear();
    cube.GetPrim().CreateAttribute(TfToken("userProperties:weight"), SdfValueTypeNames->Float);
    CHECK(!received.empty() && received.back().resyncsPrim(cubePath), "adding a property resyncs the prim");
    CHECK(!received.empty() && !received.back().resyncsPrim(otherPath), "...but not its sibling");

    received.clear();
    usd->RemovePrim(SdfPath("/World"));
    CHECK(!received.empty() && received.back().resyncsPrim(cubePath), "removing an ancestor resyncs the prim");

    // Undo (a layer import) also notifies, which is what keeps panels in sync.
    usdcc::usd::UndoManager undo;
    UsdGeomCube again = UsdGeomCube::Define(usd, cubePath);
    received.clear();
    undo.apply(stage, "Size", [&]() { return again.GetSizeAttr().Set(5.0); });
    received.clear();
    undo.stack(stage)->undo();
    CHECK(!received.empty() && received.back().affectsPrim(cubePath), "undo notifies too");

    connection.disconnect();
    received.clear();
    again.GetSizeAttr().Set(7.0);
    CHECK(received.empty(), "disconnected listener gets nothing");
}
