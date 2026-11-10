#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/stage.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>

#include <QObject>
#include <QString>

namespace usdcc::usd {
class UndoManager;
}

namespace usdcc::usd::properties {

class PropertyEditor;

// Connects one PropertyEditor to one UsdAttribute (docs/ATTRIBUTE_EDITOR.md
// §2.1): an edit gesture opens an undo session, every live value is written
// with UsdAttribute::Set() so the viewport follows, and the gesture's end
// commits the session (one undo step) or, on cancel, rolls it back.
//
// Without an UndoManager (the data-type gallery's scratch stage) edits are
// written directly and a cancel restores the value the gesture started from.
//
// refresh() pulls the attribute's current value into the editor; it is skipped
// while a gesture is in progress so live notices can't fight the user.
class USDCC_USD_UI_API AttributeBinding : public QObject {
    Q_OBJECT

public:
    AttributeBinding(PXR_NS::UsdAttribute attribute, PropertyEditor* editor, StageRefPtr stage,
                     UndoManager* undoManager, QObject* parent = nullptr);
    // For an attribute that may not exist yet, such as a shader input its definition
    // describes (docs/MATERIAL_EDITOR.md §7.1): until it does, the editor shows `fallback`,
    // and the first edit creates it as `type`, in the same undo step.
    AttributeBinding(const PXR_NS::UsdPrim& prim, const PXR_NS::TfToken& name, const PXR_NS::SdfValueTypeName& type,
                     PXR_NS::VtValue fallback, PropertyEditor* editor, StageRefPtr stage, UndoManager* undoManager,
                     QObject* parent = nullptr);

    const PXR_NS::UsdAttribute& attribute() const { return m_attribute; }
    // Whether the attribute exists: always, unless made with a fallback.
    bool exists() const;
    PropertyEditor* editor() const { return m_editor; }
    bool isEditing() const { return m_editing; }

    void refresh();
    // Clears the authored value in the edit target (undoable).
    void resetToDefault();

    // The value shown for an attribute: its default, or the first time sample
    // when it only has samples (there's no timeline to choose a time yet).
    static PXR_NS::VtValue currentValue(const PXR_NS::UsdAttribute& attribute);

Q_SIGNALS:
    void valueWritten(const PXR_NS::VtValue& value);
    void editCommitted();

private:
    void onEditStarted();
    void onValueEdited(const PXR_NS::VtValue& value);
    void onEditFinished();
    void onEditCanceled();

    // Creates the attribute if it doesn't exist, as the fallback constructor said.
    bool ensureAttribute();

    PXR_NS::UsdAttribute m_attribute;
    PropertyEditor* m_editor;
    StageRefPtr m_stage;
    UndoManager* m_undoManager;
    bool m_editing = false;
    PXR_NS::VtValue m_startValue;
    bool m_startAuthored = false;
    PXR_NS::UsdPrim m_prim;  // with a fallback: where it's created
    PXR_NS::TfToken m_name;
    PXR_NS::SdfValueTypeName m_type;
    PXR_NS::VtValue m_fallback;
    bool m_creates = false;
};

}  // namespace usdcc::usd::properties
