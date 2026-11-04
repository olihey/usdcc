#pragma once

#include "usdcc/usd/applied_schemas.h"
#include "usdcc/usd/properties/property_editor.h"

#include <vector>

class QToolButton;

namespace usdcc::usd::properties {

// The Attribute Editor's Applied Schemas row (docs/APPLIED_SCHEMAS.md §3): a
// pill for each schema, wrapping to more lines, with an × on those that can be
// removed. A built-in one is dimmed, an unregistered one amber. Its + (the
// row's last control, rowButton()) is for its owner to give a menu.
class SchemaPillsEditor : public PropertyEditor {
    Q_OBJECT

public:
    explicit SchemaPillsEditor(QWidget* parent = nullptr);

    void setSchemas(const std::vector<AppliedSchema>& schemas);
    QToolButton* addButton() const { return m_add; }

    // Not a value's editor: the prim's schemas come through setSchemas().
    void setValue(const PXR_NS::VtValue&) override {}
    PXR_NS::VtValue value() const override { return {}; }
    void setReadOnly(bool readOnly) override;
    QWidget* rowButton() const override;

Q_SIGNALS:
    void removeRequested(const QString& schema);

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    void rebuild();
    void updateHeight();

    std::vector<AppliedSchema> m_schemas;
    QToolButton* m_add;
};

}  // namespace usdcc::usd::properties
