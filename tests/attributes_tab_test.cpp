// Tab in the Attribute Editor (docs/ATTRIBUTE_EDITOR.md §4.4): it commits the field
// and starts typing in the next one on the same line, else the first one on a line
// below; Shift+Tab goes back; it wraps round; check boxes and collapsed sections are
// skipped. Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/properties/category_section.h"
#include "usdcc/usd/properties/number_field.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using usdcc::usd::AttributesViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::properties::CategorySection;
using usdcc::usd::properties::NumberField;
using usdcc::usd::properties::PropertyRow;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    for (int i = 0; i < 8; ++i) QApplication::processEvents();
}

PropertyRow* rowOf(AttributesViewPanel& panel, const std::string& name) {
    for (PropertyRow* row : panel.findChildren<PropertyRow*>()) {
        if (row->propertyPath().endsWith(QString::fromStdString("." + name))) return row;
    }
    return nullptr;
}

// A row's number fields, left to right.
std::vector<NumberField*> numbersOf(PropertyRow* row) {
    std::vector<NumberField*> fields;
    if (!row) return fields;
    for (NumberField* field : row->findChildren<NumberField*>()) fields.push_back(field);
    std::sort(fields.begin(), fields.end(), [row](NumberField* a, NumberField* b) {
        return a->mapTo(row, QPoint()).x() < b->mapTo(row, QPoint()).x();
    });
    return fields;
}

QLineEdit* textOf(PropertyRow* row) { return row ? row->findChild<QLineEdit*>() : nullptr; }

// The attribute's value as a T (default-constructed when it has none).
template <typename T>
T valueOf(const UsdAttribute& attribute) {
    T value{};
    attribute.Get(&value);
    return value;
}

void press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWidget* focus = QApplication::focusWidget();
    if (!focus) return;
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(focus, &event);
    settle();
}

// Types into whatever has the focus, replacing what's selected.
void type(const QString& text) {
    if (auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget())) edit->insert(text);
}

// The number field being typed in, if any.
NumberField* typingIn() {
    QWidget* focus = QApplication::focusWidget();
    return focus ? qobject_cast<NumberField*>(focus->parentWidget()) : nullptr;
}
}  // namespace

USDCC_TEST(attributes_tab_moves_between_fields) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    const UsdPrim prim = s->DefinePrim(SdfPath("/Thing"), TfToken("Scope"));
    const UsdAttribute a = prim.CreateAttribute(TfToken("a"), SdfValueTypeNames->Float3, /*custom=*/true);
    a.Set(GfVec3f(0, 0, 0));
    const UsdAttribute b = prim.CreateAttribute(TfToken("b"), SdfValueTypeNames->String, true);
    b.Set(std::string("world"));
    const UsdAttribute c = prim.CreateAttribute(TfToken("c"), SdfValueTypeNames->Float, true);
    c.Set(1.0f);
    prim.CreateAttribute(TfToken("d"), SdfValueTypeNames->Bool, true).Set(true);
    const UsdAttribute e = prim.CreateAttribute(TfToken("e"), SdfValueTypeNames->Float, true);
    e.Set(2.0f);

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.resize(500, 900);
    panel.show();
    panel.activateWindow();
    stages.setSelectedPaths(stage, {SdfPath("/Thing")});
    settle();

    const std::vector<NumberField*> aFields = numbersOf(rowOf(panel, "a"));
    const std::vector<NumberField*> cFields = numbersOf(rowOf(panel, "c"));
    const std::vector<NumberField*> eFields = numbersOf(rowOf(panel, "e"));
    QLineEdit* bText = textOf(rowOf(panel, "b"));
    CHECK(aFields.size() == 3 && cFields.size() == 1 && eFields.size() == 1 && bText, "the rows: %d %d %d %d",
          static_cast<int>(aFields.size()), static_cast<int>(cFields.size()), static_cast<int>(eFields.size()), bText != nullptr);
    if (aFields.size() != 3 || cFields.size() != 1 || eFields.size() != 1 || !bText) return;

    aFields[0]->beginTextEdit();
    settle();
    CHECK(typingIn() == aFields[0], "typing in a's X");
    type(QStringLiteral("5"));
    press(Qt::Key_Tab);
    CHECK(valueOf<GfVec3f>(a) == GfVec3f(5, 0, 0), "Tab commits it");
    CHECK(typingIn() == aFields[1], "and moves on to Y, on the same line");
    CHECK(static_cast<QLineEdit*>(QApplication::focusWidget())->selectedText() == QStringLiteral("0"),
          "its value selected, so typing replaces it");
    type(QStringLiteral("6"));
    press(Qt::Key_Tab);
    CHECK(typingIn() == aFields[2], "then Z");
    press(Qt::Key_Tab);
    CHECK(valueOf<GfVec3f>(a) == GfVec3f(5, 6, 0), "each one committed: (%g %g %g)", valueOf<GfVec3f>(a)[0], valueOf<GfVec3f>(a)[1],
          valueOf<GfVec3f>(a)[2]);

    CHECK(QApplication::focusWidget() == bText, "after the line's last field: the first one on the line below");
    CHECK(bText->selectedText() == QStringLiteral("world"),
          "a text field selects its text, and the refresh after a's edit leaves it so: '%s'",
          bText->selectedText().toStdString().c_str());
    type(QStringLiteral("hello"));
    press(Qt::Key_Tab);
    CHECK(valueOf<std::string>(b) == "hello", "Tab commits a text field too");
    CHECK(typingIn() == cFields[0], "then c");
    press(Qt::Key_Tab);
    CHECK(typingIn() == eFields[0], "a check box (d) is skipped");
    press(Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(typingIn() == cFields[0], "Shift+Tab goes back");
    press(Qt::Key_Tab);
    press(Qt::Key_Tab);
    auto* first = qobject_cast<QLineEdit*>(QApplication::focusWidget());
    CHECK(first && first->text() == QStringLiteral("Thing"), "after the last field it wraps round to the first: the name");
    press(Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(typingIn() == eFields[0], "and Shift+Tab from there back to the last");

    // Enter and Esc end typing where it is: they don't move on as Tab does.
    cFields[0]->beginTextEdit();
    settle();
    type(QStringLiteral("7"));
    press(Qt::Key_Return);
    CHECK(valueOf<float>(c) == 7.0f, "Enter commits");
    // (The field passes Return on to itself, which starts typing again: as before.)
    const bool inC = QApplication::focusWidget() == cFields[0] || typingIn() == cFields[0];
    CHECK(inC && !eFields[0]->isTextEditing(), "and the focus stays in the field: %s",
          QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "nothing");
    press(Qt::Key_Escape);
    cFields[0]->beginTextEdit();
    settle();
    type(QStringLiteral("8"));
    press(Qt::Key_Escape);
    CHECK(valueOf<float>(c) == 7.0f, "Esc cancels");
    CHECK(QApplication::focusWidget() == cFields[0] && !eFields[0]->isTextEditing(), "and stays put too");
    press(Qt::Key_Tab);
    CHECK(typingIn() == eFields[0], "Tab from a number field that isn't being typed in moves on as well");
    press(Qt::Key_Escape);

    // A collapsed section's fields are skipped: collapse the one a, b and c are in.
    CategorySection* section = nullptr;
    for (QWidget* at = aFields[0]; at && !section; at = at->parentWidget()) section = qobject_cast<CategorySection*>(at);
    CHECK(section != nullptr, "a is in a section");
    if (section) {
        section->setCollapsed(true);
        settle();
        first->setFocus(Qt::OtherFocusReason);
        settle();
        press(Qt::Key_Tab);
        QWidget* focus = QApplication::focusWidget();
        const bool inSection = focus && section->isAncestorOf(focus);
        CHECK(!inSection, "nothing in a collapsed section takes the focus");
        section->setCollapsed(false);
    }
}

USDCC_TEST(attributes_tab_after_a_rename) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    const UsdPrim prim = s->DefinePrim(SdfPath("/Chair"), TfToken("Scope"));
    prim.CreateAttribute(TfToken("height"), SdfValueTypeNames->Float, true).Set(1.0f);

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.resize(500, 900);
    panel.show();
    panel.activateWindow();
    stages.setSelectedPaths(stage, {SdfPath("/Chair")});
    settle();

    QLineEdit* name = nullptr;
    for (QLineEdit* edit : panel.findChildren<QLineEdit*>()) {
        if (edit->text() == QStringLiteral("Chair")) name = edit;
    }
    CHECK(name != nullptr, "the Name field");
    if (!name) return;
    name->setFocus(Qt::TabFocusReason);
    name->selectAll();
    settle();
    type(QStringLiteral("Stool"));
    press(Qt::Key_Tab);
    CHECK(s->GetPrimAtPath(SdfPath("/Stool")), "Tab commits the rename");
    // The rename rebuilds the rows; the field Tab moved to takes the focus in the new ones.
    const std::vector<NumberField*> height = numbersOf(rowOf(panel, "height"));
    CHECK(height.size() == 1 && typingIn() == height[0], "typing in the field after the name, in the rebuilt rows: %s",
          QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "nothing");
}
