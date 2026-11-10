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

AttributeBinding::AttributeBinding(const UsdPrim& prim, const TfToken& name, const SdfValueTypeName& type, VtValue fallback,
                                   PropertyEditor* editor, StageRefPtr stage, UndoManager* undoManager, QObject* parent)
    : QObject(parent),
      m_attribute(prim.GetAttribute(name)),
      m_editor(editor),
      m_stage(std::move(stage)),
      m_undoManager(undoManager),
      m_prim(prim),
      m_name(name),
      m_type(type),
      m_fallback(std::move(fallback)),
      m_creates(true) {
    connect(editor, &PropertyEditor::editStarted, this, &AttributeBinding::onEditStarted);
    connect(editor, &PropertyEditor::valueEdited, this, &AttributeBinding::onValueEdited);
    connect(editor, &PropertyEditor::editFinished, this, &AttributeBinding::onEditFinished);
    connect(editor, &PropertyEditor::editCanceled, this, &AttributeBinding::onEditCanceled);
    refresh();
}

bool AttributeBinding::exists() const { return m_attribute.IsValid() && m_attribute.IsDefined(); }

bool AttributeBinding::ensureAttribute() {
    if (exists()) return true;
    if (!m_creates || !m_prim || !m_type) return false;
    m_attribute = m_prim.CreateAttribute(m_name, m_type, /*custom=*/false);
    return exists();
}

VtValue AttributeBinding::currentValue(const UsdAttribute& attribute) {
    VtValue value;
    if (!attribute.Get(&value, UsdTimeCode::Default()) && attribute.GetNumTimeSamples() > 0) {
        attribute.Get(&value, UsdTimeCode::EarliestTime());
    }
    return value;
}

void AttributeBinding::refresh() {
    if (m_editing) return;
    if (exists()) {
        m_editor->setValue(currentValue(m_attribute));
    } else if (m_creates) {
        m_editor->setValue(m_fallback);
    }
}

void AttributeBinding::resetToDefault() {
    if (!exists()) return;
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
    const bool existed = exists();
    m_startValue = existed ? currentValue(m_attribute) : m_fallback;
    m_startAuthored = existed && m_attribute.HasAuthoredValue();
    const std::string name = existed ? m_attribute.GetName().GetString() : m_name.GetString();
    if (m_undoManager && m_stage) {
        m_undoManager->beginEdit(m_stage, tr("Edit %1").arg(QString::fromStdString(name)));
    }
    // Inside the session, so that undoing the edit removes it again.
    if (!existed) ensureAttribute();
}

void AttributeBinding::onValueEdited(const VtValue& value) {
    if (value.IsEmpty() || (!exists() && !m_creates)) return;
    // A single-shot edit can arrive without a gesture around it in odd cases;
    // treat it as one.
    const bool implicitGesture = !m_editing;
    if (implicitGesture) onEditStarted();
    if (exists() && m_attribute.Set(value, UsdTimeCode::Default())) {
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
    } else if (exists()) {
        m_attribute.ClearDefault();
    }
    refresh();
}

}  // namespace usdcc::usd::properties
