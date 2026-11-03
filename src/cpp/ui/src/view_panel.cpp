#include "usdcc/ui/view_panel.h"

#include "usdcc/ui/panel_menu_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>
#include <DockWidgetTab.h>
#include <QtCore/qcompilerdetection.h>

#include <QBoxLayout>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QMenu>
#include <QSignalBlocker>

#include <utility>

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED

namespace usdcc::ui {

ViewPanel::ViewPanel(usdcc::usd::StageManager* stageManager, usdcc::usd::StageRefPtr stage, const QString& title,
                      QWidget* parent)
    : ads::CDockWidget(title, parent), m_stageManager(stageManager), m_stage(std::move(stage)) {
    // ADS shows this panel as a tab (ads::CDockWidgetTab) in its dock area's
    // title bar, and that tab already has its own context menu
    // (Detach/Pin/Close/...) via CDockWidgetTab::contextMenuEvent(). An
    // event filter lets us extend that menu with our own entries without
    // subclassing CDockWidgetTab — ADS creates that internally via its own
    // component factory, with no hook to inject a custom subclass.
    // tabWidget() is already valid here: CDockWidget's constructor creates
    // it before returning.
    tabWidget()->installEventFilter(this);

    m_optionsButton = new PanelMenuButton(tr("Panel options"), [this](QMenu* menu) { populateOptionsMenu(menu); });

    m_stageCombo = new QComboBox();
    m_stageCombo->setPlaceholderText(tr("No stage"));  // shown while the panel has none
    connect(m_stageCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &ViewPanel::onStageComboChanged);

    m_stageOpenedConnection = m_stageManager->stageOpened.connect([this](usdcc::usd::StageRefPtr opened) {
        refreshStageCombo();
        // A panel with nothing to show picks up the stage that was just opened.
        if (!m_stage && adoptsOpenedStage()) {
            setStage(std::move(opened));
        }
    });
    m_stageClosedConnection = m_stageManager->stageClosed.connect([this](usdcc::usd::StageRefPtr closed) {
        refreshStageCombo();
        // Normally File > Close Stage has already closed or switched every
        // panel showing it; a stage closed some other way (e.g. from Python)
        // leaves its panels empty.
        if (closed == m_stage) {
            setStage(nullptr);
        }
    });
}

void ViewPanel::setStage(usdcc::usd::StageRefPtr stage) {
    if (stage == m_stage) {
        return;
    }
    m_stage = std::move(stage);

    const int index = m_stage ? m_stageCombo->findData(static_cast<qlonglong>(m_stage->cacheId())) : -1;
    if (index != m_stageCombo->currentIndex()) {
        const QSignalBlocker blocker(m_stageCombo);
        m_stageCombo->setCurrentIndex(index);
    }

    onStageChanged(m_stage);
    Q_EMIT stageChanged(m_stage);
}

void ViewPanel::addPanelControls(QBoxLayout* toolbarLayout) {
    // The same small margin on every side, so the options button sits in the
    // toolbar's corner without extra room around it.
    usdcc::ui::scaled::margins(toolbarLayout, 4, 4, 4, 4);
    toolbarLayout->addWidget(m_optionsButton);
    toolbarLayout->addWidget(m_stageCombo, 1);
}

void ViewPanel::refreshStageCombo() {
    const QSignalBlocker blocker(m_stageCombo);
    m_stageCombo->clear();
    // File names, with just enough folders to tell same-named files apart;
    // the full identifier is the tooltip.
    const auto& stages = m_stageManager->stages();
    const std::vector<std::string> names = usdcc::usd::shortStageNames(stages);
    for (std::size_t i = 0; i < stages.size(); ++i) {
        m_stageCombo->addItem(QString::fromStdString(names[i]), static_cast<qlonglong>(stages[i]->cacheId()));
        m_stageCombo->setItemData(m_stageCombo->count() - 1,
                                  QString::fromStdString(stages[i]->usdStage()->GetRootLayer()->GetIdentifier()),
                                  Qt::ToolTipRole);
    }
    m_stageCombo->setCurrentIndex(m_stage ? m_stageCombo->findData(static_cast<qlonglong>(m_stage->cacheId())) : -1);
    m_stageCombo->setEnabled(m_stageCombo->count() > 0);
}

void ViewPanel::onStageComboChanged(int index) {
    if (index < 0) {
        return;
    }
    const auto cacheId = static_cast<long>(m_stageCombo->itemData(index).toLongLong());
    if (auto stage = m_stageManager->findByCacheId(cacheId)) {
        setStage(stage);
    }
}

void ViewPanel::setContentWidget(QWidget* widget) { setWidget(widget); }

QWidget* ViewPanel::contentWidget() const { return widget(); }

void ViewPanel::populateContextMenu(QMenu* menu) {
    menu->addSeparator();
    menu->addAction(tr("Duplicate"), this, [this]() {
        auto* copy = duplicate(window());

        // ads::CDockManager looks dock widgets up by objectName() (CDockWidget's
        // constructor sets that to its title); giving the duplicate the exact
        // same title/objectName as this panel would silently overwrite this
        // panel's entry in that lookup map once both are docked, since
        // QMap::insert() replaces same-key entries rather than rejecting
        // them. Find a free "<title> (Copy N)" name instead.
        QString candidateTitle = tr("%1 (Copy)").arg(windowTitle());
        for (int suffix = 2; dockManager() && dockManager()->findDockWidget(candidateTitle); ++suffix) {
            candidateTitle = tr("%1 (Copy %2)").arg(windowTitle()).arg(suffix);
        }
        copy->setWindowTitle(candidateTitle);
        copy->setObjectName(candidateTitle);
        // So it is made again after a restart (ViewPanelRegistry::savePanels()).
        copy->setProperty(ViewPanelRegistry::kTypeProperty, property(ViewPanelRegistry::kTypeProperty));

        if (auto* area = dockAreaWidget()) {
            dockManager()->addDockWidgetTabToArea(copy, area);
        }
    });
}

bool ViewPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tabWidget() && event->type() == QEvent::ContextMenu) {
        auto* menuEvent = static_cast<QContextMenuEvent*>(event);
        QMenu* menu = tabWidget()->buildContextMenu(nullptr);
        populateContextMenu(menu);
        menu->exec(menuEvent->globalPos());
        menu->deleteLater();
        return true;
    }
    return ads::CDockWidget::eventFilter(watched, event);
}

}  // namespace usdcc::ui

QT_WARNING_POP
