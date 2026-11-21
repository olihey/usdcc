#include "usdcc/usd/render/swarm_panel.h"

#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/ui/peers_side_panel.h"
#include "usdcc/usd/render/render_job.h"
#include "usdcc/usd/render/render_network.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace usdcc::render {

using usdcc::peers::FoundRoom;
using usdcc::peers::Room;
using usdcc::peers::RoomType;

namespace {

enum Column { NameColumn, TakesColumn, RenderersColumn, StatusColumn, ProgressColumn, TimeColumn };

constexpr int kKeyRole = Qt::UserRole;
constexpr int kRoomRole = Qt::UserRole + 1;       // a room's row: the Room*
constexpr int kFoundRole = Qt::UserRole + 2;      // a found room's row
constexpr int kFractionRole = Qt::UserRole + 3;   // -1: no bar; 0..1; -2: busy, not known
constexpr int kStartedRole = Qt::UserRole + 4;    // ms since the epoch; 0: none

QString roomKeyOf(Room* room) { return QStringLiteral("room|%1").arg(quintptr(room)); }

QString elapsed(qint64 startedMs) {
    if (startedMs <= 0) return {};
    const qint64 seconds = std::max<qint64>(0, (QDateTime::currentMSecsSinceEpoch() - startedMs) / 1000);
    if (seconds >= 3600) {
        return QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg((seconds / 60) % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

// The Progress column: a bar while a member renders.
class ProgressDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const QVariant value = index.data(kFractionRole);
        if (!value.isValid() || value.toDouble() == -1.0) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem item = option;
        initStyleOption(&item, index);
        item.text.clear();
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &item, painter, item.widget);
        QStyleOptionProgressBar bar;
        bar.rect = option.rect.adjusted(2, 3, -2, -3);
        bar.minimum = 0;
        bar.maximum = 1000;
        const double fraction = value.toDouble();
        bar.progress = fraction < 0 ? 0 : int(fraction * 1000.0);
        if (fraction < 0) bar.maximum = 0;  // busy, how far not known
        bar.textVisible = fraction >= 0;
        bar.text = fraction >= 0 ? QStringLiteral("%1%").arg(int(fraction * 100.0 + 0.5)) : QString();
        bar.state = option.state | QStyle::State_Horizontal;
        bar.palette = option.palette;
        QApplication::style()->drawControl(QStyle::CE_ProgressBar, &bar, painter, item.widget);
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setWidth(std::max(size.width(), 90));
        return size;
    }
};

}  // namespace

SwarmSidePanel::SwarmSidePanel(RenderNetwork* network, usdcc::ui::PeersController* controller, QWidget* parent)
    : usdcc::ui::SidePanel(tr("Swarm"), parent), m_network(network), m_controller(controller) {
    setObjectName(QStringLiteral("Swarm"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* buttons = new QHBoxLayout();
    m_host = new QPushButton(tr("Host…"), content);
    m_host->setObjectName(QStringLiteral("swarmHost"));
    m_host->setToolTip(tr("Host a render queue room: instances in it can send each other render jobs"));
    m_join = new QPushButton(tr("Join…"), content);
    m_join->setObjectName(QStringLiteral("swarmJoin"));
    m_join->setToolTip(tr("Join a render queue by its invite link or address"));
    m_leave = new QPushButton(tr("Leave"), content);
    m_leave->setObjectName(QStringLiteral("swarmLeave"));
    m_copyLink = new QPushButton(tr("Copy Link"), content);
    m_copyLink->setObjectName(QStringLiteral("swarmCopyLink"));
    m_copyLink->setToolTip(tr("Copy the invite link of the room this instance hosts"));
    for (QPushButton* button : {m_host, m_join, m_leave, m_copyLink}) buttons->addWidget(button);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    m_tree = new QTreeWidget(content);
    m_tree->setObjectName(QStringLiteral("swarmTree"));
    m_tree->setHeaderLabels({tr("Room / instance"), tr("Takes jobs"), tr("Renderers"), tr("Status"), tr("Progress"), tr("Time")});
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(StatusColumn, QHeaderView::Stretch);
    m_tree->setItemDelegateForColumn(ProgressColumn, new ProgressDelegate(m_tree));
    layout->addWidget(m_tree, 1);
    setWidget(content);

    m_clock = new QTimer(this);
    m_clock->setInterval(1000);
    connect(m_clock, &QTimer::timeout, this, &SwarmSidePanel::updateTimes);

    connect(m_host, &QPushButton::clicked, this, [this]() {
        usdcc::ui::hostRoomDialog(m_controller, this, RoomType::RenderQueue);
    });
    connect(m_join, &QPushButton::clicked, this, [this]() {
        usdcc::ui::joinRoomDialog(m_controller, this, RoomType::RenderQueue);
    });
    connect(m_leave, &QPushButton::clicked, this, [this]() {
        if (Room* room = selectedRoom()) room->close();
    });
    connect(m_copyLink, &QPushButton::clicked, this, [this]() {
        if (Room* room = selectedRoom(); room && room->isHost()) QApplication::clipboard()->setText(room->link());
    });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &SwarmSidePanel::updateButtons);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item && item->data(0, kFoundRole).isValid()) {
            usdcc::ui::joinFoundRoom(m_controller, this, item->data(0, kFoundRole).value<FoundRoom>());
        }
    });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (m_updating || column != TakesColumn || !m_network) return;
        Room* room = static_cast<Room*>(item->data(0, kRoomRole).value<QObject*>());
        if (room) m_network->setTakesJobs(room, item->checkState(TakesColumn) == Qt::Checked);
    });
    if (m_network) {
        connect(m_network, &RenderNetwork::nodesChanged, this, &SwarmSidePanel::refresh);
        connect(m_network, &RenderNetwork::roomsChanged, this, &SwarmSidePanel::refresh);
    }
    if (m_controller) {
        connect(m_controller->peers(), &usdcc::peers::Peers::foundRoomsChanged, this, &SwarmSidePanel::refresh);
    }
    refresh();
}

SwarmSidePanel::~SwarmSidePanel() = default;

void SwarmSidePanel::showEvent(QShowEvent* event) {
    usdcc::ui::SidePanel::showEvent(event);
    if (m_controller) m_controller->setSwarmShown(true);
    m_clock->start();
    refresh();
}

void SwarmSidePanel::hideEvent(QHideEvent* event) {
    usdcc::ui::SidePanel::hideEvent(event);
    if (m_controller) m_controller->setSwarmShown(false);
    m_clock->stop();
}

QTreeWidgetItem* SwarmSidePanel::itemFor(const QString& key, QTreeWidgetItem* parent, int index) {
    QTreeWidgetItem* item = m_items.value(key);
    if (!item) {
        item = new QTreeWidgetItem();
        item->setData(0, kKeyRole, key);
        m_items.insert(key, item);
    }
    // In its place, under its parent.
    QTreeWidgetItem* current = parent ? (index < parent->childCount() ? parent->child(index) : nullptr)
                                      : (index < m_tree->topLevelItemCount() ? m_tree->topLevelItem(index) : nullptr);
    if (current != item) {
        if (QTreeWidgetItem* old = item->parent()) {
            old->removeChild(item);
        } else if (const int top = m_tree->indexOfTopLevelItem(item); top >= 0) {
            m_tree->takeTopLevelItem(top);
        }
        if (parent) {
            parent->insertChild(index, item);
        } else {
            m_tree->insertTopLevelItem(index, item);
        }
    }
    return item;
}

void SwarmSidePanel::fillNode(QTreeWidgetItem* item, const RenderNode& node, bool self) {
    item->setText(NameColumn, self ? tr("%1 (this instance)").arg(node.peer.name) : node.peer.name);
    item->setToolTip(NameColumn, node.peer.app);
    item->setText(TakesColumn, node.node ? tr("Yes") : tr("No"));
    QStringList renderers;
    for (const RenderNode::Renderer& r : node.renderers) {
        renderers.append(QString::fromStdString(r.name) + (r.tiles ? QStringLiteral(" ▦") : QString()));
    }
    item->setText(RenderersColumn, renderers.isEmpty() ? tr("None") : renderers.join(QStringLiteral(", ")));
    item->setToolTip(RenderersColumn, tr("▦: renders tiles"));
    if (node.current) {
        const RenderNode::Current& c = *node.current;
        QString what = c.scene.isEmpty() ? c.title : c.scene;
        QString status = tr("Rendering %1, frame %2").arg(what, QString::fromStdString(frameText(c.frame)));
        if (!c.from.isEmpty()) status += tr(" for %1").arg(c.from);
        if (!c.renderer.isEmpty()) status += tr(" · %1").arg(c.renderer);
        item->setText(StatusColumn, status);
        item->setToolTip(StatusColumn, c.title);
        item->setData(ProgressColumn, kFractionRole, c.fraction >= 0 ? c.fraction : -2.0);
        item->setData(TimeColumn, kStartedRole, c.startedMs);
    } else {
        item->setText(StatusColumn, node.busy ? tr("Busy") : node.node ? tr("Idle") : tr("Idle · not taking jobs"));
        item->setToolTip(StatusColumn, {});
        item->setData(ProgressColumn, kFractionRole, -1.0);
        item->setData(TimeColumn, kStartedRole, qint64(0));
    }
    item->setText(TimeColumn, elapsed(item->data(TimeColumn, kStartedRole).toLongLong()));
}

void SwarmSidePanel::refresh() {
    if (!m_network || m_updating) return;
    m_updating = true;
    QSet<QString> seen;
    int top = 0;
    const std::vector<RenderNode> nodes = m_network->nodes();
    for (Room* room : m_network->rooms()) {
        const QString key = roomKeyOf(room);
        seen.insert(key);
        QTreeWidgetItem* item = itemFor(key, nullptr, top++);
        item->setData(0, kRoomRole, QVariant::fromValue<QObject*>(room));
        const QString state = room->state() == Room::State::Open           ? QString()
                              : room->state() == Room::State::Reconnecting ? tr(" · reconnecting")
                                                                           : tr(" · connecting");
        item->setText(NameColumn, (room->name().isEmpty() ? tr("(connecting)") : room->name()) + state);
        item->setToolTip(NameColumn, room->isHost() ? tr("This instance hosts it") : tr("Hosted by %1").arg(room->hostPeer().name));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(TakesColumn, m_network->takesJobs(room) ? Qt::Checked : Qt::Unchecked);
        item->setText(TakesColumn, tr("This instance"));
        item->setToolTip(TakesColumn, tr("Whether this instance renders jobs from the others in this room"));
        item->setText(RenderersColumn, tr("%n instance(s)", nullptr, int(room->peers().size())));
        item->setText(StatusColumn, room->isHost() ? tr("Hosted here") : tr("Hosted by %1").arg(room->hostPeer().name));
        QFont bold = item->font(NameColumn);
        bold.setBold(true);
        item->setFont(NameColumn, bold);
        // This instance first, then the others as they say.
        int child = 0;
        const QString selfKey = key + QStringLiteral("|self");
        seen.insert(selfKey);
        fillNode(itemFor(selfKey, item, child++), m_network->self(room), true);
        for (const RenderNode& node : nodes) {
            if (node.room != room) continue;
            const QString nodeKey = key + u'|' + node.peer.id;
            seen.insert(nodeKey);
            fillNode(itemFor(nodeKey, item, child++), node, false);
        }
        // Members that say nothing (not usdcc, or an older one).
        for (const usdcc::peers::Peer& peer : room->peers()) {
            if (peer.id == room->me().id) continue;
            const QString nodeKey = key + u'|' + peer.id;
            if (seen.contains(nodeKey)) continue;
            seen.insert(nodeKey);
            QTreeWidgetItem* member = itemFor(nodeKey, item, child++);
            member->setText(NameColumn, peer.name);
            member->setText(StatusColumn, tr("Says nothing about rendering"));
        }
        if (!item->data(0, Qt::UserRole + 10).toBool()) {
            item->setExpanded(true);  // the first time only
            item->setData(0, Qt::UserRole + 10, true);
        }
    }
    // Render queues on this machine and network.
    const QList<FoundRoom> found = m_controller ? m_controller->peers()->foundRooms(RoomType::RenderQueue) : QList<FoundRoom>();
    if (!found.isEmpty()) {
        const QString groupKey = QStringLiteral("found");
        seen.insert(groupKey);
        QTreeWidgetItem* group = itemFor(groupKey, nullptr, top++);
        group->setText(NameColumn, tr("Found: double-click to join"));
        group->setFirstColumnSpanned(true);
        int child = 0;
        for (const FoundRoom& room : found) {
            const QString key = QStringLiteral("found|") + room.roomId();
            seen.insert(key);
            QTreeWidgetItem* item = itemFor(key, group, child++);
            item->setData(0, kFoundRole, QVariant::fromValue(room));
            item->setText(NameColumn, room.announcement.name);
            item->setText(StatusColumn, tr("Hosted by %1%2%3")
                                            .arg(room.announcement.hostName,
                                                 room.where == FoundRoom::Where::ThisMachine ? tr(" on this machine") : QString(),
                                                 room.announcement.password ? tr(" · password") : QString()));
        }
        if (!group->data(0, Qt::UserRole + 10).toBool()) {
            group->setExpanded(true);
            group->setData(0, Qt::UserRole + 10, true);
        }
    }
    if (m_network->rooms().isEmpty() && found.isEmpty()) {
        const QString key = QStringLiteral("empty");
        seen.insert(key);
        QTreeWidgetItem* item = itemFor(key, nullptr, top++);
        item->setText(NameColumn, tr("No render queues yet: Host… one, or Join… one by its link."));
        item->setFirstColumnSpanned(true);
        item->setFlags(Qt::ItemIsEnabled);
    }
    // What went: every one taken out first, so that deleting one doesn't delete another twice.
    QList<QTreeWidgetItem*> gone;
    for (auto it = m_items.begin(); it != m_items.end();) {
        if (!seen.contains(it.key())) {
            gone.append(it.value());
            it = m_items.erase(it);
        } else {
            ++it;
        }
    }
    for (QTreeWidgetItem* item : gone) {
        if (QTreeWidgetItem* parent = item->parent()) {
            parent->removeChild(item);
        } else if (const int index = m_tree->indexOfTopLevelItem(item); index >= 0) {
            m_tree->takeTopLevelItem(index);
        }
    }
    qDeleteAll(gone);
    m_updating = false;
    updateButtons();
}

void SwarmSidePanel::updateTimes() {
    for (QTreeWidgetItem* item : std::as_const(m_items)) {
        const qint64 started = item->data(TimeColumn, kStartedRole).toLongLong();
        if (started > 0) item->setText(TimeColumn, elapsed(started));
    }
}

Room* SwarmSidePanel::selectedRoom() const {
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    for (QTreeWidgetItem* item = selected.isEmpty() ? nullptr : selected.first(); item; item = item->parent()) {
        if (QObject* room = item->data(0, kRoomRole).value<QObject*>()) return static_cast<Room*>(room);
    }
    return nullptr;
}

void SwarmSidePanel::updateButtons() {
    Room* room = selectedRoom();
    m_leave->setEnabled(room != nullptr);
    m_leave->setText(room && room->isHost() ? tr("Close") : tr("Leave"));
    m_copyLink->setEnabled(room && room->isHost());
}

}  // namespace usdcc::render
