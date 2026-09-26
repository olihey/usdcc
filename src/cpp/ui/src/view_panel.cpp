#include "usdcc/ui/view_panel.h"

#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>
#include <DockWidgetTab.h>
#include <QtCore/qcompilerdetection.h>

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

    m_stageCombo = new QComboBox();
    connect(m_stageCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &ViewPanel::onStageComboChanged);

    m_stageOpenedConnection =
        m_stageManager->stageOpened.connect([this](usdcc::usd::StageRefPtr) { refreshStageCombo(); });
    m_stageClosedConnection = m_stageManager->stageClosed.connect([this](usdcc::usd::StageRefPtr closed) {
        if (closed == m_stage) {
            // The stage this panel was showing is gone, and a ViewPanel can
            // never fall back to "no stage" — so the panel goes too.
            // deleteDockWidget() itself removes this from the dock manager
            // and calls deleteLater(), so this is safe to call synchronously
            // from within this signal handler.
            deleteDockWidget();
            return;
        }
        refreshStageCombo();
    });
}

void ViewPanel::setStage(usdcc::usd::StageRefPtr stage) {
    if (!stage || stage == m_stage) {
        return;
    }
    m_stage = std::move(stage);

    const int index = m_stageCombo->findData(static_cast<qlonglong>(m_stage->cacheId()));
    if (index != m_stageCombo->currentIndex()) {
        const QSignalBlocker blocker(m_stageCombo);
        m_stageCombo->setCurrentIndex(index);
    }

    onStageChanged(m_stage);
}

void ViewPanel::refreshStageCombo() {
    const QSignalBlocker blocker(m_stageCombo);
    m_stageCombo->clear();
    for (const auto& stage : m_stageManager->stages()) {
        const QString identifier = QString::fromStdString(stage->usdStage()->GetRootLayer()->GetIdentifier());
        m_stageCombo->addItem(identifier, static_cast<qlonglong>(stage->cacheId()));
    }
    m_stageCombo->setCurrentIndex(m_stageCombo->findData(static_cast<qlonglong>(m_stage->cacheId())));
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

ViewPanel* ViewPanel::duplicate(QWidget* parent) const {
    return new ViewPanel(m_stageManager, m_stage, windowTitle(), parent);
}

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
