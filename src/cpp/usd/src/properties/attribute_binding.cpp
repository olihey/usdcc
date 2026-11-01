#include "usdcc/usd/properties/attribute_binding.h"

#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/timeCode.h>

namespace usdcc::usd::properties {

using namespace PXR_NS;

AttributeBinding::AttributeBinding(UsdAttribute attribute, PropertyEditor* editor, StageRefPtr stage,
                                   UndoManager* undoManager, QObject* parent)
    : QObject(parent),
      m_attribute(std::move(attribute)),
      m_editor(editor),
      m_stage(std::move(stage)),
      m_undoManager(undoManager) {
    connect(editor, &PropertyEditor::editStarted, this, &AttributeBinding::onEditStarted);
    connect(editor, &PropertyEditor::valueEdited, this, &AttributeBinding::onValueEdited);
    connect(editor, &PropertyEditor::editFinished, this, &AttributeBinding::onEditFinished);
    connect(editor, &PropertyEditor::editCanceled, this, &AttributeBinding::onEditCanceled);
    refresh();
}

VtValue AttributeBinding::currentValue(const UsdAttribute& attribute) {
    VtValue value;
    if (!attribute.Get(&value, UsdTimeCode::Default()) && attribute.GetNumTimeSamples() > 0) {
        attribute.Get(&value, UsdTimeCode::EarliestTime());
    }
    return value;
}

void AttributeBinding::refresh() {
    if (!m_editing && m_attribute) {
        m_editor->setValue(currentValue(m_attribute));
    }
}

void AttributeBinding::resetToDefault() {
    if (!m_attribute) return;
    if (m_undoManager && m_stage) {
        m_undoManager->apply(m_stage, tr("Reset %1").arg(QString::fromStdString(m_attribute.GetName().GetString())),
                             [this]() { return m_attribute.Clear(); });
    } else {
        m_attribute.Clear();
    }
    refresh();
}

void AttributeBinding::onEditStarted() {
    if (m_editing) return;
    m_editing = true;
    m_startValue = currentValue(m_attribute);
    m_startAuthored = m_attribute.HasAuthoredValue();
    if (m_undoManager && m_stage) {
        m_undoManager->beginEdit(m_stage,
                                 tr("Edit %1").arg(QString::fromStdString(m_attribute.GetName().GetString())));
    }
}

void AttributeBinding::onValueEdited(const VtValue& value) {
    if (value.IsEmpty() || !m_attribute) return;
    // A single-shot edit can arrive without a gesture around it in odd cases;
    // treat it as one.
    const bool implicitGesture = !m_editing;
    if (implicitGesture) onEditStarted();
    if (m_attribute.Set(value, UsdTimeCode::Default())) {
        Q_EMIT valueWritten(value);
    }
    if (implicitGesture) onEditFinished();
}

void AttributeBinding::onEditFinished() {
    if (!m_editing) return;
    m_editing = false;
    if (m_undoManager && m_stage) {
        m_undoManager->endEdit(m_stage);
    }
    refresh();
    Q_EMIT editCommitted();
}

void AttributeBinding::onEditCanceled() {
    if (!m_editing) return;
    m_editing = false;
    if (m_undoManager && m_stage) {
        m_undoManager->cancelEdit(m_stage);
    } else if (m_startAuthored) {
        m_attribute.Set(m_startValue, UsdTimeCode::Default());
    } else {
        m_attribute.ClearDefault();
    }
    refresh();
}

}  // namespace usdcc::usd::properties
