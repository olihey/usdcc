#include "usdcc/ui/view_panel.h"

#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/panel_menu_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <QBoxLayout>
#include <QComboBox>
#include <QEvent>
#include <QMenu>
#include <QSignalBlocker>

#include <algorithm>
#include <utility>

namespace usdcc::ui {

ViewPanel::ViewPanel(usdcc::usd::StageManager* stageManager, usdcc::usd::StageRefPtr stage, const QString& title,
                      QWidget* parent)
    : DockPanel(title, parent), m_stageManager(stageManager), m_stage(std::move(stage)) {
    m_stageCombo = new QComboBox();
    m_stageCombo->setPlaceholderText(tr("No stage"));  // shown while the panel has none
    // Sized by that placeholder, not by the stage names: by default a combo takes its size
    // from what it holds when first shown (usually nothing yet) and again on every restyle,
    // so a UI scale change and back left it wider than before. The layout's stretch gives
    // it its room; a long name is elided.
    m_stageCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
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
    m_currentStageConnection = m_stageManager->currentStageChanged.connect([this](usdcc::usd::StageRefPtr current) {
        // With none current (the last stage closed), stageClosed above has emptied it.
        if (m_followsCurrentStage && current) setStage(std::move(current));
        syncStageCombo();  // its Follow Current Stage entry names the current stage
    });
    // Appearance > Follow Current Stage, for this panel's type.
    onSettingsChanged(this, [this](const std::string& key) {
        const QString type = property(ViewPanelRegistry::kTypeProperty).toString();
        if (!type.isEmpty() && key == ViewPanelRegistry::followSettingKey(type)) applyFollowSetting(true);
    });
}

void ViewPanel::setFollowsCurrentStage(bool follows) {
    if (follows == m_followsCurrentStage) return;
    m_followsCurrentStage = follows;
    if (follows) {
        if (usdcc::usd::StageRefPtr current = m_stageManager->currentStage()) setStage(std::move(current));
    }
    syncStageCombo();
}

void ViewPanel::applyFollowSetting(bool stopIfOff) {
    const QString type = property(ViewPanelRegistry::kTypeProperty).toString();
    if (type.isEmpty()) return;
    const bool on = ViewPanelRegistry::followsCurrentStageByDefault(type);
    if (on || stopIfOff) setFollowsCurrentStage(on);
}

bool ViewPanel::event(QEvent* event) {
    if (event->type() == QEvent::DynamicPropertyChange &&
        static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == ViewPanelRegistry::kTypeProperty) {
        applyFollowSetting(false);
    }
    return DockPanel::event(event);
}

void ViewPanel::setStage(usdcc::usd::StageRefPtr stage) {
    if (stage == m_stage) {
        return;
    }
    m_stage = std::move(stage);
    syncStageCombo();

    onStageChanged(m_stage);
    notifyOptionsChanged();  // what they offer may depend on the stage
    Q_EMIT stageChanged(m_stage);
}

void ViewPanel::addPanelControls(QBoxLayout* toolbarLayout) {
    // The same small margin on every side, so the options button sits in the
    // toolbar's corner without extra room around it.
    usdcc::ui::scaled::margins(toolbarLayout, 4, 4, 4, 4);
    toolbarLayout->addWidget(optionsButton());
    toolbarLayout->addWidget(m_stageCombo, 1);
}

namespace {
// The Stage dropdown's first entry; the stages follow a separator.
constexpr int kFollowIndex = 0;
}  // namespace

void ViewPanel::refreshStageCombo() {
    const QSignalBlocker blocker(m_stageCombo);
    m_stageCombo->clear();
    // No data: only the stages' entries carry a cache id.
    m_stageCombo->addItem(tr("Follow Current Stage"));
    m_stageCombo->setItemData(kFollowIndex, tr("Show whichever stage is current, and switch with it"), Qt::ToolTipRole);
    // File names, with just enough folders to tell same-named files apart;
    // the full identifier is the tooltip.
    const auto& stages = m_stageManager->stages();
    if (!stages.empty()) m_stageCombo->insertSeparator(m_stageCombo->count());
    const std::vector<std::string> names = usdcc::usd::shortStageNames(stages);
    for (std::size_t i = 0; i < stages.size(); ++i) {
        m_stageCombo->addItem(QString::fromStdString(names[i]), static_cast<qlonglong>(stages[i]->cacheId()));
        m_stageCombo->setItemData(m_stageCombo->count() - 1,
                                  QString::fromStdString(stages[i]->usdStage()->GetRootLayer()->GetIdentifier()),
                                  Qt::ToolTipRole);
    }
    syncStageCombo();
}

void ViewPanel::syncStageCombo() {
    if (m_stageCombo->count() == 0) return;  // not filled yet (refreshStageCombo())
    const QSignalBlocker blocker(m_stageCombo);
    // While following it says which stage that is now.
    const usdcc::usd::StageRefPtr current = m_stageManager->currentStage();
    const int currentIndex = current ? m_stageCombo->findData(static_cast<qlonglong>(current->cacheId())) : -1;
    m_stageCombo->setItemText(kFollowIndex, currentIndex > kFollowIndex
                                                ? tr("Follow Current Stage (%1)").arg(m_stageCombo->itemText(currentIndex))
                                                : tr("Follow Current Stage"));
    const int index = m_followsCurrentStage ? kFollowIndex
                      : m_stage             ? m_stageCombo->findData(static_cast<qlonglong>(m_stage->cacheId()))
                                            : -1;
    if (index != m_stageCombo->currentIndex()) m_stageCombo->setCurrentIndex(index);
}

void ViewPanel::onStageComboChanged(int index) {
    if (index < 0) {
        return;
    }
    if (index == kFollowIndex) {
        setFollowsCurrentStage(true);
        return;
    }
    const auto cacheId = static_cast<long>(m_stageCombo->itemData(index).toLongLong());
    if (auto stage = m_stageManager->findByCacheId(cacheId)) {
        // A stage picked by hand: this panel shows that one from now on.
        m_followsCurrentStage = false;
        setStage(stage);
        syncStageCombo();  // setStage() does nothing for the stage it already shows
    }
}

void ViewPanel::populateContextMenu(QMenu* menu) {
    DockPanel::populateContextMenu(menu);
    menu->addAction(tr("Duplicate"), this, [this]() { duplicateBeside(); });
}

ViewPanel* ViewPanel::duplicateBeside() {
    auto* copy = duplicate(window());

    // The same title: a tab never says "copy". ads::CDockManager looks dock widgets
    // up by objectName(), and the same one would silently replace this panel's entry
    // in that map once both are docked, so that one has to be new.
    copy->setWindowTitle(windowTitle());
    copy->setObjectName(uniqueName(objectName(), [this](const QString& name) {
        return dockManager() && dockManager()->findDockWidget(name) != nullptr;
    }));
    // So it is made again after a restart (ViewPanelRegistry::savePanels()).
    copy->setProperty(ViewPanelRegistry::kTypeProperty, property(ViewPanelRegistry::kTypeProperty));
    // A copy of a following panel follows too.
    if (m_followsCurrentStage) copy->setFollowsCurrentStage(true);

    if (isCollapsed()) {
        copy->collapseAt(collapsedArea(), dockManager());
        copy->raise();
    } else if (auto* area = dockAreaWidget()) {
        dockManager()->addDockWidgetTabToArea(copy, area);
    }
    return copy;
}

namespace {
ViewPanel::TabMenuExtension& tabMenuExtension() {
    static ViewPanel::TabMenuExtension extension;
    return extension;
}
}  // namespace

void ViewPanel::setTabMenuExtension(TabMenuExtension extension) { tabMenuExtension() = std::move(extension); }

void ViewPanel::extendTabMenu(QMenu* menu) {
    if (tabMenuExtension()) tabMenuExtension()(this, menu);
}

}  // namespace usdcc::ui
