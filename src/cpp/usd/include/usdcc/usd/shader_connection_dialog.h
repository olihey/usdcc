#pragma once

#include "usdcc/core/export.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>

#include <QDialog>

#include <string>
#include <utility>
#include <vector>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::usd {

// Picks the other end of a UsdShade connection (docs/ATTRIBUTE_EDITOR.md §4.3): the
// prims of the attribute's connection scope (its material) as a tree on the left,
// the selected node's attributes on the right, and Connect, enabled once an
// attribute that can be connected is selected.
//
// - PickSource, from a receiver (an input): only what could be its source
//   (whyNotPossibleSource()): other nodes' outputs, and the inputs of the
//   materials and node graphs it is inside. Those of the wrong type are greyed,
//   with the reason as their tooltip.
// - PickReceiver, from a shader output: only the node's unconnected receivers of
//   the same type that may take the output.
//
// The attribute's own prim, and nodes with nothing to list, are shown but can't
// be picked.
class USDCC_USD_UI_API ShaderConnectionDialog : public QDialog {
    Q_OBJECT

public:
    enum class Mode { PickSource, PickReceiver };

    ShaderConnectionDialog(const PXR_NS::UsdAttribute& attribute, Mode mode, QWidget* parent = nullptr);

    // The picked attribute's path; empty until one is selected.
    PXR_NS::SdfPath selected() const;

    QTreeWidget* nodeTree() const { return m_nodes; }
    QTreeWidget* attributeList() const { return m_attributes; }
    QPushButton* connectButton() const { return m_connect; }
    // The tree's item for `prim`, or null.
    QTreeWidgetItem* nodeItem(const PXR_NS::SdfPath& prim) const;
    // The list's item for `attribute` (of the shown node), or null.
    QTreeWidgetItem* attributeItem(const PXR_NS::TfToken& attribute) const;

private:
    void addNode(const PXR_NS::UsdPrim& prim, QTreeWidgetItem* parent);
    void showAttributes();
    void updateConnectButton();
    // What the right side lists for `node` in this mode, each with why it can't
    // be picked (empty when it can).
    std::vector<std::pair<PXR_NS::UsdAttribute, std::string>> candidates(const PXR_NS::UsdPrim& node) const;

    PXR_NS::UsdAttribute m_attribute;
    Mode m_mode;
    QTreeWidget* m_nodes;
    QTreeWidget* m_attributes;
    QPushButton* m_connect;
};

}  // namespace usdcc::usd
