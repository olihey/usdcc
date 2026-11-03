// Read-only fields have their own background; and in PropertyRow's layout an
// editor's row button (an array's Edit) is the row's rightmost control, after
// a trailing control and a note, and a square icon button. Runs in
// usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/theme.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_row.h"

#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/types.h>

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QTemporaryDir>
#include <QToolButton>

#include <algorithm>
#include <cstdlib>

using usdcc::usd::properties::PropertyRow;
PXR_NAMESPACE_USING_DIRECTIVE

USDCC_TEST(property_read_only_fields_look_different) {
    QTemporaryDir settings;
    auto& app = *static_cast<QApplication*>(QCoreApplication::instance());
    usdcc::ui::UiScaleController scale(app, settings.filePath(QStringLiteral("ui.ini")));
    const QString before = app.styleSheet();
    usdcc::ui::theme::apply(app);

    QWidget panel;
    panel.setObjectName(QStringLiteral("panel"));
    panel.setStyleSheet(QStringLiteral("QWidget#panel { background: #1c1c1e; }"));  // the Attribute Editor's
    auto* editable = usdcc::usd::properties::createPropertyEditor(SdfValueTypeNames->String, {}, &panel);
    auto* readOnly = usdcc::usd::properties::createPropertyEditor(SdfValueTypeNames->String, {}, &panel);
    auto* number = usdcc::usd::properties::createPropertyEditor(SdfValueTypeNames->Float, {}, &panel);
    auto* array = usdcc::usd::properties::createPropertyEditor(SdfValueTypeNames->FloatArray, {}, &panel);
    auto* matrix = usdcc::usd::properties::createPropertyEditor(SdfValueTypeNames->Matrix4d, {}, &panel);
    readOnly->setReadOnly(true);
    number->setReadOnly(true);
    for (auto* editor : {editable, readOnly, number, array, matrix}) editor->setValue(VtValue());
    editable->setGeometry(10, 10, 200, 20);
    readOnly->setGeometry(10, 40, 200, 20);
    number->setGeometry(10, 70, 200, 20);
    array->setGeometry(10, 100, 200, 20);
    matrix->setGeometry(10, 130, 200, 20);
    panel.resize(220, 160);
    panel.show();
    QApplication::processEvents();

    const QImage image = panel.grab().toImage();
    if (const char* out = std::getenv("USDCC_TEST_SNAPSHOT")) image.save(QString::fromLocal8Bit(out));
    // Inside each field, right of any text.
    const QColor editableFill = image.pixelColor(190, 20);
    const QColor readOnlyFill = image.pixelColor(190, 50);
    const QColor numberFill = image.pixelColor(190, 80);
    const QColor arrayFill = image.pixelColor(150, 110);  // the summary, left of its Edit button
    const QColor matrixFill = image.pixelColor(190, 140);  // the summary, right of its disclosure arrow
    CHECK(readOnlyFill != editableFill && readOnlyFill.lightness() > editableFill.lightness(),
          "a read-only field is lighter than an editable one: %s vs %s", qPrintable(readOnlyFill.name()),
          qPrintable(editableFill.name()));
    CHECK(numberFill == readOnlyFill, "and a read-only number field matches it: %s vs %s",
          qPrintable(numberFill.name()), qPrintable(readOnlyFill.name()));
    CHECK(arrayFill == readOnlyFill, "so does an array's summary, edited only through Edit: %s vs %s",
          qPrintable(arrayFill.name()), qPrintable(readOnlyFill.name()));
    CHECK(matrixFill == readOnlyFill, "and a matrix's (a transform op's), edited in its grid: %s vs %s",
          qPrintable(matrixFill.name()), qPrintable(readOnlyFill.name()));
    app.setStyleSheet(before);
}

USDCC_TEST(property_row_edit_button_is_rightmost) {
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    auto* editor = usdcc::usd::properties::createPropertyEditor(SdfValueTypeNames->FloatArray);
    auto* edit = qobject_cast<QToolButton*>(editor->rowButton());
    CHECK(edit && edit->objectName() == "propertyIconButton" && !edit->icon().isNull() && edit->text().isEmpty(),
          "an array's Edit is an icon button");
    if (!edit) return;

    PropertyRow row(QStringLiteral("points"), editor);
    auto* trailing = new QLabel(QStringLiteral("trailing"));
    row.addTrailingWidget(trailing);
    row.setNote(QStringLiteral("note"));
    row.resize(500, 30);
    row.show();
    QApplication::processEvents();

    int rightmost = 0;
    for (QWidget* child : row.findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (child->isVisible() && child != edit) rightmost = std::max(rightmost, child->geometry().right());
    }
    CHECK(edit->parentWidget() == &row && edit->geometry().left() > rightmost,
          "Edit comes after everything else: %d > %d", edit->geometry().left(), rightmost);
    CHECK(edit->width() == edit->height(), "and is square: %dx%d", edit->width(), edit->height());
}
