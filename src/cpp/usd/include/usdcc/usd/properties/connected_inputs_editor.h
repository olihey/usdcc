#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/properties/property_editor.h"

#include <pxr/usd/sdf/path.h>

#include <QString>

#include <vector>

class QToolButton;

namespace usdcc::usd::properties {

// A shader output's row in the Attribute Editor (docs/ATTRIBUTE_EDITOR.md §4.3): a
// pill for each input connected to it, showing the input relative to the material,
// which selects that prim, with an × that disconnects it; "Not connected" when
// there are none. The row's last control connects another input.
class USDCC_USD_UI_API ConnectedInputsEditor : public PropertyEditor {
    Q_OBJECT

public:
    struct Receiver {
        PXR_NS::SdfPath path;  // the input
        QString label;         // as shown: "Surface.inputs:diffuseColor"
    };

    explicit ConnectedInputsEditor(QWidget* parent = nullptr);

    void setReceivers(const std::vector<Receiver>& receivers);
    QToolButton* connectButton() const { return m_connect; }

    void setValue(const PXR_NS::VtValue&) override {}
    PXR_NS::VtValue value() const override { return {}; }
    void setReadOnly(bool readOnly) override;
    QWidget* rowButton() const override;

Q_SIGNALS:
    void selectRequested(const PXR_NS::SdfPath& input);
    void disconnectRequested(const PXR_NS::SdfPath& input);
    void connectRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    void rebuild();
    void updateHeight();

    std::vector<Receiver> m_receivers;
    bool m_built = false;
    QToolButton* m_connect;
};

}  // namespace usdcc::usd::properties
