#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/value_type_info.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/attribute.h>

#include <QString>
#include <QWidget>

#include <optional>
#include <utility>
#include <vector>

namespace usdcc::usd::properties {

// Per-attribute information that changes how a value is edited, beyond its
// type (docs/ATTRIBUTE_EDITOR.md §4).
struct EditorHints {
    std::vector<PXR_NS::TfToken> allowedTokens;  // token -> combo box
    // Ranges for numeric scalars: a soft range turns a number into a slider,
    // a hard range clamps. From UsdAttribute::GetSoftLimits()/GetHardLimits().
    std::optional<std::pair<double, double>> softRange;
    std::optional<std::pair<double, double>> hardRange;
    // Asset paths are shown resolved against, and made relative to, this layer.
    PXR_NS::SdfLayerHandle anchorLayer;
    QString suffix;  // e.g. "°"
    // Text fields: shown while empty (e.g. "defaultPrim").
    QString placeholder;

    static EditorHints forAttribute(const PXR_NS::UsdAttribute& attribute);
};

// Base class of every value editor in the attribute editor: one widget that
// shows and edits a VtValue of one SdfValueTypeName (docs/ATTRIBUTE_EDITOR.md
// §2.1). setValue() never emits; user edits are reported as editStarted, then
// any number of valueEdited (live), then editFinished — or editCanceled, after
// which the caller restores the value it had at editStarted.
class USDCC_USD_UI_API PropertyEditor : public QWidget {
    Q_OBJECT

public:
    PropertyEditor(ValueTypeInfo info, QWidget* parent = nullptr);

    const ValueTypeInfo& typeInfo() const { return m_info; }

    virtual void setValue(const PXR_NS::VtValue& value) = 0;
    virtual PXR_NS::VtValue value() const = 0;

    virtual void setReadOnly(bool readOnly);
    bool isReadOnly() const { return m_readOnly; }

    // True for the read-only text shown for a type no editor handles yet.
    virtual bool isFallback() const { return false; }

    // A button that belongs at the very end of the row (an array's Edit):
    // PropertyRow moves it there, after any note, badge or trailing control.
    // Null for none.
    virtual QWidget* rowButton() const { return nullptr; }
    // A second line under the value (an asset's resolved path): PropertyRow
    // moves it under the row, from the value's left edge to the row's right
    // edge, past any buttons. Null for none.
    virtual QWidget* subLineWidget() const { return nullptr; }

Q_SIGNALS:
    void editStarted();
    void valueEdited(const PXR_NS::VtValue& value);
    void editFinished();
    void editCanceled();

protected:
    // A single-shot edit (a toggle, a committed text field): start, value, finish.
    void emitSingleEdit(const PXR_NS::VtValue& value);

    ValueTypeInfo m_info;
    bool m_readOnly = false;
};

// The editor for `typeName` (scalar or array), or a read-only fallback if no
// editor handles it. Never null.
PropertyEditor* createPropertyEditor(const PXR_NS::SdfValueTypeName& typeName, const EditorHints& hints = {},
                                     QWidget* parent = nullptr);

}  // namespace usdcc::usd::properties
