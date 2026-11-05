// The Attribute Editor frames primvars sharing a first namespace, titled with it,
// and their labels leave it out. Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>

#include <QApplication>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <memory>

using usdcc::usd::AttributesViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::properties::PropertyRow;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

// The frame titled `title`, or null.
QFrame* frameTitled(AttributesViewPanel& panel, const QString& title) {
    for (QLabel* label : panel.findChildren<QLabel*>(QStringLiteral("propertyFrameTitle"))) {
        if (label->text() == title) return qobject_cast<QFrame*>(label->parentWidget());
    }
    return nullptr;
}

PropertyRow* rowLabelled(QWidget& parent, const QString& label) {
    for (PropertyRow* row : parent.findChildren<PropertyRow*>()) {
        if (row->labelText() == label) return row;
    }
    return nullptr;
}
}  // namespace

USDCC_TEST(attributes_primvar_groups) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdPrim prim = stage->usdStage()->DefinePrim(SdfPath("/Mesh"), TfToken("Mesh"));
    UsdGeomPrimvarsAPI api(prim);
    for (const char* name : {"skel:jointIndices", "skel:jointWeights", "arnold:subdiv", "arnold:smoothing", "lone:one", "plain"}) {
        api.CreatePrimvar(TfToken(name), SdfValueTypeNames->Float).Set(1.0f);
    }
    // As a mesh's UV sets are: the primvar, its indices and a name.
    for (const char* uvSet : {"st", "st1"}) {
        UsdGeomPrimvar primvar = api.CreatePrimvar(TfToken(uvSet), SdfValueTypeNames->TexCoord2fArray);
        primvar.Set(VtVec2fArray{GfVec2f(0, 0)});
        primvar.SetIndices(VtIntArray{0});
        prim.CreateAttribute(TfToken(std::string("primvars:") + uvSet + ":name"), SdfValueTypeNames->String).Set(std::string("uv"));
    }

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    stages.setSelectedPaths(stage, {SdfPath("/Mesh")});
    settle();

    QFrame* skel = frameTitled(panel, QStringLiteral("skel"));
    QFrame* arnold = frameTitled(panel, QStringLiteral("arnold"));
    CHECK(skel && arnold, "a frame per namespace, titled with it as it's written");
    if (!skel || !arnold) return;
    CHECK(skel->objectName() == QStringLiteral("compositionArc"), "the frame Composition uses");
    CHECK(!frameTitled(panel, QStringLiteral("lone")), "one on its own has no frame");
    CHECK(!frameTitled(panel, QStringLiteral("plain")), "nor does a primvar with nothing named after it");

    QFrame* st = frameTitled(panel, QStringLiteral("st"));
    QFrame* st1 = frameTitled(panel, QStringLiteral("st1"));
    CHECK(st && st1 && st != st1, "a UV set and what's named after it get a frame each, st1 apart from st");
    if (!st || !st1) return;
    CHECK(st->findChildren<PropertyRow*>().size() == 3, "the primvar, its indices and its name");
    CHECK(rowLabelled(*st, QStringLiteral("Value")) && rowLabelled(*st, QStringLiteral("Indices")) && rowLabelled(*st, QStringLiteral("Name")),
          "labelled without the title");
    CHECK(st->findChildren<PropertyRow*>().first()->labelText() == QStringLiteral("Value"), "the primvar itself first");

    CHECK(skel->findChildren<PropertyRow*>().size() == 2, "both primvars in it");
    CHECK(rowLabelled(*skel, QStringLiteral("Joint Indices")) && rowLabelled(*skel, QStringLiteral("Joint Weights")),
          "their labels leave the namespace out");
    CHECK(rowLabelled(*arnold, QStringLiteral("Subdiv")), "and in the other frame");
    CHECK(rowLabelled(panel, QStringLiteral("Plain")) && !rowLabelled(*skel, QStringLiteral("Plain")), "the others stay outside");
    bool loneOutside = false;
    for (PropertyRow* row : panel.findChildren<PropertyRow*>()) {
        loneOutside = loneOutside || (row->labelText().contains(QStringLiteral("One")) && !skel->isAncestorOf(row) && !arnold->isAncestorOf(row));
    }
    CHECK(loneOutside, "one on its own keeps a plain row");

    // Still editable as the same attribute, whatever its label says.
    PropertyRow* indices = rowLabelled(*skel, QStringLiteral("Joint Indices"));
    CHECK(indices, "the row");
    if (!indices) return;
    Q_EMIT indices->editor()->valueEdited(VtValue(5.0f));
    settle();
    float value = 0.0f;
    prim.GetAttribute(TfToken("primvars:skel:jointIndices")).Get(&value);
    CHECK(value == 5.0f, "editing it edits the primvar, whose name still has the namespace: %f", value);

    // Search: by the namespace, which the labels no longer say, and hiding what doesn't match.
    QLineEdit* search = nullptr;
    for (QLineEdit* edit : panel.findChildren<QLineEdit*>()) {
        if (edit->placeholderText() == QStringLiteral("Search attributes")) {
            search = edit;
            break;
        }
    }
    CHECK(search, "the search field");
    if (!search) return;
    panel.show();
    settle();
    search->setText(QStringLiteral("skel"));
    settle();
    CHECK(skel->isVisible() && !arnold->isVisible(), "searching a namespace finds its frame, and hides an empty one");
    CHECK(rowLabelled(*skel, QStringLiteral("Joint Indices"))->isVisible(), "with its rows");
    search->setText(QStringLiteral("subdiv"));
    settle();
    CHECK(!skel->isVisible() && arnold->isVisible(), "a frame shows for one matching row");
    CHECK(!rowLabelled(*arnold, QStringLiteral("Smoothing"))->isVisible(), "and hides the row that doesn't match");
    search->clear();
    settle();
    CHECK(skel->isVisible() && arnold->isVisible(), "clearing brings them back");
}
