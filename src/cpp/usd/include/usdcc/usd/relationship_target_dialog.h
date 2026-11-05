#pragma once

#include "usdcc/core/export.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QDialog>
#include <QString>

#include <string>

class QPushButton;

namespace usdcc::usd {

class PrimTreeWidget;

// Picks what a relationship targets (docs/RELATIONSHIPS.md): the stage's prims as
// a tree with the Outliner's icons and a name filter. Prims the relationship's
// rule (RelationshipTargetRules) blocks are greyed out and can't be picked, with
// the rule's reason as their tooltip; one it warns about is amber.
class USDCC_USD_UI_API RelationshipTargetDialog : public QDialog {
    Q_OBJECT

public:
    // `current`, if it's there, starts selected.
    RelationshipTargetDialog(const PXR_NS::UsdStageRefPtr& stage, const std::string& relationship,
                             const PXR_NS::SdfPath& current, QWidget* parent = nullptr);

    // Adds a button that accepts with no target (selectedPath() empty).
    void allowNoTarget(const QString& buttonText);

    PXR_NS::SdfPath selectedPath() const;
    PrimTreeWidget* tree() const { return m_tree; }
    QPushButton* useButton() const { return m_use; }

public Q_SLOTS:
    // Esc: clears the filter first, then cancels.
    void reject() override;

private:
    PrimTreeWidget* m_tree;
    QPushButton* m_use;
    bool m_noTarget = false;
};

}  // namespace usdcc::usd
