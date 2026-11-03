// The Outliner's names, coloured and weighted as in usdview
// (docs/OUTLINER_NAMES.md): instances, composition arcs, instance proxies,
// specifiers and inactive prims, the tooltips, an instance's proxies loaded
// on demand and read-only, and a coloured name keeping its colour on a
// selected row. Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/outliner_item_delegate.h"
#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QFont>
#include <QImage>
#include <QTemporaryDir>
#include <QTreeView>

#include <cstdlib>

using usdcc::usd::OutlinerItemDelegate;
using usdcc::usd::UsdPrimTreeModel;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const QColor kInstance(135, 206, 250);
const QColor kArcs(222, 158, 46);
const QColor kPrototype(118, 136, 217);

UsdStageRefPtr testStage() {
    const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
    layer->ImportFromString(R"(#usda 1.0

def Xform "src"
{
    def Cube "geo"
    {
    }
}

def Xform "inst" (
    instanceable = true
    references = </src>
)
{
}

def Xform "ref" (
    references = </src>
)
{
}

def Xform "looks" (
    variants = {
        string look = "red"
    }
    prepend variantSets = "look"
)
{
    variantSet "look" = {
        "red" {
        }
    }
}

over "patch"
{
}

class Xform "_class_Prop"
{
}

def Xform "off" (
    active = false
)
{
}

def Xform "refOff" (
    active = false
    references = </src>
)
{
}
)");
    return UsdStage::Open(layer);
}

QColor colorOf(const UsdPrimTreeModel& model, const char* path) {
    return model.data(model.indexForPath(SdfPath(path)), Qt::ForegroundRole).value<QColor>();
}

QFont fontOf(const UsdPrimTreeModel& model, const char* path) {
    return model.data(model.indexForPath(SdfPath(path)), Qt::FontRole).value<QFont>();
}

QString tipOf(const UsdPrimTreeModel& model, const char* path) {
    return model.data(model.indexForPath(SdfPath(path)), Qt::ToolTipRole).toString();
}

bool closeTo(QRgb pixel, const QColor& color, int tolerance) {
    return std::abs(qRed(pixel) - color.red()) <= tolerance && std::abs(qGreen(pixel) - color.green()) <= tolerance &&
           std::abs(qBlue(pixel) - color.blue()) <= tolerance;
}
}  // namespace

USDCC_TEST(outliner_names_like_usdview) {
    const UsdStageRefPtr stage = testStage();
    UsdPrimTreeModel model;
    model.setStage(stage);

    CHECK(!colorOf(model, "/src").isValid(), "a plain prim keeps the theme's colour");
    CHECK(colorOf(model, "/inst") == kInstance, "an instance: light sky blue");
    CHECK(colorOf(model, "/ref") == kArcs && colorOf(model, "/looks") == kArcs,
          "references and variant sets: dark yellow");
    CHECK(colorOf(model, "/refOff") == kArcs.darker(150), "inactive: the same colour, darker");
    CHECK(colorOf(model, "/off") == QColor(255, 255, 255, 191).darker(150), "an inactive plain prim: the theme's, darker");

    CHECK(fontOf(model, "/src").weight() == QFont::Bold && !fontOf(model, "/src").italic(), "a def is bold");
    CHECK(fontOf(model, "/patch").italic() && fontOf(model, "/patch").weight() == QFont::Light, "an over: light italic");
    CHECK(fontOf(model, "/_class_Prop").weight() == QFont::Normal && !fontOf(model, "/_class_Prop").italic(),
          "a class: normal");
    CHECK(!fontOf(model, "/off").italic(), "inactive is no longer italic");

    CHECK(tipOf(model, "/inst").contains("Instanced Defined Xform Prim"), "tooltip: [%s]", qPrintable(tipOf(model, "/inst")));
    CHECK(tipOf(model, "/ref").startsWith("/ref\n") && tipOf(model, "/ref").contains("Has composition arcs"),
          "tooltip: [%s]", qPrintable(tipOf(model, "/ref")));
    CHECK(tipOf(model, "/off").contains("Inactive Defined Xform Prim") && tipOf(model, "/patch").contains("Undefined Prim") &&
              tipOf(model, "/_class_Prop").contains("Abstract Xform Prim"),
          "tooltips for inactive, over and class");
}

USDCC_TEST(outliner_instance_proxies) {
    const UsdStageRefPtr stage = testStage();
    UsdPrimTreeModel model;
    model.setStage(stage);

    const QModelIndex inst = model.indexForPath(SdfPath("/inst"));
    CHECK(model.hasChildren(inst) && model.rowCount(inst) == 0 && model.canFetchMore(inst),
          "an instance's proxies wait until it is expanded");
    CHECK(model.rowCount(model.indexForPath(SdfPath("/ref"))) == 1, "a plain reference's children are there at once");
    model.fetchMore(inst);
    CHECK(model.rowCount(inst) == 1 && !model.canFetchMore(inst), "fetched: %d row(s)", model.rowCount(inst));

    const QModelIndex geo = model.indexForPath(SdfPath("/inst/geo"));
    CHECK(geo.isValid() && colorOf(model, "/inst/geo") == kPrototype, "an instance proxy: slate blue");
    CHECK(tipOf(model, "/inst/geo").contains("Prototype"), "tooltip: [%s]", qPrintable(tipOf(model, "/inst/geo")));
    CHECK(!(model.flags(geo) & Qt::ItemIsEditable) && !(model.flags(geo) & Qt::ItemIsUserCheckable) &&
              !model.data(geo, Qt::CheckStateRole).isValid(),
          "read-only: no rename, no checkbox");
    CHECK(!model.setData(geo, QStringLiteral("renamed"), Qt::EditRole) && !model.setData(geo, Qt::Unchecked, Qt::CheckStateRole),
          "and edits are refused");

    // A fresh model loads them on the way to a path (a viewport pick).
    UsdPrimTreeModel fresh;
    fresh.setStage(stage);
    CHECK(!fresh.indexForPath(SdfPath("/inst/geo")).isValid() && fresh.fetchPath(SdfPath("/inst/geo")).isValid(),
          "fetchPath() loads the proxies down to a path");
}

USDCC_TEST(outliner_selected_name_keeps_its_colour) {
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    UsdPrimTreeModel model;
    model.setStage(testStage());
    QTreeView view;
    view.setModel(&model);
    view.setItemDelegate(new OutlinerItemDelegate(&view));
    view.resize(320, 300);
    view.show();
    const QModelIndex inst = model.indexForPath(SdfPath("/inst"));
    view.selectionModel()->select(inst, QItemSelectionModel::ClearAndSelect);
    QApplication::processEvents();

    const QImage row = view.viewport()->grab(view.visualRect(inst)).toImage();
    bool blue = false;
    for (int y = 0; y < row.height() && !blue; ++y) {
        for (int x = 0; x < row.width() && !blue; ++x) blue = closeTo(row.pixel(x, y), kInstance, 30);
    }
    CHECK(blue, "the selected instance's name is still light sky blue");
}
