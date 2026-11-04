#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/properties/property_editor.h"

#include <pxr/usd/sdf/path.h>

#include <vector>

class QToolButton;

namespace usdcc::usd::properties {

// A relationship's row in the Attribute Editor (docs/RELATIONSHIPS.md): a pill for
// each target showing its path, which selects it, with an × to remove it, wrapping
// to more lines. One with a problem (it doesn't exist, or its rule objects) is
// amber, with the reason as its tooltip. The row's last control adds a target.
class USDCC_USD_UI_API RelationshipTargetsEditor : public PropertyEditor {
    Q_OBJECT

public:
    struct Target {
        PXR_NS::SdfPath path;
        QString problem;  // empty when it's fine
    };

    explicit RelationshipTargetsEditor(QWidget* parent = nullptr);

    // `replaces`: the relationship holds one target, so the button changes it.
    void setTargets(const std::vector<Target>& targets, bool replaces);
    QToolButton* addButton() const { return m_add; }

    void setValue(const PXR_NS::VtValue&) override {}
    PXR_NS::VtValue value() const override { return {}; }
    void setReadOnly(bool readOnly) override;
    QWidget* rowButton() const override;

Q_SIGNALS:
    void selectRequested(const PXR_NS::SdfPath& path);
    void removeRequested(const PXR_NS::SdfPath& path);
    void addRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    void rebuild();
    void updateHeight();

    std::vector<Target> m_targets;
    bool m_replaces = false;
    bool m_built = false;
    QToolButton* m_add;
};

}  // namespace usdcc::usd::properties
