#pragma once

#include "usdcc/core/export.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QDialog>

#include <optional>

class QLineEdit;
class QPushButton;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTreeView;

namespace usdcc::usd {

// Picks one prim of a stage (a reference's or payload's target prim, in the
// Attribute Editor's Composition section): the stage's prims as a tree, with
// the Outliner's icons and each prim's type, the default prim in bold, and a
// filter that keeps the prims whose name contains the text (and their
// ancestors). "Use Prim" (or a double-click) accepts the selected prim.
class USDCC_USD_UI_API PrimPickerDialog : public QDialog {
    Q_OBJECT

public:
    // `current`, if it's in the stage, starts selected.
    PrimPickerDialog(const PXR_NS::UsdStageRefPtr& stage, const PXR_NS::SdfPath& current, QWidget* parent = nullptr);

    // The selected prim; empty for none.
    PXR_NS::SdfPath selectedPath() const;
    void setFilter(const QString& text);
    void selectPath(const PXR_NS::SdfPath& path);
    // The prims the filter leaves, for tests.
    int visiblePrimCount() const;

private:
    QLineEdit* m_filter;
    QTreeView* m_tree;
    QStandardItemModel* m_model;
    QSortFilterProxyModel* m_proxy;
    QPushButton* m_use;
};

// Opens `file` (without loading its payloads) and asks for one of its prims,
// starting at `current`. Nothing on Cancel, or when the file can't be opened
// (which a message box says).
std::optional<PXR_NS::SdfPath> pickPrimFromFile(const QString& file, const PXR_NS::SdfPath& current, QWidget* parent);

}  // namespace usdcc::usd
