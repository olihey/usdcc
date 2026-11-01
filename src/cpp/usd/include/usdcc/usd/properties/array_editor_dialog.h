#pragma once

#include "usdcc/usd/properties/property_editor.h"

#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/valueTypeName.h>

#include <QDialog>

class QTableView;

namespace usdcc::usd::properties {

class ArrayModel;

// Edits a copy of an array value (docs/ATTRIBUTE_EDITOR.md §4): one row per
// element, each edited with that element type's own PropertyEditor; add,
// insert, remove, resize, and paste one element per line from the clipboard.
// The table is virtual (rows are formatted on demand), so arrays with
// millions of elements open instantly. The caller applies array() as one edit
// when exec() returns Accepted.
class ArrayEditorDialog : public QDialog {
    Q_OBJECT

public:
    ArrayEditorDialog(const PXR_NS::VtValue& array, const PXR_NS::SdfValueTypeName& arrayType, EditorHints hints,
                      const QString& title, QWidget* parent = nullptr);

    PXR_NS::VtValue array() const;

private:
    void addElement();
    void insertElement();
    void removeElements();
    void resizeElements();
    void paste();
    void updateCount();

    PXR_NS::SdfValueTypeName m_arrayType;
    ArrayModel* m_model;
    QTableView* m_view;
    class QLabel* m_countLabel;
};

}  // namespace usdcc::usd::properties
