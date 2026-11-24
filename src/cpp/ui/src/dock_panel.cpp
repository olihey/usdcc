#include "usdcc/ui/dock_panel.h"

#include "busy_overlay.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/options_menu_view.h"
#include "usdcc/ui/panel_maximizer.h"
#include "usdcc/ui/panel_menu_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"

#include <AutoHideDockContainer.h>
#include <DockAreaTabBar.h>
#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidgetTab.h>
#include <QtCore/qcompilerdetection.h>

#include <QBoxLayout>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSplitter>
#include <QThread>
#include <QToolButton>

#include <algorithm>
#include <utility>

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED

namespace usdcc::ui {

PanelMaximizer* DockPanel::maximizer() const {
    const ads::CDockManager* manager = dockManager();
    auto* window = manager ? qobject_cast<MainWindow*>(manager->window()) : nullptr;
    PanelMaximizer* found = window ? window->panelMaximizer() : nullptr;
    return found && found->canMaximize(this) ? found : nullptr;
}

DockPanel::DockPanel(const QString& title, QWidget* parent) : ads::CDockWidget(title, parent) {
    // ADS shows this panel as a tab (ads::CDockWidgetTab) in its dock area's
    // title bar, and that tab already has its own context menu
    // (Detach/Pin/Close/...) via CDockWidgetTab::contextMenuEvent(). An
    // event filter lets us extend that menu with our own entries without
    // subclassing CDockWidgetTab — ADS creates that internally via its own
    // component factory, with no hook to inject a custom subclass.
    // tabWidget() is already valid here: CDockWidget's constructor creates
    // it before returning.
    tabWidget()->installEventFilter(this);
}

PanelMenuButton* DockPanel::optionsButton() {
    if (m_optionsButton) return m_optionsButton;
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
    return m_optionsButton;
}

DockPanel::~DockPanel() {
    // Without busyChanged(): what listens may be half gone with the panel.
    if (m_busyOverlay) {
        blockSignals(true);
        m_busyOverlay->cancelAll();
    }
}

std::shared_ptr<Busy> DockPanel::beginBusy(const QString& text, BusyOptions options) {
    auto state = std::make_shared<Busy::State>();
    state->options = std::move(options);
    state->text = text;
    const auto add = [this, state]() {
        if (!m_busyOverlay) {
            m_busyOverlay = new BusyOverlay(this);
            // Closing the panel (its tab's ×, Close in a menu) cancels the work.
            connect(this, &ads::CDockWidget::closed, m_busyOverlay, &BusyOverlay::cancelAll);
        }
        m_busyOverlay->add(state);
    };
    // From another thread (a Python thread, say) the handle works at once, and the panel
    // takes it up on the UI thread.
    if (QThread::currentThread() == thread()) {
        add();
    } else {
        QMetaObject::invokeMethod(this, add, Qt::QueuedConnection);
    }
    return std::make_shared<Busy>(state);
}

bool DockPanel::isBusy() const { return m_busyOverlay && m_busyOverlay->hasWork(); }

QImage DockPanel::busySnapshot(QWidget* container) {
    QScreen* screen = container->screen();
    if (!screen) return {};
    return screen->grabWindow(container->winId()).toImage();
}

namespace {
ads::SideBarLocation sideBar(Qt::DockWidgetArea area) {
    switch (area) {
        case Qt::LeftDockWidgetArea:
            return ads::SideBarLeft;
        case Qt::RightDockWidgetArea:
            return ads::SideBarRight;
        case Qt::TopDockWidgetArea:
            return ads::SideBarTop;
        default:
            return ads::SideBarBottom;
    }
}
}  // namespace

void DockPanel::raise() {
    if (!dockManager()) {
        QWidget::raise();
    } else if (!isAutoHide()) {
        CDockWidget::raise();
    } else if (!isClosed()) {
        autoHideDockContainer()->collapseView(false);
    }
}

Qt::DockWidgetArea DockPanel::collapsedArea() const {
    if (!isAutoHide()) return Qt::NoDockWidgetArea;
    switch (autoHideLocation()) {
        case ads::SideBarLeft:
            return Qt::LeftDockWidgetArea;
        case ads::SideBarRight:
            return Qt::RightDockWidgetArea;
        case ads::SideBarTop:
            return Qt::TopDockWidgetArea;
        default:
            return Qt::BottomDockWidgetArea;
    }
}

void DockPanel::leavingTabBar(bool left) {
    if (!left) {
        if (dockAreaWidget()) m_tabBars << QPointer<QObject>(dockAreaWidget()->titleBar()->tabBar());
        return;
    }
    for (const QPointer<QObject>& tabBar : std::as_const(m_tabBars)) {
        if (tabBar) QCoreApplication::removePostedEvents(tabBar, QEvent::MetaCall);
    }
    m_tabBars.clear();
}

void DockPanel::collapseAt(Qt::DockWidgetArea area, ads::CDockManager* manager) {
    if (!manager || isFloating()) return;
    // Opened where it is first: ADS moves a closed one with its tab hidden. Nowhere (new, or
    // in no restored layout), it can't open before it has a place.
    const bool placed = dockAreaWidget() && dockContainer() == manager;
    if (placed && isClosed()) toggleView(true);
    if (!placed || !isAutoHide()) {
        leavingTabBar();
        if (placed) {
            setAutoHide(true, sideBar(area));
        } else {
            manager->addAutoHideDockWidget(sideBar(area), this);
        }
        leavingTabBar(true);
        // ADS takes the size it had docked, most of the window for one that had none.
        if (ads::CAutoHideDockContainer* container = autoHideDockContainer()) {
            const QSize window = manager->window()->size();
            const bool vertical = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
            container->setSize((vertical ? window.width() : window.height()) / 4);
        }
    } else if (autoHideLocation() != sideBar(area)) {
        autoHideDockContainer()->moveToNewSideBarLocation(sideBar(area));
    }
    if (isClosed()) toggleView(true);
    if (ads::CAutoHideDockContainer* container = autoHideDockContainer()) container->collapseView(true);
}

void DockPanel::leaveEdge() {
    if (!isAutoHide()) return;
    if (isClosed()) toggleView(true);
    leavingTabBar();
    setAutoHide(false);
    leavingTabBar(true);
}

void DockPanel::setContentWidget(QWidget* widget) {
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

QWidget* DockPanel::contentWidget() const { return m_content; }

bool DockPanel::hasOptionsPanel() {
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

void DockPanel::showOptionsPopup() {
    if (!m_optionsButton || !hasOptionsPanel() || m_pinned) return;
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

void DockPanel::setOptionsPinned(bool pinned) {
    if (!m_optionsHost || !m_optionsButton || pinned == m_pinned) return;
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

void DockPanel::notifyOptionsChanged() {
    if (m_menuView) m_menuView->scheduleRebuild();
}

void DockPanel::showEvent(QShowEvent* event) {
    CDockWidget::showEvent(event);
    // Once, when it first shows: by then it has the name its setting is saved under.
    if (m_restoredPin) return;
    m_restoredPin = true;
    if (appSettings().value(QStringLiteral("ViewPanel/optionsPinned/") + objectName(), false).toBool()) setOptionsPinned(true);
}

void DockPanel::populateContextMenu(QMenu* menu) {
    menu->addSeparator();
    if (PanelMaximizer* max = maximizer()) {
        menu->addAction(max->isMaximized(this) ? tr("Restore Layout") : tr("Maximize"), this,
                        [this, max]() { max->toggle(this); });
    }
}

namespace {
DockPanel::TabWorkspaceMenu& tabWorkspaceMenu() {
    static DockPanel::TabWorkspaceMenu factory;
    return factory;
}
}  // namespace

void DockPanel::setTabWorkspaceMenu(TabWorkspaceMenu factory) { tabWorkspaceMenu() = std::move(factory); }

bool DockPanel::eventFilter(QObject* watched, QEvent* event) {
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
        extendTabMenu(menu);
        menu->exec(menuEvent->globalPos());
        menu->deleteLater();
        return true;
    }
    // Instead of ADS's undocking, which stays for a floating panel; docked, the
    // tab's context menu and dragging it still do that.
    if (watched == tabWidget() && event->type() == QEvent::MouseButtonDblClick &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        if (PanelMaximizer* max = maximizer()) {
            max->toggle(this);
            return true;
        }
    }
    return ads::CDockWidget::eventFilter(watched, event);
}

}  // namespace usdcc::ui

QT_WARNING_POP
