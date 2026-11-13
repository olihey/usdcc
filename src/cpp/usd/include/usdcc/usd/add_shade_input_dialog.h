#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/shade_actions.h"

#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/prim.h>

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QPushButton;

namespace usdcc::usd {

namespace properties {
class PropertyEditor;
}

// What a new input of a Material, NodeGraph or Shader is (docs/MATERIAL_EDITOR.md §4): its
// name, type, default value (in the Attribute Editor's own editor for that type), allowed
// tokens, connectability, display name and group, and documentation. Opened from the Material
// Editor's node menu and the + on an Attribute Editor's Inputs header; nothing is authored
// here, shade_actions::addInput() authors spec().
class USDCC_USD_UI_API AddShadeInputDialog : public QDialog {
    Q_OBJECT

public:
    explicit AddShadeInputDialog(const PXR_NS::UsdPrim& prim, QWidget* parent = nullptr);

    shade_actions::InputSpec spec() const;

    // The chosen type: the scalar type, or its array type with Array checked.
    PXR_NS::SdfValueTypeName typeName() const;
    // The type picked in the Type menu ("float", "color3f", ...), not an array. False for
    // what isn't one, which changes nothing.
    bool setScalarType(const QString& token);
    QString scalarType() const { return m_scalarType; }

    QLineEdit* nameField() const { return m_name; }
    // Shows the type; opens the menu of them: the common ones, then a submenu per group.
    QPushButton* typeButton() const { return m_type; }
    QMenu* typeMenu() const { return m_typeMenu; }
    QCheckBox* arrayCheck() const { return m_array; }
    QCheckBox* setDefaultCheck() const { return m_setDefault; }
    properties::PropertyEditor* valueEditor() const { return m_value; }
    QLineEdit* allowedTokensField() const { return m_allowedTokens; }
    QComboBox* connectabilityCombo() const { return m_connectability; }
    QLineEdit* displayNameField() const { return m_displayName; }
    QLineEdit* displayGroupField() const { return m_displayGroup; }
    QPlainTextEdit* documentationField() const { return m_documentation; }
    QPushButton* addButton() const { return m_add; }
    QLabel* problemLabel() const { return m_problem; }

private:
    std::vector<PXR_NS::TfToken> allowedTokens() const;
    // A new value editor for the type and allowed tokens, keeping what it can of the value.
    void rebuildValueEditor();
    void updateState();

    PXR_NS::UsdPrim m_prim;
    QFormLayout* m_form = nullptr;
    QLineEdit* m_name = nullptr;
    QPushButton* m_type = nullptr;
    QMenu* m_typeMenu = nullptr;
    QString m_scalarType;
    QCheckBox* m_array = nullptr;
    QLineEdit* m_allowedTokens = nullptr;
    QCheckBox* m_setDefault = nullptr;
    QWidget* m_valueHolder = nullptr;
    properties::PropertyEditor* m_value = nullptr;
    QComboBox* m_connectability = nullptr;
    QLineEdit* m_displayName = nullptr;
    QLineEdit* m_displayGroup = nullptr;
    QPlainTextEdit* m_documentation = nullptr;
    QLabel* m_problem = nullptr;
    QPushButton* m_add = nullptr;
};

// Asks for a new input of `prim` and adds it, as one undo step. False if canceled or refused.
USDCC_USD_UI_API bool addShadeInputInteractively(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& prim, QWidget* parent);

}  // namespace usdcc::usd
