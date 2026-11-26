// The Primvars panel (docs/PRIMVAR_VIEWER.md §8): its list across a selection, the shared
// primvar focus, the views and their registry, a view in a panel of its own, and the Attribute
// Editor's problem notes. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/primvar_colors.h"
#include "usdcc/usd/primvar_values_view.h"
#include "usdcc/usd/viewport_hud.h"
#include "usdcc/usd/primvar_views.h"
#include "usdcc/usd/primvars_view_panel.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/property_sheet.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/viewport_view_panel.h"
#include "usdcc/tools/tool_set.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QApplication>
#include <QClipboard>
#include <QItemSelectionModel>
#include <QTableView>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QJsonArray>
#include <QLocale>
#include <QMenu>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>

#include <cstdio>
#include <functional>
#include <memory>
#include <set>
#include <string>

using namespace usdcc::usd;
using usdcc::usd::properties::PropertyRow;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
bool waitFor(const std::function<bool()>& done, int ms = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > ms) return false;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

void settle() {
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

QTreeWidgetItem* rowNamed(QTreeWidget* list, const QString& name) {
    for (QTreeWidgetItemIterator it(list); *it; ++it) {
        if ((*it)->text(0) == name) return *it;
    }
    return nullptr;
}

QStringList topLevel(QTreeWidget* list) {
    QStringList names;
    for (int i = 0; i < list->topLevelItemCount(); ++i) names << list->topLevelItem(i)->text(0);
    return names;
}

QStringList tabNames(QTabWidget* tabs) {
    QStringList names;
    for (int i = 0; i < tabs->count(); ++i) names << tabs->tabText(i);
    return names;
}

QString summaryOf(QWidget& panel) {
    const auto* label = panel.findChild<QLabel*>(QStringLiteral("primvarsSummary"));
    return label ? label->text() : QString();
}

struct Fixture {
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale{*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini"))};
    StageManager stages;
    std::shared_ptr<UndoManager> undo = std::make_shared<UndoManager>();
    StageRefPtr stage;
    Fixture() {
        QStandardPaths::setTestModeEnabled(true);
        stage = stages.openStage(std::string(USDCC_TEST_DATA_DIR) + "/broken_primvars.usda");
        // Edits stay in the session layer, so the file and other tests are untouched.
        if (stage) stage->usdStage()->SetEditTarget(stage->usdStage()->GetSessionLayer());
    }
    ~Fixture() {
        if (stage) stage->usdStage()->GetSessionLayer()->Clear();
        if (stage) stages.closeStage(stage);
    }
    void select(std::vector<SdfPath> paths) {
        stages.setSelectedPaths(stage, std::move(paths));
        settle();
    }
};

// Shows what it's told, for the registry's tests.
class CountingView : public PrimvarView {
public:
    using PrimvarView::PrimvarView;
    void setContext(const PrimvarViewContext& context, bool) override {
        ++calls;
        primvar = context.primvar;
    }
    int calls = 0;
    std::string primvar;
};
}  // namespace

USDCC_TEST(primvars_panel_list) {
    Fixture fixture;
    CHECK(fixture.stage, "the stage opens");
    if (!fixture.stage) return;
    PrimvarsViewPanel panel(&fixture.stages, fixture.stage, fixture.undo);
    panel.setShowInherited(true);
    QTreeWidget* list = panel.list();
    CHECK(list->topLevelItemCount() == 0 && summaryOf(panel) == "Select a prim", "nothing selected: %s", qPrintable(summaryOf(panel)));

    fixture.select({SdfPath("/World/Good")});
    CHECK((topLevel(list) == QStringList{"dirt", "displayColor", "id", "names", "st", "wetness"}), "a row each: %s",
          qPrintable(topLevel(list).join(',')));
    CHECK(summaryOf(panel) == "6 primvars on Good", "the summary: %s", qPrintable(summaryOf(panel)));
    QTreeWidgetItem* st = rowNamed(list, "st");
    CHECK(st && st->text(1) == "texCoord2f[]" && st->text(2) == "faceVarying" && st->text(3) == "8 of 8, indexed into 4" &&
              st->text(4) == "here" && st->icon(0).isNull(),
          "a row's columns: %s | %s", st ? qPrintable(st->text(3)) : "", st ? qPrintable(st->text(4)) : "");
    QTreeWidgetItem* wetness = rowNamed(list, "wetness");
    CHECK(wetness && wetness->text(4) == "from World" && wetness->font(0).italic(), "an inherited one");
    panel.setShowInherited(false);
    CHECK(!rowNamed(list, "wetness") && rowNamed(list, "st"), "inherited ones can be left out");
    panel.setShowInherited(true);

    fixture.select({SdfPath("/World/ElementCount")});
    QTreeWidgetItem* dirt = rowNamed(list, "dirt");
    CHECK(dirt && !dirt->icon(0).isNull() && dirt->toolTip(0).contains("vertex needs 6 values") && dirt->text(3) == "8 of 6",
          "a problem shows on its row");
    CHECK(summaryOf(panel) == "2 primvars on ElementCount, 1 error", "and in the summary: %s", qPrintable(summaryOf(panel)));

    fixture.select({SdfPath("/World/SkinningValues")});
    QTreeWidgetItem* skinning = rowNamed(list, "Skinning");
    CHECK(skinning && skinning->childCount() == 2 && skinning->child(0)->text(0) == "skel:jointIndices" && !skinning->icon(0).isNull(),
          "a group's row, with its members under it");
    CHECK(rowNamed(list, "skel:jointWeights") && rowNamed(list, "skel:jointWeights")->toolTip(0).isEmpty() &&
              skinning->toolTip(0).contains("add up to 1"),
          "the group's problems are the group's");

    fixture.select({SdfPath("/World/Good"), SdfPath("/World/ElementCount")});
    dirt = rowNamed(list, "dirt");
    CHECK(dirt && dirt->text(3).endsWith("(on 2 of 2)") && dirt->toolTip(0).startsWith("ElementCount: "),
          "several prims: %s / %s", dirt ? qPrintable(dirt->text(3)) : "", dirt ? qPrintable(dirt->toolTip(0)) : "");
    CHECK(rowNamed(list, "st") && rowNamed(list, "st")->text(3).endsWith("(on 1 of 2)") && summaryOf(panel).startsWith("6 primvars on 2 prims"),
          "the union: %s", qPrintable(summaryOf(panel)));

    // An edit is read again, a moment later.
    fixture.select({SdfPath("/World/ElementCount")});
    UsdGeomPrimvarsAPI(fixture.stage->usdStage()->GetPrimAtPath(SdfPath("/World/ElementCount")))
        .GetPrimvar(TfToken("dirt"))
        .Set(VtFloatArray(6, 1.0f));
    CHECK(waitFor([&]() { return rowNamed(list, "dirt") && rowNamed(list, "dirt")->icon(0).isNull(); }), "follows edits");
}

USDCC_TEST(primvars_panel_missing) {
    Fixture fixture;
    if (!fixture.stage) return;
    const StageRefPtr stage = fixture.stages.createStage();
    stage->usdStage()->GetRootLayer()->ImportFromString(R"(#usda 1.0
def Mesh "Mesh" ( prepend apiSchemas = ["MaterialBindingAPI"] ) {
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    rel material:binding = </Material>
}
def Material "Material" {
    token outputs:surface.connect = </Material/Surface.outputs:surface>
    def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        float inputs:roughness.connect = </Material/Dirt.outputs:result>
        token outputs:surface
    }
    def Shader "Dirt" {
        uniform token info:id = "UsdPrimvarReader_float"
        string inputs:varname = "dirt"
        float outputs:result
    }
}
)");
    PrimvarsViewPanel panel(&fixture.stages, stage, fixture.undo);
    fixture.stages.setSelectedPaths(stage, {SdfPath("/Mesh")});
    settle();
    QTreeWidgetItem* dirt = rowNamed(panel.list(), "dirt");
    CHECK(dirt && dirt->text(3) == "missing" && dirt->text(5) == "Material" && !dirt->icon(0).isNull(), "what the material reads and isn't there");
    CHECK(summaryOf(panel) == "0 primvars on Mesh, 1 missing", "%s", qPrintable(summaryOf(panel)));
    stage->setPrimvarFocus("dirt");
    settle();
    auto* info = qobject_cast<PrimvarInfoView*>(panel.view("info"));
    CHECK(info && info->text().contains("Read by /Material, but none of the selected prims has it"), "Info says so: %s",
          info ? qPrintable(info->text()) : "");
    fixture.stages.closeStage(stage);
}

USDCC_TEST(primvars_panel_focus_and_info) {
    Fixture fixture;
    if (!fixture.stage) return;
    PrimvarsViewPanel panel(&fixture.stages, fixture.stage, fixture.undo);
    PrimvarsViewPanel other(&fixture.stages, fixture.stage, fixture.undo);
    fixture.stage->setPrimvarFocus({});
    fixture.select({SdfPath("/World/Good")});
    auto* info = qobject_cast<PrimvarInfoView*>(panel.view("info"));
    CHECK(tabNames(panel.tabs()).value(0) == "Info" && info, "Info is the first view");
    if (!info) return;
    CHECK(info->text().contains("Pick a primvar"), "nothing in focus");

    panel.list()->setCurrentItem(rowNamed(panel.list(), "dirt"));
    CHECK(fixture.stage->primvarFocus() == "dirt", "picking a row sets the stage's focus");
    CHECK(other.list()->currentItem() && other.list()->currentItem()->text(0) == "dirt", "which every panel on the stage follows");
    CHECK(info->text().contains("6 of 6 (points)") && info->text().contains("Authored here"), "Info: %s", qPrintable(info->text()));
    CHECK(waitFor([&]() { return !info->isComputing(); }) && info->text().contains("Mean") &&
              info->text().contains(QLocale().toString(2.5, 'g', 5)),
          "and the statistics, worked out on a worker: %s", qPrintable(info->text()));

    fixture.stage->setPrimvarFocus("st");
    settle();
    CHECK(panel.list()->currentItem() && panel.list()->currentItem()->text(0) == "st", "focus set elsewhere picks the row");
    CHECK(waitFor([&]() { return !info->isComputing(); }) && info->text().contains("Elements") && info->text().contains("(0, 0)"),
          "statistics of an indexed one, over its elements: %s", qPrintable(info->text()));

    fixture.select({SdfPath("/World/IndexRange")});
    CHECK(waitFor([&]() { return !info->isComputing(); }) && info->text().contains("Of the values, as some indices are outside them") &&
              info->text().contains("outside its 2 values"),
          "indices outside: the values' statistics, and the problem: %s", qPrintable(info->text()));

    fixture.select({SdfPath("/World/Good")});
    fixture.stage->setPrimvarFocus("wetness");
    settle();
    CHECK(info->text().contains("Inherited from /World"), "an inherited one");

    // Group rows focus their first member.
    fixture.select({SdfPath("/World/SkinningValues")});
    panel.list()->setCurrentItem(rowNamed(panel.list(), "Skinning"));
    CHECK(fixture.stage->primvarFocus() == "skel:jointIndices", "a group's row");
    CHECK(info->text().contains("Skinning") && info->text().contains("outside the 2 joints"), "Info has its group: %s", qPrintable(info->text()));
}

USDCC_TEST(primvars_panel_views) {
    Fixture fixture;
    if (!fixture.stage) return;
    QString opened;
    PrimvarsViewPanel::setHooks({[&opened](const QString& id, const StageRefPtr&) { opened = id; }});
    PrimvarsViewPanel panel(&fixture.stages, fixture.stage, fixture.undo);
    fixture.select({SdfPath("/World/Good")});

    CountingView* counting = nullptr;
    PrimvarViews::instance().add({QStringLiteral("test.uv"), QStringLiteral("UV"),
                                  [&counting](QWidget* parent) { return counting = new CountingView(parent); },
                                  [](const PrimvarViewContext& context) {
                                      const PrimvarInfo* info = context.info();
                                      return info && info->type.GetDimensions().size == 1 && info->type.GetDimensions().d[0] == 2;
                                  }});
    fixture.stage->setPrimvarFocus("dirt");
    settle();
    CHECK((tabNames(panel.tabs()) == QStringList{"Info", "Values"}), "a view shows where it applies only");
    fixture.stage->setPrimvarFocus("st");
    settle();
    CHECK((tabNames(panel.tabs()) == QStringList{"Info", "Values", "UV"}) && counting && counting->primvar == "st", "a plugin's view");
    panel.tabs()->setCurrentIndex(2);
    auto* open = panel.findChild<QToolButton*>(QStringLiteral("openPrimvarView"));
    CHECK(open && open->isVisibleTo(&panel), "Open in Own Panel");
    if (open) open->click();
    CHECK(opened == "test.uv", "asks the app to open the view: %s", qPrintable(opened));
    const QJsonObject state = panel.saveSessionState();
    CHECK(state.value("tab").toString() == "test.uv", "a session keeps the tab");
    panel.tabs()->setCurrentIndex(0);
    panel.restoreSessionState(state);
    CHECK(panel.tabs()->currentIndex() == 2, "and gets it back");

    CHECK(PrimvarViews::instance().remove("test.uv") && !PrimvarViews::instance().remove("test.uv"), "removed");
    settle();
    CHECK((tabNames(panel.tabs()) == QStringList{"Info", "Values"}) && !panel.view("test.uv"), "its tab goes");
    PrimvarsViewPanel::setHooks({});
}

USDCC_TEST(primvars_view_in_own_panel) {
    Fixture fixture;
    if (!fixture.stage) return;
    fixture.stage->setPrimvarFocus({});
    PrimvarViewPanel panel(&fixture.stages, fixture.stage, "info", "Primvar Info");
    fixture.select({SdfPath("/World/Good")});
    QComboBox* choice = panel.primvarChoice();
    CHECK(choice->count() == 7 && choice->itemText(0) == "(none)" && choice->currentIndex() == 0, "a choice of the primvars (%d)",
          choice->count());
    choice->setCurrentIndex(choice->findText("names"));
    CHECK(fixture.stage->primvarFocus() == "names", "picking one sets the focus");
    auto* info = qobject_cast<PrimvarInfoView*>(panel.view());
    CHECK(info && waitFor([&]() { return !info->isComputing(); }) && info->text().contains("Distinct"), "the view shows it: %s",
          info ? qPrintable(info->text()) : "");
    fixture.stage->setPrimvarFocus("dirt");
    settle();
    CHECK(choice->currentText() == "dirt", "and follows it");

    PrimvarViewPanel gone(&fixture.stages, fixture.stage, "no.such.view", "Primvar Gone");
    CHECK(!gone.view(), "a view whose plugin isn't running");
}

USDCC_TEST(attributes_primvar_problem_notes) {
    Fixture fixture;
    if (!fixture.stage) return;
    StageRefPtr shown;
    std::string focused = "unset";
    PropertySheet::setShowPrimvars([&](const StageRefPtr& stage, const std::string& primvar) {
        shown = stage;
        focused = primvar;
    });
    AttributesViewPanel panel(&fixture.stages, fixture.stage, fixture.undo);
    fixture.select({SdfPath("/World/ElementCount")});
    PropertyRow* dirt = nullptr;
    for (PropertyRow* row : panel.findChildren<PropertyRow*>()) {
        if (row->propertyPath().endsWith("primvars:dirt")) dirt = row;
    }
    const auto noteLabel = [&dirt]() { return dirt ? dirt->findChild<QLabel*>(QStringLiteral("propertyNote")) : nullptr; };
    CHECK(dirt && dirt->note() == QString::fromUtf8("\xe2\x9a\xa0 vertex") && noteLabel()->property("severity").toString() == "error" &&
              noteLabel()->toolTip().contains("vertex needs 6 values"),
          "a primvar's problem in its note: %s", dirt ? qPrintable(dirt->note()) : "");

    UsdGeomPrimvarsAPI(fixture.stage->usdStage()->GetPrimAtPath(SdfPath("/World/ElementCount")))
        .GetPrimvar(TfToken("dirt"))
        .Set(VtFloatArray(6, 1.0f));
    CHECK(waitFor([&]() {
              for (PropertyRow* row : panel.findChildren<PropertyRow*>()) {
                  if (row->propertyPath().endsWith("primvars:dirt")) dirt = row;
              }
              return dirt && dirt->note() == "vertex";
          }),
          "gone once it's fixed");
    CHECK(noteLabel() && noteLabel()->property("severity").toString().isEmpty(), "and its colour");

    auto* show = panel.findChild<QToolButton*>(QStringLiteral("showPrimvarsButton"));
    CHECK(show, "the Primvars section links to the panel");
    if (show) show->click();
    CHECK(shown == fixture.stage && focused.empty(), "for the stage");
    PropertySheet::setShowPrimvars({});
}

USDCC_TEST(primvars_values_view) {
    Fixture fixture;
    if (!fixture.stage) return;
    PrimvarsViewPanel panel(&fixture.stages, fixture.stage, fixture.undo);
    fixture.select({SdfPath("/World/Good")});
    fixture.stage->setPrimvarFocus("dirt");
    settle();
    CHECK((tabNames(panel.tabs()) == QStringList{"Info", "Values"}), "Values is the second view: %s", qPrintable(tabNames(panel.tabs()).join(',')));
    auto* view = qobject_cast<PrimvarValuesView*>(panel.view("values"));
    if (!view) return;
    PrimvarValuesModel* model = view->model();
    const auto headers = [model]() {
        QStringList names;
        for (int i = 0; i < model->columnCount(); ++i) names << model->headerData(i, Qt::Horizontal, Qt::DisplayRole).toString();
        return names;
    };
    const auto at = [model](int row, int column) { return model->data(model->index(row, column), Qt::DisplayRole).toString(); };
    const auto status = [view]() { return view->findChild<QLabel*>(QStringLiteral("primvarValuesStatus"))->text(); };
    CHECK(model->rowCount() == 6 && (headers() == QStringList{"dirt", "points"}) && at(3, 0) == "3.0" && at(5, 1) == "(2.0, 1.0, 0.0)",
          "a point each, with where it is: %s | %s", qPrintable(headers().join(',')), qPrintable(at(5, 1)));
    CHECK(status() == "6 points, vertex on /World/Good", "what the rows are: %s", qPrintable(status()));

    fixture.stage->setPrimvarFocus("st");
    settle();
    CHECK(model->rowCount() == 8 && (headers() == QStringList{"st", "face", "point"}), "face corners, with their face and point: %s",
          qPrintable(headers().join(',')));
    CHECK(at(0, 0) == QString::fromUtf8("(0.0, 0.0)  \xe2\x9f\xa8" "0\xe2\x9f\xa9") && at(5, 0).endsWith(QString::fromUtf8("\xe2\x9f\xa8" "1\xe2\x9f\xa9")) &&
              at(4, 1) == "1" && at(4, 2) == "1" && at(7, 2) == "4",
          "an indexed one's value and index: %s; face %s, point %s", qPrintable(at(0, 0)), qPrintable(at(4, 1)), qPrintable(at(4, 2)));

    fixture.stage->setPrimvarFocus("names");
    settle();
    CHECK(model->rowCount() == 1 && headers().front() == "names" && headers().contains("displayColor") && headers().contains("wetness") &&
              at(0, 0) == "[3] a, b, c",
          "constant: one row, the whole of each, inherited ones too: %s / %s", qPrintable(headers().join(',')), qPrintable(at(0, 0)));

    fixture.stage->setPrimvarFocus("id");
    settle();
    CHECK(model->rowCount() == 2 && (headers() == QStringList{"id"}) && at(1, 0) == "2", "uniform: a face each");

    // Copy: the selected cells, tab-separated, under their columns' names.
    fixture.stage->setPrimvarFocus("dirt");
    settle();
    view->table()->selectionModel()->select(QItemSelection(model->index(1, 0), model->index(2, 1)), QItemSelectionModel::ClearAndSelect);
    CHECK(view->selectedText() == "dirt\tpoints\n1.0\t(1.0, 0.0, 0.0)\n2.0\t(2.0, 0.0, 0.0)", "copied: %s", qPrintable(view->selectedText()));
    view->copy();
    CHECK(QApplication::clipboard()->text() == view->selectedText(), "to the clipboard");

    fixture.select({SdfPath("/World/IndexRange")});
    fixture.stage->setPrimvarFocus("st");
    settle();
    const QVariant red = model->data(model->index(2, 0), Qt::ForegroundRole);
    CHECK(at(2, 0).startsWith("outside the values") && red.isValid() && at(0, 0).startsWith("(0.0, 0.0)"), "an index outside, marked: %s",
          qPrintable(at(2, 0)));
    fixture.select({SdfPath("/World/ElementCount")});
    fixture.stage->setPrimvarFocus("dirt");
    settle();
    CHECK(model->rowCount() == 6 && at(5, 0) == "5.0", "as many rows as there are points, though it has more values");
    fixture.select({SdfPath("/World/ElementSize")});
    fixture.stage->setPrimvarFocus("sh");
    settle();
    CHECK(at(0, 0) == "[0.0, 1.0, 2.0]" && at(1, 0) == "none" && model->data(model->index(1, 0), Qt::ForegroundRole).isValid(),
          "three numbers an element, and a face without its values: %s", qPrintable(at(1, 0)));

    fixture.stage->setPrimvarFocus({});
    settle();
    CHECK(tabNames(panel.tabs()) == QStringList{"Info"}, "no tab without a primvar with values");
}

USDCC_TEST(primvars_values_million_points) {
    Fixture fixture;
    if (!fixture.stage) return;
    // A grid of 1000 x 1000 points, 999 x 999 quads, with a value for each point.
    constexpr int side = 1000;
    const StageRefPtr stage = fixture.stages.createStage();
    const UsdGeomMesh mesh = UsdGeomMesh::Define(stage->usdStage(), SdfPath("/Grid"));
    VtVec3fArray points(side * side);
    VtFloatArray heat(side * side);
    for (int i = 0; i < side * side; ++i) {
        points[i] = GfVec3f(float(i % side), float(i / side), 0);
        heat[i] = float(i % 97);
    }
    VtIntArray counts((side - 1) * (side - 1), 4), indices;
    indices.reserve(counts.size() * 4);
    for (int y = 0; y + 1 < side; ++y) {
        for (int x = 0; x + 1 < side; ++x) {
            const int a = y * side + x;
            for (const int corner : {a, a + 1, a + side + 1, a + side}) indices.push_back(corner);
        }
    }
    mesh.GetPointsAttr().Set(points);
    mesh.GetFaceVertexCountsAttr().Set(counts);
    mesh.GetFaceVertexIndicesAttr().Set(indices);
    UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("heat"), SdfValueTypeNames->FloatArray, UsdGeomTokens->vertex).Set(heat);
    stage->setPrimvarFocus("heat");

    PrimvarValuesView view;
    view.resize(600, 400);
    view.show();
    QElapsedTimer timer;
    timer.start();
    fixture.stages.setSelectedPaths(stage, {SdfPath("/Grid")});
    PrimvarContextTracker tracker(&fixture.stages);
    tracker.setStage(stage);
    const qint64 read = timer.restart();
    view.setContext(tracker.context(), false);
    settle();
    const qint64 shown = timer.elapsed();
    PrimvarValuesModel* model = view.model();
    CHECK(model->rowCount() == side * side, "a million rows (%d)", model->rowCount());
    CHECK(model->data(model->index(side * side - 1, 0), Qt::DisplayRole).toString() == QString::number((side * side - 1) % 97) + ".0",
          "the last one");
    view.table()->scrollToBottom();
    settle();
    std::printf("  a million points: read in %lld ms, shown in %lld ms\n", read, shown);
    CHECK(shown < 2000, "shown without formatting them all (%lld ms)", shown);
    fixture.stages.closeStage(stage);
}

USDCC_TEST(primvars_values_element_selection) {
    Fixture fixture;
    if (!fixture.stage) return;
    PrimvarsViewPanel panel(&fixture.stages, fixture.stage, fixture.undo);
    fixture.select({SdfPath("/World/Good")});
    fixture.stage->setPrimvarFocus("dirt");
    settle();
    auto* view = qobject_cast<PrimvarValuesView*>(panel.view("values"));
    if (!view) return;
    PrimvarValuesModel* model = view->model();
    QItemSelectionModel* rows = view->table()->selectionModel();
    rows->select(QItemSelection(model->index(1, 0), model->index(2, 1)), QItemSelectionModel::ClearAndSelect);
    const PrimvarElements& picked = fixture.stage->primvarElements();
    CHECK(picked.prim == SdfPath("/World/Good") && picked.domain == "points" && (picked.indices == std::vector<std::size_t>{1, 2}),
          "rows picked are the stage's elements: %s %zu", picked.domain.c_str(), picked.indices.size());

    fixture.stage->setPrimvarElements({SdfPath("/World/Good"), "points", {0, 4, 5}});
    settle();
    const auto selectedRows = [rows]() {
        std::set<int> found;
        for (const QModelIndex& index : rows->selectedIndexes()) found.insert(index.row());
        return found;
    };
    CHECK((selectedRows() == std::set<int>{0, 4, 5}), "and the stage's are picked here (%zu)", selectedRows().size());

    // Another primvar over the same points keeps them; another domain doesn't show them.
    fixture.stage->setPrimvarFocus("st");
    settle();
    CHECK(selectedRows().empty() && fixture.stage->primvarElements().indices.size() == 3, "face corners aren't points, and nothing's lost");
    fixture.stage->setPrimvarFocus("dirt");
    settle();
    CHECK((selectedRows() == std::set<int>{0, 4, 5}), "back on points");

    // The row under the cursor.
    Q_EMIT view->table()->entered(model->index(3, 0));
    CHECK((fixture.stage->primvarElementHover().indices == std::vector<std::size_t>{3}), "hover");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(view->table()->viewport(), &leave);
    CHECK(fixture.stage->primvarElementHover().empty(), "and leaving");

    fixture.select({SdfPath("/World/ElementCount")});
    CHECK(fixture.stage->primvarElements().empty() && selectedRows().empty(), "another prim drops them");
}

USDCC_TEST(viewport_overlays_menu) {
    Fixture fixture;
    if (!fixture.stage) return;
    auto tools = std::make_shared<usdcc::tools::ToolSet>();
    ViewportViewPanel viewport(&fixture.stages, fixture.stage, tools, fixture.undo);
    // populateOptionsMenu() is protected: reached through a subclass, as viewport_display_test does.
    struct Opener : ViewportViewPanel {
        static void populate(ViewportViewPanel* p, QMenu* m) { (p->*(&Opener::populateOptionsMenu))(m); }
    };
    QMenu menu;
    Opener::populate(&viewport, &menu);
    QAction* elements = menu.findChild<QAction*>(QStringLiteral("overlay:usdcc.primvarElements"));
    CHECK(elements && elements->isCheckable() && elements->isChecked(), "Overlays > Primvar Elements, on");
    if (!elements) return;
    elements->setChecked(false);
    CHECK(viewport.hydraWindow()->hiddenOverlays().count("usdcc.primvarElements"), "switched off for this viewport");
    const QJsonObject state = viewport.saveSessionState();
    CHECK(state.value("hiddenOverlays").toArray().size() == 1, "a session keeps it");
    std::unique_ptr<usdcc::ui::ViewPanel> copy(viewport.duplicate());
    auto* duplicate = qobject_cast<ViewportViewPanel*>(copy.get());
    CHECK(duplicate && duplicate->hydraWindow()->hiddenOverlays().count("usdcc.primvarElements"), "and Duplicate");
    elements->setChecked(true);
    viewport.restoreSessionState(state);
    CHECK(viewport.hydraWindow()->hiddenOverlays().count("usdcc.primvarElements"), "and gives it back");
}

USDCC_TEST(viewport_color_by) {
    Fixture fixture;
    if (!fixture.stage) return;
    auto tools = std::make_shared<usdcc::tools::ToolSet>();
    ViewportViewPanel viewport(&fixture.stages, fixture.stage, tools, fixture.undo);
    fixture.select({SdfPath("/World/Good")});
    struct Opener : ViewportViewPanel {
        static void populate(ViewportViewPanel* p, QMenu* m) { (p->*(&Opener::populateOptionsMenu))(m); }
    };
    QMenu menu;
    Opener::populate(&viewport, &menu);
    QAction* dirt = menu.findChild<QAction*>(QStringLiteral("colorBy:dirt"));
    CHECK(menu.findChild<QAction*>(QStringLiteral("colorBy:")) && dirt && menu.findChild<QAction*>(QStringLiteral("colorBy:st")),
          "Color By lists None and the selected prims' primvars");
    if (!dirt) return;
    dirt->trigger();
    HydraViewportWindow* window = viewport.hydraWindow();
    CHECK(viewport.primvarColoring().primvar == "dirt" && window->primvarColorStats().prims >= 2 &&
              window->primvarColorStats().range == (PrimvarColorRange{0, 7}),
          "colours by it, the stage's range: %zu", window->primvarColorStats().prims);
    std::optional<HudLegend> legend = window->hudInput().legend;
    CHECK(legend && legend->ramp && legend->title == "Color by dirt" && legend->low == QLocale().toString(0.0) &&
              legend->high == QLocale().toString(7.0),
          "the HUD's legend: %s", legend ? qPrintable(legend->low + " " + legend->high) : "none");

    // An edit that widens the range, a moment later.
    UsdGeomPrimvarsAPI(fixture.stage->usdStage()->GetPrimAtPath(SdfPath("/World/Good"))).GetPrimvar(TfToken("dirt")).Set(VtFloatArray{0, 1, 2, 3, 4, 50});
    CHECK(waitFor([&]() { return window->primvarColorStats().range && window->primvarColorStats().range->high == 50; }), "follows edits");

    QMenu again;
    Opener::populate(&viewport, &again);
    CHECK(again.findChild<QAction*>(QStringLiteral("colorizer:categories")) == nullptr && again.findChild<QAction*>(QStringLiteral("colorizer:ramp")),
          "the colours that apply to floats");
    QAction* lit = again.findChild<QAction*>(QStringLiteral("colorByLitAction"));
    if (lit) lit->trigger();
    CHECK(viewport.primvarColoring().lit, "Lit");

    PrimvarColoring ids;
    ids.primvar = "id";
    viewport.setPrimvarColoring(ids);
    legend = window->hudInput().legend;
    CHECK(legend && !legend->ramp && legend->categories.size() >= 2, "whole numbers: a colour each (%zu)", legend ? legend->categories.size() : 0);
    ids.primvar = "st";
    viewport.setPrimvarColoring(ids);
    legend = window->hudInput().legend;
    CHECK(legend && legend->note == "U red, V green", "a UV set says what its colours mean");

    PrimvarColoring fixed;
    fixed.primvar = "dirt";
    fixed.range = PrimvarColorRange{-1, 1};
    fixed.component = 0;
    viewport.setPrimvarColoring(fixed);
    const QJsonObject state = viewport.saveSessionState();
    CHECK(state.value("colorBy").toObject().value("primvar").toString() == "dirt" &&
              state.value("colorBy").toObject().value("range").toArray().size() == 2,
          "a session keeps it");
    std::unique_ptr<usdcc::ui::ViewPanel> copy(viewport.duplicate());
    CHECK(qobject_cast<ViewportViewPanel*>(copy.get())->primvarColoring() == fixed, "and Duplicate");
    viewport.setPrimvarColoring({});
    CHECK(!window->hudInput().legend, "off: no legend");
    viewport.restoreSessionState(state);
    CHECK(viewport.primvarColoring() == fixed, "and gives it back");
    viewport.setPrimvarColoring({});
}
