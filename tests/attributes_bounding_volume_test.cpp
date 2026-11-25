// The Attribute Editor's Bounding Volume section: shown for prims with bounds, in stage
// units, following edits and time. Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/property_sheet.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <memory>

using usdcc::usd::AttributesViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// Long enough for slow bounds' delay too.
void rest() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 600) QApplication::processEvents(QEventLoop::AllEvents, 20);
}

// Looked up each time: a rebuild replaces it.
QWidget* section(AttributesViewPanel& panel) {
    return panel.sheet()->scrollArea()->widget()->findChild<QWidget*>(QStringLiteral("boundingVolumeSection"));
}

bool shown(AttributesViewPanel& panel) {
    QWidget* s = section(panel);
    return s && s->isVisible();
}

VtValue value(AttributesViewPanel& panel, const char* label) {
    if (QWidget* s = section(panel)) {
        for (auto* row : s->findChildren<usdcc::usd::properties::PropertyRow*>()) {
            if (row->labelText() == QLatin1String(label)) return row->editor()->value();
        }
    }
    return VtValue();
}

bool is(const VtValue& v, const GfVec3d& wanted) {
    return v.IsHolding<GfVec3d>() && GfIsClose(v.UncheckedGet<GfVec3d>(), wanted, 1e-6);
}
}  // namespace

USDCC_TEST(attributes_bounding_volume_section) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    const UsdPrim group = s->DefinePrim(SdfPath("/Group"), TfToken("Xform"));
    UsdGeomCube cube = UsdGeomCube::Define(s, SdfPath("/Group/Cube"));
    s->DefinePrim(SdfPath("/Empty"), TfToken("Xform"));
    s->DefinePrim(SdfPath("/Mat"), TfToken("Material"));
    s->DefinePrim(SdfPath("/Set"), TfToken("Scope"));
    UsdGeomCube::Define(s, SdfPath("/Set/Cube"));

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.show();
    const auto select = [&](const char* path) {
        stages.setSelectedPaths(stage, {SdfPath(path)});
        rest();
    };
    for (const char* path : {"/Empty", "/Mat"}) {
        select(path);
        CHECK(!shown(panel), "no bounds, no section: %s", path);
    }
    for (const char* path : {"/Group", "/Group/Cube", "/Set"}) {
        select(path);
        CHECK(shown(panel), "bounds, its own or its children's: %s", path);
        CHECK(is(value(panel, "Size"), GfVec3d(2)), "its size: %s", path);
    }

    select("/Group/Cube");
    UsdGeomXformCommonAPI(group).SetTranslate(GfVec3d(10, 0, 0));
    rest();
    CHECK(is(value(panel, "Center"), GfVec3d(10, 0, 0)), "moved by its parent");
    CHECK(is(value(panel, "Min"), GfVec3d(9, -1, -1)) && is(value(panel, "Max"), GfVec3d(11, 1, 1)), "its corners");
    cube.GetSizeAttr().Set(4.0, UsdTimeCode(0));
    cube.GetSizeAttr().Set(8.0, UsdTimeCode(10));
    stage->setTime(10);
    rest();
    CHECK(is(value(panel, "Size"), GfVec3d(8)), "at the stage's time");

    UsdGeomSetStageMetersPerUnit(s, 1.0);
    rest();
    CHECK(value(panel, "Units") == VtValue(std::string("1 unit = 1 m")), "the stage's units");

    panel.sheet()->setSearchText(QStringLiteral("size"));
    rest();
    CHECK(!shown(panel), "a search hides it");
    panel.sheet()->setSearchText(QString());
    rest();
    CHECK(shown(panel), "and it comes back");

    select("/Group");
    s->RemovePrim(SdfPath("/Group/Cube"));
    rest();
    CHECK(!shown(panel), "gone with the geometry");
}
