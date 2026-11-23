#include "broadcast_panel.h"

#include "broadcast.h"

#include "usdcc/peers/room.h"
#include "usdcc/ui/peers_controller.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

namespace usdcc::broadcast {

namespace {
constexpr int kIdRole = Qt::UserRole;
}

BroadcastPanel::BroadcastPanel(usdcc::ui::PeersController* peers, QWidget* parent)
    : SidePanel(tr("Broadcast"), parent), m_peers(peers) {
    setObjectName(QStringLiteral("ViewportBroadcastPanel"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_status = new QLabel(content);
    m_status->setObjectName(QStringLiteral("broadcastStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto* linkRow = new QHBoxLayout;
    m_link = new QLineEdit(content);
    m_link->setObjectName(QStringLiteral("broadcastLink"));
    m_link->setReadOnly(true);
    m_link->setToolTip(tr("Whoever opens this link in a browser watches the viewport"));
    m_copy = new QPushButton(tr("Copy"), content);
    m_copy->setObjectName(QStringLiteral("broadcastCopy"));
    m_stop = new QPushButton(tr("Stop"), content);
    m_stop->setObjectName(QStringLiteral("broadcastStop"));
    linkRow->addWidget(m_link, 1);
    linkRow->addWidget(m_copy);
    linkRow->addWidget(m_stop);
    layout->addLayout(linkRow);

    auto* watchersLabel = new QLabel(tr("Watchers"), content);
    layout->addWidget(watchersLabel);
    m_watchers = new QListWidget(content);
    m_watchers->setObjectName(QStringLiteral("broadcastWatchers"));
    m_watchers->setMaximumHeight(110);
    layout->addWidget(m_watchers);
    auto* watcherRow = new QHBoxLayout;
    m_allow = new QPushButton(tr("Allow Navigating"), content);
    m_allow->setObjectName(QStringLiteral("broadcastAllow"));
    m_allow->setToolTip(tr("The selected watcher moves this viewport's camera, instead of whoever did"));
    m_takeBack = new QPushButton(tr("Take Back"), content);
    m_takeBack->setObjectName(QStringLiteral("broadcastTakeBack"));
    m_takeBack->setToolTip(tr("Ends the watcher's navigating"));
    watcherRow->addWidget(m_allow);
    watcherRow->addWidget(m_takeBack);
    m_remove = new QPushButton(tr("Remove"), content);
    m_remove->setObjectName(QStringLiteral("broadcastRemove"));
    m_remove->setToolTip(tr("Closes the selected watcher's page: they can come back with the link"));
    watcherRow->addStretch(1);
    watcherRow->addWidget(m_remove);
    layout->addLayout(watcherRow);

    auto* chatLabel = new QLabel(tr("Chat"), content);
    layout->addWidget(chatLabel);
    // Text only, never HTML (§9).
    m_chat = new QPlainTextEdit(content);
    m_chat->setObjectName(QStringLiteral("broadcastChat"));
    m_chat->setReadOnly(true);
    layout->addWidget(m_chat, 1);
    m_say = new QLineEdit(content);
    m_say->setObjectName(QStringLiteral("broadcastSay"));
    m_say->setPlaceholderText(tr("Message the watchers"));
    m_say->setMaxLength(2000);
    layout->addWidget(m_say);
    setWidget(content);

    connect(m_copy, &QPushButton::clicked, this, &BroadcastPanel::copyRequested);
    connect(m_stop, &QPushButton::clicked, this, &BroadcastPanel::stopRequested);
    connect(m_remove, &QPushButton::clicked, this, [this] { removeSelectedWatcher(); });
    connect(m_allow, &QPushButton::clicked, this, [this] { allowSelectedWatcher(); });
    connect(m_takeBack, &QPushButton::clicked, this, [this] {
        if (m_broadcast) m_broadcast->takeBack();
    });
    connect(m_watchers, &QListWidget::itemSelectionChanged, this, &BroadcastPanel::updateState);
    connect(m_say, &QLineEdit::returnPressed, this, [this] {
        if (say(m_say->text())) m_say->clear();
    });
    if (m_peers) {
        connect(m_peers, &usdcc::ui::PeersController::chatAdded, this,
                [this](usdcc::peers::Room* room, const QString& with, const usdcc::ui::PeersController::ChatLine& line) {
                    if (!m_broadcast || room != m_broadcast->room() || !with.isEmpty() || !line.to.isEmpty()) return;
                    appendChatLine(line.from.isValid() ? line.from.name : QString(), line.text);
                });
        connect(m_peers, &usdcc::ui::PeersController::chatReset, this, [this](usdcc::peers::Room* room) {
            if (m_broadcast && room == m_broadcast->room()) reloadChat();
        });
    }
    updateState();
}

void BroadcastPanel::setBroadcast(Broadcast* broadcast) {
    for (const QMetaObject::Connection& connection : std::as_const(m_connections)) disconnect(connection);
    m_connections.clear();
    m_broadcast = broadcast;
    if (broadcast) {
        m_connections.append(connect(broadcast, &Broadcast::watchersChanged, this, &BroadcastPanel::updateWatchers));
        m_connections.append(connect(broadcast, &Broadcast::navigatorChanged, this, &BroadcastPanel::updateWatchers));
        m_connections.append(connect(broadcast, &Broadcast::stopped, this, [this](const QString& reason) {
            setBroadcast(nullptr);
            m_status->setText(reason.isEmpty() ? tr("The broadcast ended.") : tr("The broadcast ended: %1").arg(reason));
        }));
        m_connections.append(connect(broadcast, &QObject::destroyed, this, [this] { setBroadcast(nullptr); }));
        if (FrameSource* source = broadcast->source()) {
            m_connections.append(connect(source, &FrameSource::titleChanged, this, &BroadcastPanel::updateState));
        }
    }
    reloadChat();
    updateWatchers();
}

bool BroadcastPanel::removeSelectedWatcher() {
    QListWidgetItem* item = m_watchers->currentItem();
    if (!m_broadcast || !m_broadcast->room() || !item) return false;
    m_broadcast->room()->remove(item->data(kIdRole).toString(), tr("The host removed you from the broadcast"));
    return true;
}

bool BroadcastPanel::allowSelectedWatcher() {
    QListWidgetItem* item = m_watchers->currentItem();
    return m_broadcast && item && m_broadcast->allowNavigating(item->data(kIdRole).toString());
}

bool BroadcastPanel::say(const QString& text) {
    if (!m_peers || !m_broadcast || !m_broadcast->room() || text.trimmed().isEmpty()) return false;
    m_peers->say(m_broadcast->room(), text);
    return true;
}

void BroadcastPanel::updateWatchers() {
    const QString selected = m_watchers->currentItem() ? m_watchers->currentItem()->data(kIdRole).toString() : QString();
    m_watchers->clear();
    if (m_broadcast) {
        for (const usdcc::peers::Peer& watcher : m_broadcast->watchers()) {
            QString text = watcher.name;
            if (watcher.id == m_broadcast->navigator()) {
                text = tr("%1 (navigating)").arg(watcher.name);
            } else if (m_broadcast->asking().contains(watcher.id)) {
                text = tr("%1 (asks to navigate)").arg(watcher.name);
            }
            auto* item = new QListWidgetItem(text, m_watchers);
            item->setData(kIdRole, watcher.id);
            if (watcher.id == selected) m_watchers->setCurrentItem(item);
        }
    }
    updateState();
}

void BroadcastPanel::updateState() {
    const bool live = m_broadcast && m_broadcast->isLive();
    if (live) {
        const QString title = m_broadcast->source() ? m_broadcast->source()->title() : QString();
        const int watchers = m_watchers->count();
        m_status->setText(watchers == 0 ? tr("Broadcasting %1. Nobody is watching yet.").arg(title)
                                        : tr("Broadcasting %1 to %n watcher(s).", nullptr, watchers).arg(title));
        m_link->setText(m_broadcast->link().toString());
    } else {
        if (!m_broadcast && m_status->text().isEmpty()) {
            m_status->setText(tr("Not broadcasting. Start Broadcast is in a viewport's menu, under Broadcast."));
        }
        m_link->clear();
    }
    m_copy->setEnabled(live);
    m_stop->setEnabled(live);
    m_remove->setEnabled(live && m_watchers->currentItem());
    m_allow->setEnabled(live && m_watchers->currentItem() &&
                        m_watchers->currentItem()->data(kIdRole).toString() != m_broadcast->navigator());
    m_takeBack->setEnabled(live && !m_broadcast->navigator().isEmpty());
    m_say->setEnabled(live && m_peers);
}

void BroadcastPanel::reloadChat() {
    m_chat->clear();
    if (!m_peers || !m_broadcast || !m_broadcast->room()) return;
    for (const usdcc::ui::PeersController::ChatLine& line : m_peers->chat(m_broadcast->room())) {
        if (!line.to.isEmpty()) continue;
        appendChatLine(line.from.isValid() ? line.from.name : QString(), line.text);
    }
}

void BroadcastPanel::appendChatLine(const QString& who, const QString& text) {
    // What the room itself says ("Anna joined") without a name.
    m_chat->appendPlainText(who.isEmpty() ? text : QStringLiteral("%1: %2").arg(who, text));
    m_chat->verticalScrollBar()->setValue(m_chat->verticalScrollBar()->maximum());
}

}  // namespace usdcc::broadcast
