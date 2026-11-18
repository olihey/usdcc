#include "usdcc/ui/view_panel.h"

#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/options_menu_view.h"
#include "usdcc/ui/panel_maximizer.h"
#include "usdcc/ui/panel_menu_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
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
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QToolButton>

#include <algorithm>
#include <utility>

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED

namespace usdcc::ui {

namespace {
// The main window's, when `panel` can be maximized in it right now.
PanelMaximizer* maximizerFor(const ViewPanel* panel) {
    const ads::CDockManager* manager = panel->dockManager();
    auto* window = manager ? qobject_cast<MainWindow*>(manager->window()) : nullptr;
    PanelMaximizer* maximizer = window ? window->panelMaximizer() : nullptr;
    return maximizer && maximizer->canMaximize(panel) ? maximizer : nullptr;
}
}  // namespace

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

    m_optionsButton = new PanelMenuButton(tr("Panel options"), [this](QMenu* menu) {
        menu->addAction(tr("Pin to Side"), this, [this]() { setOptionsPinned(true); });
        menu->addSeparator();
        populateOptionsMenu(menu);
    });
    // Pinned, the button unpins. With an options panel, it opens that, not the menu.
    m_optionsButton->setClickHandler([this]() {
        if (m_pinned) {
            setOptionsPinned(false);
            return true;
        }
        if (!hasOptionsPanel()) return false;
        showOptionsPopup();
        return true;
    });

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
    return ads::CDockWidget::event(event);
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
    toolbarLayout->addWidget(m_optionsButton);
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

void ViewPanel::setContentWidget(QWidget* widget) {
    // Beside an options panel, which stays hidden until there is one.
    if (!m_split) {
        m_split = new QSplitter(Qt::Horizontal, this);
        m_split->setObjectName(QStringLiteral("viewPanelSplit"));
        m_split->setChildrenCollapsible(false);
        m_optionsHost = new QWidget(m_split);
        m_optionsHost->setObjectName(QStringLiteral("viewPanelOptions"));
        m_optionsHost->setAttribute(Qt::WA_StyledBackground);
        auto* layout = new QVBoxLayout(m_optionsHost);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        // A header saying what it is, with the way to unpin it.
        auto* header = new QWidget(m_optionsHost);
        header->setObjectName(QStringLiteral("panelToolbar"));
        header->setAttribute(Qt::WA_StyledBackground);
        auto* headerLayout = new QHBoxLayout(header);
        usdcc::ui::scaled::margins(headerLayout, 8, 4, 4, 4);
        headerLayout->addWidget(new QLabel(tr("Options"), header), 1);
        auto* unpin = new QToolButton(header);
        unpin->setObjectName(QStringLiteral("unpinOptions"));
        unpin->setText(tr("Unpin"));
        unpin->setAutoRaise(true);
        unpin->setToolTip(tr("Put the options back behind the options button"));
        connect(unpin, &QToolButton::clicked, this, [this]() { setOptionsPinned(false); });
        headerLayout->addWidget(unpin);
        layout->addWidget(header);
        m_optionsBody = new QWidget(m_optionsHost);
        auto* bodyLayout = new QVBoxLayout(m_optionsBody);
        bodyLayout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_optionsBody, 1);
        m_optionsHost->hide();
        m_split->addWidget(m_optionsHost);
        m_split->setStretchFactor(0, 0);
        setWidget(m_split);
    }
    if (m_content) {
        m_content->hide();
        m_content->deleteLater();
    }
    m_content = widget;
    m_split->addWidget(widget);
    m_split->setStretchFactor(1, 1);
}

QWidget* ViewPanel::contentWidget() const { return m_content; }

bool ViewPanel::hasOptionsPanel() {
    if (m_optionsPanel == OptionsPanel::NotAsked) {
        m_optionsWidget = m_split ? createOptionsPanel() : nullptr;
        m_optionsPanel = m_optionsWidget ? OptionsPanel::Made : OptionsPanel::None;
        if (m_optionsWidget) {
            m_optionsWidget->setParent(m_optionsBody);
            m_optionsWidget->hide();
        }
    }
    return m_optionsPanel == OptionsPanel::Made;
}

void ViewPanel::showOptionsPopup() {
    if (!hasOptionsPanel() || m_pinned) return;
    if (!m_popup) {
        // Parented to the top-level window, as the options menu is: in a viewport panel this
        // panel is a native child, which a popup can't have as its transient parent.
        m_popup = new QFrame(window(), Qt::Popup);
        m_popup->setObjectName(QStringLiteral("viewPanelOptionsPopup"));
        static_cast<QFrame*>(m_popup)->setFrameShape(QFrame::StyledPanel);
        auto* layout = new QVBoxLayout(m_popup);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto* header = new QWidget(m_popup);
        header->setObjectName(QStringLiteral("panelToolbar"));
        header->setAttribute(Qt::WA_StyledBackground);
        auto* headerLayout = new QHBoxLayout(header);
        usdcc::ui::scaled::margins(headerLayout, 8, 4, 4, 4);
        headerLayout->addWidget(new QLabel(tr("Options"), header), 1);
        auto* pin = new QToolButton(header);
        pin->setObjectName(QStringLiteral("pinOptions"));
        pin->setText(tr("Pin to Side"));
        pin->setAutoRaise(true);
        pin->setToolTip(tr("Keep the options beside the panel"));
        connect(pin, &QToolButton::clicked, this, [this]() {
            m_popup->close();
            setOptionsPinned(true);
        });
        headerLayout->addWidget(pin);
        layout->addWidget(header);
    }
    m_popup->layout()->addWidget(m_optionsWidget);
    m_optionsWidget->show();
    m_popup->adjustSize();
    m_popup->resize(std::max(m_popup->width(), usdcc::ui::scaled::pixels(240)), m_popup->height());
    m_popup->move(m_optionsButton->mapToGlobal(QPoint(0, m_optionsButton->height())));
    m_popup->show();
}

void ViewPanel::setOptionsPinned(bool pinned) {
    if (!m_optionsHost || pinned == m_pinned) return;
    m_pinned = pinned;
    if (pinned) {
        if (hasOptionsPanel()) {
            if (m_popup) m_popup->hide();
            m_optionsBody->layout()->addWidget(m_optionsWidget);
            m_optionsWidget->show();
        } else {
            m_menuView = new OptionsMenuView([this](QMenu* menu) { populateOptionsMenu(menu); }, m_optionsBody);
            m_optionsBody->layout()->addWidget(m_menuView);
        }
        m_optionsHost->show();
        const int width = std::max(m_optionsHost->sizeHint().width(), usdcc::ui::scaled::pixels(220));
        m_split->setSizes({width, std::max(1, m_split->width() - width)});
    } else {
        m_optionsHost->hide();
        if (m_menuView) {
            m_menuView->hide();
            m_menuView->deleteLater();
            m_menuView = nullptr;
        }
    }
    m_optionsButton->setCheckable(true);
    m_optionsButton->setChecked(pinned);
    m_optionsButton->setToolTip(pinned ? tr("Unpin the options") : tr("Panel options"));
    QSettings settings = appSettings();
    settings.setValue(QStringLiteral("ViewPanel/optionsPinned/") + objectName(), pinned);
}

void ViewPanel::notifyOptionsChanged() {
    if (m_menuView) m_menuView->scheduleRebuild();
}

void ViewPanel::showEvent(QShowEvent* event) {
    CDockWidget::showEvent(event);
    // Once, when it first shows: by then it has the name its setting is saved under.
    if (m_restoredPin) return;
    m_restoredPin = true;
    if (appSettings().value(QStringLiteral("ViewPanel/optionsPinned/") + objectName(), false).toBool()) setOptionsPinned(true);
}

void ViewPanel::populateContextMenu(QMenu* menu) {
    menu->addSeparator();
    if (PanelMaximizer* maximizer = maximizerFor(this)) {
        menu->addAction(maximizer->isMaximized(this) ? tr("Restore Layout") : tr("Maximize"), this,
                        [this, maximizer]() { maximizer->toggle(this); });
    }
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

    if (auto* area = dockAreaWidget()) {
        dockManager()->addDockWidgetTabToArea(copy, area);
    }
    return copy;
}

namespace {
ViewPanel::TabMenuExtension& tabMenuExtension() {
    static ViewPanel::TabMenuExtension extension;
    return extension;
}
ViewPanel::TabWorkspaceMenu& tabWorkspaceMenu() {
    static ViewPanel::TabWorkspaceMenu factory;
    return factory;
}
}  // namespace

void ViewPanel::setTabMenuExtension(TabMenuExtension extension) { tabMenuExtension() = std::move(extension); }
void ViewPanel::setTabWorkspaceMenu(TabWorkspaceMenu factory) { tabWorkspaceMenu() = std::move(factory); }

bool ViewPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tabWidget() && event->type() == QEvent::ContextMenu) {
        auto* menuEvent = static_cast<QContextMenuEvent*>(event);
        QMenu* menu = tabWidget()->buildContextMenu(nullptr);
        // ADS's Detach (and Pin) and Close in one group, without its divider;
        // then the workspaces.
        for (QAction* action : menu->actions()) {
            if (action->isSeparator()) menu->removeAction(action);
        }
        if (tabWorkspaceMenu()) {
            if (QMenu* workspaces = tabWorkspaceMenu()(menu)) menu->addMenu(workspaces);
        }
        populateContextMenu(menu);
        if (tabMenuExtension()) tabMenuExtension()(this, menu);
        menu->exec(menuEvent->globalPos());
        menu->deleteLater();
        return true;
    }
    // Instead of ADS's undocking, which stays for a floating panel; docked, the
    // tab's context menu and dragging it still do that.
    if (watched == tabWidget() && event->type() == QEvent::MouseButtonDblClick &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        if (PanelMaximizer* maximizer = maximizerFor(this)) {
            maximizer->toggle(this);
            return true;
        }
    }
    return ads::CDockWidget::eventFilter(watched, event);
}

}  // namespace usdcc::ui

QT_WARNING_POP
