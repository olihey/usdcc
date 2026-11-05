#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>

#include <QObject>
#include <QString>

#include <functional>
#include <vector>

class QCheckBox;
class QMenu;
class QWidget;

namespace usdcc::usd {
class UndoManager;
class XformStack;
}  // namespace usdcc::usd

namespace usdcc::usd::properties {

class AttributeBinding;
class CategorySection;
class PropertyRow;

// The attribute editor's Transform section for a UsdGeomXformable prim
// (docs/ATTRIBUTE_EDITOR.md §6): one row per op in xformOpOrder order — drag
// handle, label ("Translate (pivot)", "… ⁻¹" for inverse ops), the value's
// editor, a remove button — plus "Reset Stack" and "+ Add" in the section
// header. Rows reorder by dragging their handle; the context menu adds move
// up/down, rename, reset, pivot, bake and convert. Every action is one undo
// step (XformStack does the work).
//
// Clicking an op's label makes it the target of the Move/Rotate/Scale tools
// (Stage::setTargetXformOp) — clicking it again goes back to Auto; the target
// row is marked "◆".
//
// The section is rebuilt from scratch after every structural change (the
// panel sees the resync), so the editor never has to patch its rows.
class USDCC_USD_UI_API TransformStackEditor : public QObject {
    Q_OBJECT

public:
    struct OpRow {
        PropertyRow* row = nullptr;
        AttributeBinding* binding = nullptr;  // null for inverse ops (no value of their own)
        PXR_NS::TfToken opName;               // as in xformOpOrder (with !invert! for inverses)
        PXR_NS::TfToken attrName;
    };

    TransformStackEditor(CategorySection* section, const PXR_NS::UsdPrim& prim, StageRefPtr stage,
                         UndoManager* undoManager, QObject* parent = nullptr);

    const std::vector<OpRow>& rows() const { return m_rows; }

    // Re-reads what can change without a rebuild (Reset Stack's state).
    void refresh();

    // Moves op `from` to index `to` (one undo step). Used by drag and drop.
    void moveOp(size_t from, size_t to);

private:
    PXR_NS::UsdPrim prim() const;
    bool edit(const QString& description, const std::function<bool(XformStack&)>& change);
    void addOp(int type);
    void updateTargetMarkers();
    void toggleTarget(size_t index);
    size_t insertIndex() const;
    void populateRowMenu(QMenu* menu, size_t index);
    void populateStackActions(QMenu* menu);

    PXR_NS::SdfPath m_primPath;
    StageRefPtr m_stage;
    UndoManager* m_undoManager;
    std::vector<OpRow> m_rows;
    QCheckBox* m_resetStack = nullptr;
    usdcc::core::Signal<>::Connection m_targetConnection;
};

}  // namespace usdcc::usd::properties
