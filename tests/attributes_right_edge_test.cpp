// Everything in the Attribute Editor ends the same distance from its right edge:
// section headers' buttons, rows' fields, buttons and notes, and frames. Runs in
// usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/properties/category_section.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>

#include <cstdio>
#include <map>
#include <memory>
#include <set>

using usdcc::usd::AttributesViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::properties::CategorySection;
using usdcc::usd::properties::PropertyRow;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    for (int i = 0; i < 8; ++i) QApplication::processEvents();
}

bool insideFrame(const QWidget* widget, const QWidget* stop) {
    for (const QWidget* w = widget->parentWidget(); w && w != stop; w = w->parentWidget()) {
        if (w->objectName() == QStringLiteral("compositionArc")) return true;
    }
    return false;
}

// How far the rightmost visible thing under `widget` ends from the right of `content`.
int gapOf(const QWidget* widget, const QWidget* content) {
    int right = -1;
    for (const QWidget* child : widget->findChildren<QWidget*>()) {
        if (!child->isVisible() || child->width() <= 0) continue;
        right = std::max(right, child->mapTo(content, QPoint(child->width(), 0)).x());
    }
    return right < 0 ? -1 : content->width() - right;
}
}  // namespace

USDCC_TEST(attributes_one_right_edge) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    const UsdPrim mesh = s->DefinePrim(SdfPath("/Mesh"), TfToken("Mesh"));
    s->DefinePrim(SdfPath("/Other"), TfToken("Xform"));
    bool resets = false;
    UsdGeomXformable(mesh).AddTranslateOp().Set(GfVec3d(1, 2, 3));
    (void)resets;
    mesh.GetReferences().AddInternalReference(SdfPath("/Other"));
    mesh.CreateRelationship(TfToken("custom:target")).AddTarget(SdfPath("/Other"));
    mesh.CreateAttribute(TfToken("userProperties:label"), SdfValueTypeNames->String).Set(std::string("text"));
    UsdGeomPrimvarsAPI api(mesh);
    api.CreatePrimvar(TfToken("skel:a"), SdfValueTypeNames->Float).Set(1.0f);
    api.CreatePrimvar(TfToken("skel:b"), SdfValueTypeNames->FloatArray);
    api.CreatePrimvar(TfToken("plain"), SdfValueTypeNames->Float, UsdGeomTokens->constant).Set(1.0f);

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    // These tests run without the app's theme; this is the width it gives the bar.
    panel.setStyleSheet(QStringLiteral("QScrollBar:vertical { width: 10px; }"));
    panel.resize(700, 900);
    panel.show();
    stages.setSelectedPaths(stage, {SdfPath("/Mesh")});
    settle();

    QWidget* content = panel.findChild<QWidget*>(QStringLiteral("attributeContent"));
    CHECK(content, "the content");
    if (!content) return;

    std::map<int, QStringList> outside;
    std::set<int> inside;
    for (PropertyRow* row : content->findChildren<PropertyRow*>()) {
        if (!row->isVisible()) continue;
        // A checkbox is as wide as itself: it isn't meant to reach the edge.
        const QList<QWidget*> children = row->findChildren<QWidget*>();
        const int gap = gapOf(row, content);
        bool onlyCheck = false;
        for (const QWidget* child : children) {
            if (child->isVisible() && qobject_cast<const QCheckBox*>(child)) onlyCheck = true;
        }
        if (onlyCheck && gap > 40) continue;
        if (insideFrame(row, content)) {
            inside.insert(gap);
        } else {
            outside[gap] << row->labelText();
        }
    }
    for (CategorySection* section : content->findChildren<CategorySection*>()) {
        if (!section->isVisible()) continue;
        for (QAbstractButton* button : section->findChildren<QAbstractButton*>()) {
            const bool inHeader = button->parentWidget() && button->parentWidget()->parentWidget() == section;
            if (!inHeader || !button->isVisible() || button->objectName() == QStringLiteral("sectionHeader")) continue;
            if (qobject_cast<QCheckBox*>(button)) continue;
            outside[gapOf(button->parentWidget(), content)] << QStringLiteral("header of a section");
        }
    }
    for (const auto& [gap, labels] : outside) {
        std::printf("right gap %d: %s\n", gap, qPrintable(labels.join(QStringLiteral(", ")).left(160)));
    }
    for (int gap : inside) std::printf("right gap inside a frame: %d\n", gap);

    CHECK(outside.size() == 1, "one right edge outside frames: %d different", int(outside.size()));
    const int edge = outside.empty() ? -1 : outside.begin()->first;
    for (QFrame* frame : content->findChildren<QFrame*>(QStringLiteral("compositionArc"))) {
        if (!frame->isVisible()) continue;
        const int gap = content->width() - frame->mapTo(content, QPoint(frame->width(), 0)).x();
        CHECK(gap == edge, "a frame's border is on that edge: %d, not %d", gap, edge);
    }
    CHECK(inside.size() <= 1, "and what's in the frames shares an edge too: %d different", int(inside.size()));

    // The search field above, measured from the panel's edge like a row.
    const auto gapFromPanel = [&panel](const QWidget* widget) {
        return panel.width() - widget->mapTo(&panel, QPoint(widget->width(), 0)).x();
    };
    QLineEdit* search = nullptr;
    for (QLineEdit* edit : panel.findChildren<QLineEdit*>()) {
        if (edit->placeholderText() == QStringLiteral("Search attributes")) search = edit;
    }
    QWidget* firstRow = nullptr;
    for (PropertyRow* row : content->findChildren<PropertyRow*>()) {
        if (row->isVisible() && !insideFrame(row, content) && gapOf(row, content) == edge) {
            firstRow = row;
            break;
        }
    }
    CHECK(search && firstRow, "the search field and a row");
    if (!search || !firstRow) return;
    const int rowGap = gapFromPanel(content) + edge;
    std::printf("from the panel's edge: search %d, rows %d\n", gapFromPanel(search), rowGap);
    CHECK(gapFromPanel(search) == rowGap, "the search field ends where the rows do: %d, not %d", gapFromPanel(search), rowGap);

    // Scrolling or not, nothing moves: the scroll bar's room is always kept.
    panel.resize(700, 400);
    settle();
    const int gapScrolling = gapFromPanel(content) + edge;
    panel.resize(700, 6000);
    settle();
    const int gapNotScrolling = gapFromPanel(content) + edge;
    CHECK(gapScrolling == gapNotScrolling, "the rows stay put when the scroll bar goes: %d, then %d", gapScrolling, gapNotScrolling);
    CHECK(gapFromPanel(search) == gapNotScrolling, "and the search field with them");
}
