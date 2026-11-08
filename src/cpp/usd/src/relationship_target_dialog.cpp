#include "usdcc/usd/relationship_target_dialog.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/prim_tree_widget.h"
#include "usdcc/usd/relationship_targets.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <optional>

namespace usdcc::usd {

RelationshipTargetDialog::RelationshipTargetDialog(const PXR_NS::UsdStageRefPtr& stage, const std::string& relationship,
                                                   const PXR_NS::SdfPath& current, QWidget* parent)
    : QDialog(parent) {
    const QString name = QString::fromStdString(relationship);
    setWindowTitle(tr("Target of %1").arg(name));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tree = new PrimTreeWidget(this);
    // Asked of the registry now, so rules changed at runtime count.
    m_tree->setRowStatus([relationship](const PXR_NS::UsdPrim& prim) {
        const RelationshipTargetVerdict verdict = RelationshipTargetRules::instance().check(relationship, prim);
        UsdPrimTreeModel::RowStatus status;
        status.usable = verdict.kind != RelationshipTargetVerdict::Kind::Blocked;
        status.warning = verdict.kind == RelationshipTargetVerdict::Kind::Warn;
        status.note = QString::fromStdString(verdict.reason);
        return status;
    });
    m_tree->setStage(stage);
    layout->addWidget(m_tree, 1);

    if (const std::optional<RelationshipTargetRule> rule = RelationshipTargetRules::instance().find(relationship);
        rule && !rule->reason.empty()) {
        auto* note = new QLabel(QString::fromStdString(rule->reason), this);
        note->setObjectName(QStringLiteral("propertyNote"));
        note->setContentsMargins(8, 2, 8, 2);
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    auto* buttons = new QDialogButtonBox(this);
    usdcc::ui::scaled::margins(buttons->layout(), 8, 6, 8, 8);
    m_use = buttons->addButton(tr("Use Prim"), QDialogButtonBox::AcceptRole);
    m_use->setDefault(true);
    m_use->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_tree, &PrimTreeWidget::selectionChanged, this, [this]() { m_use->setEnabled(!m_tree->selectedPath().IsEmpty()); });
    connect(m_tree, &PrimTreeWidget::rowActivated, this, &QDialog::accept);
    layout->addWidget(buttons);

    if (!current.IsEmpty()) m_tree->selectPath(current.GetPrimPath());
    m_tree->filterEdit()->setFocus();
    resize(usdcc::ui::scaled::size(480, 640));
}

void RelationshipTargetDialog::allowNoTarget(const QString& buttonText) {
    auto* buttons = findChild<QDialogButtonBox*>();
    QPushButton* none = buttons->addButton(buttonText, QDialogButtonBox::ActionRole);
    none->setAutoDefault(false);
    connect(none, &QPushButton::clicked, this, [this]() {
        m_noTarget = true;
        accept();
    });
}

PXR_NS::SdfPath RelationshipTargetDialog::selectedPath() const {
    return m_noTarget ? PXR_NS::SdfPath() : m_tree->selectedPath();
}

void RelationshipTargetDialog::reject() {
    if (!m_tree->isFiltering()) {
        QDialog::reject();
    } else {
        m_tree->clearFilter();
    }
}

}  // namespace usdcc::usd
