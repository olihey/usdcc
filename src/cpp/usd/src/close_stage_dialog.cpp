#include "usdcc/usd/close_stage_dialog.h"

#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include <vector>

namespace usdcc::usd {

namespace {
QString displayName(const StageRefPtr& stage) {
    return QString::fromStdString(stage->usdStage()->GetRootLayer()->GetDisplayName());
}

// Every panel showing `stage`, including ones closed (hidden) in the dock
// manager: they still hold the stage.
std::vector<usdcc::ui::ViewPanel*> panelsShowing(const StageRefPtr& stage) {
    std::vector<usdcc::ui::ViewPanel*> panels;
    for (QWidget* widget : QApplication::allWidgets()) {
        auto* panel = qobject_cast<usdcc::ui::ViewPanel*>(widget);
        if (panel && panel->currentStage() == stage) {
            panels.push_back(panel);
        }
    }
    return panels;
}
}  // namespace

bool closeStageInteractively(StageManager* stageManager, const StageRefPtr& stage, QWidget* dialogParent) {
    if (!stage) {
        return false;
    }
    const std::vector<usdcc::ui::ViewPanel*> panels = panelsShowing(stage);
    // Only panels the user can see are worth asking about; hidden ones just
    // follow the answer (or, with no question asked, are left empty).
    QStringList visibleTitles;
    for (usdcc::ui::ViewPanel* panel : panels) {
        if (!panel->isClosed()) {
            visibleTitles << panel->windowTitle();
        }
    }

    if (!visibleTitles.isEmpty()) {
        CloseStageDialog dialog(stageManager, stage, visibleTitles, dialogParent);
        if (dialog.exec() != QDialog::Accepted) {
            return false;
        }
        const StageRefPtr replacement = dialog.replacement();
        for (usdcc::ui::ViewPanel* panel : panels) {
            if (dialog.closePanels()) {
                panel->deleteDockWidget();  // removes it from the dock manager, then deleteLater()
            } else {
                panel->setStage(replacement);
            }
        }
    }

    // Panels not handled above (hidden ones, when nothing was asked) are left
    // with no stage by ViewPanel's own stageClosed handler.
    stageManager->closeStage(stage);
    return true;
}

CloseStageDialog::CloseStageDialog(StageManager* stageManager, const StageRefPtr& stage,
                                   const QStringList& panelTitles, QWidget* parent)
    : QDialog(parent), m_stageManager(stageManager) {
    setWindowTitle(tr("Close Stage"));
    setMinimumWidth(420);  // room for the panel list before it wraps

    auto* layout = new QVBoxLayout(this);
    auto* message = new QLabel(
        tr("<b>%1</b> is still shown in: %2.<br>What should happen to these panels?")
            .arg(displayName(stage).toHtmlEscaped(), panelTitles.join(QStringLiteral(", ")).toHtmlEscaped()),
        this);
    message->setWordWrap(true);
    layout->addWidget(message);

    auto* choices = new QGridLayout;
    m_closeButton = new QRadioButton(tr("&Close the panels"), this);
    m_switchButton = new QRadioButton(tr("&Switch them to:"), this);
    m_replacementCombo = new QComboBox(this);
    for (const StageRefPtr& other : stageManager->stages()) {
        if (other != stage) {
            m_replacementCombo->addItem(displayName(other), static_cast<qlonglong>(other->cacheId()));
            m_replacementCombo->setItemData(m_replacementCombo->count() - 1,
                                            QString::fromStdString(other->usdStage()->GetRootLayer()->GetIdentifier()),
                                            Qt::ToolTipRole);
        }
    }
    m_replacementCombo->addItem(tr("No stage"));  // no data: leaves the panels empty
    choices->addWidget(m_closeButton, 0, 0, 1, 2);
    choices->addWidget(m_switchButton, 1, 0);
    choices->addWidget(m_replacementCombo, 1, 1);
    choices->setColumnStretch(1, 1);
    layout->addLayout(choices);

    // Switching keeps the layout, so it is the default.
    m_switchButton->setChecked(true);
    connect(m_switchButton, &QRadioButton::toggled, m_replacementCombo, &QWidget::setEnabled);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->addButton(tr("Close Stage"), QDialogButtonBox::AcceptRole)->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

bool CloseStageDialog::closePanels() const { return m_closeButton->isChecked(); }

StageRefPtr CloseStageDialog::replacement() const {
    const QVariant cacheId = m_replacementCombo->currentData();
    return cacheId.isValid() ? m_stageManager->findByCacheId(static_cast<long>(cacheId.toLongLong())) : StageRefPtr();
}

}  // namespace usdcc::usd
