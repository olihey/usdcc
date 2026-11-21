#include "usdcc/ui/peer_rooms_debug_dialog.h"

#include "usdcc/peers/channel.h"
#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/peers/state.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace usdcc::ui {

using namespace usdcc::peers;

namespace {

constexpr int kKeptMessages = 200;  // Per channel
constexpr int kKeyRole = Qt::UserRole + 1;

// An item's key: "<room address as number>|<what>|<name>".
QString keyOf(Room* room, const QString& what, const QString& name = {}) {
    return QString::number(quintptr(room)) + u'|' + what + u'|' + name;
}

QString stateName(Room::State state) {
    switch (state) {
    case Room::State::Connecting: return QStringLiteral("connecting");
    case Room::State::Open: return QStringLiteral("open");
    case Room::State::Reconnecting: return QStringLiteral("reconnecting");
    case Room::State::Closed: return QStringLiteral("closed");
    }
    return {};
}

QJsonObject peerObject(const Peer& peer) {
    return {{"id", peer.id},       {"name", peer.name},         {"kind", kindName(peer.kind)},
            {"app", peer.app},     {"color", peer.color},       {"fingerprint", peer.fingerprint},
            {"instance", peer.instance}};
}

QString shortJson(const QJsonValue& value) {
    QString text = value.isObject()  ? QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact))
                   : value.isArray() ? QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact))
                                     : value.toVariant().toString();
    return text.size() > 120 ? text.left(119) + u'…' : text;
}

}  // namespace

PeerRoomsDebugDialog::PeerRoomsDebugDialog(Peers* peers, QWidget* parent) : QDialog(parent), m_peers(peers) {
    setWindowTitle(tr("Peer Rooms"));
    setObjectName(QStringLiteral("peerRoomsDebugDialog"));
    resize(1000, 640);
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(tr("Every room this instance is in. Channels show what arrived here since this window "
                               "opened; maps and per-peer states show their current values."),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* splitter = new QSplitter(this);
    m_tree = new QTreeWidget(splitter);
    m_tree->setObjectName(QStringLiteral("peerRoomsTree"));
    m_tree->setHeaderLabels({tr("Item"), tr("Value")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_text = new QPlainTextEdit(splitter);
    m_text->setObjectName(QStringLiteral("peerRoomsJson"));
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    m_text->setFont(mono);
    splitter->addWidget(m_tree);
    splitter->addWidget(m_text);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] { showSelected(); });
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, [this] { refresh(); });
    m_timer->start();
    if (m_peers) {
        connect(m_peers, &Peers::roomAdded, this, [this] { refresh(); });
        connect(m_peers, &Peers::roomRemoved, this, [this] { refresh(); });
    }
    refresh();
}

PeerRoomsDebugDialog::~PeerRoomsDebugDialog() = default;

void PeerRoomsDebugDialog::watchChannels(Room* room) {
    for (const QString& name : room->channelNames()) {
        Channel* channel = room->findChannel(name);
        if (!channel || m_watched.contains(channel)) continue;
        m_watched.insert(channel);
        connect(channel, &QObject::destroyed, this, [this, channel] {
            m_watched.remove(channel);
            m_messages.remove(channel);
        });
        connect(channel, &Channel::received, this, [this, channel](const Message& message) {
            std::deque<QJsonObject>& kept = m_messages[channel];
            QJsonObject entry{{"time", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
                              {"type", message.type},
                              {"from", message.from},
                              {"body", message.body}};
            if (!message.to.isEmpty()) entry.insert("to", message.to);
            if (message.seq) entry.insert("seq", message.seq);
            if (!message.bytes.isEmpty()) entry.insert("bytes", qint64(message.bytes.size()));
            kept.push_back(entry);
            while (kept.size() > kKeptMessages) kept.pop_front();
        });
    }
}

void PeerRoomsDebugDialog::refresh() {
    if (!m_peers) return;
    QString structure;
    for (Room* room : m_peers->rooms()) {
        watchChannels(room);
        structure += keyOf(room, {}) + room->name() + stateName(room->state());
        for (const Peer& peer : room->peers()) structure += peer.id + peer.name;
        structure += room->channelNames().join(u',') + u';' + room->stateNames().join(u',') + u';' +
                     room->peerStateNames().join(u',') + u'\n';
    }
    if (structure != m_structure) {
        m_structure = structure;
        rebuildTree();
    }
    showSelected();
}

void PeerRoomsDebugDialog::rebuildTree() {
    const QString selected = m_tree->currentItem() ? m_tree->currentItem()->data(0, kKeyRole).toString() : QString();
    QSet<QString> collapsed;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_tree->topLevelItem(i);
        if (!top->isExpanded()) collapsed.insert(top->data(0, kKeyRole).toString());
        for (int j = 0; j < top->childCount(); ++j) {
            if (!top->child(j)->isExpanded()) collapsed.insert(top->child(j)->data(0, kKeyRole).toString());
        }
    }
    m_tree->clear();
    QTreeWidgetItem* current = nullptr;
    const auto add = [&](QTreeWidgetItem* parent, const QString& label, const QString& value, const QString& key) {
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        item->setText(0, label);
        item->setText(1, value);
        item->setData(0, kKeyRole, key);
        if (key == selected) current = item;
        return item;
    };
    for (Room* room : m_peers->rooms()) {
        QTreeWidgetItem* top = add(nullptr,
                                   room->name().isEmpty() ? tr("(connecting)") : room->name(),
                                   QStringLiteral("%1 · %2 · %3")
                                       .arg(roomTypeName(room->type()), room->isHost() ? tr("host") : tr("member"),
                                            stateName(room->state())),
                                   keyOf(room, QStringLiteral("room")));
        QTreeWidgetItem* people = add(top, tr("Peers"), QString::number(room->peers().size()), keyOf(room, "peers"));
        for (const Peer& peer : room->peers()) {
            add(people, peer.name, kindName(peer.kind) + (peer.id == room->me().id ? tr(" (this instance)") : QString()),
                keyOf(room, "peer", peer.id));
        }
        QTreeWidgetItem* channels = add(top, tr("Channels"), QString::number(room->channelNames().size()),
                                        keyOf(room, "channels"));
        for (const QString& name : room->channelNames()) add(channels, name, {}, keyOf(room, "channel", name));
        QTreeWidgetItem* maps = add(top, tr("Maps"), QString::number(room->stateNames().size()), keyOf(room, "maps"));
        for (const QString& name : room->stateNames()) add(maps, name, {}, keyOf(room, "map", name));
        QTreeWidgetItem* states = add(top, tr("Per-peer states"), QString::number(room->peerStateNames().size()),
                                      keyOf(room, "peerStates"));
        for (const QString& name : room->peerStateNames()) add(states, name, {}, keyOf(room, "peerState", name));
        top->setExpanded(!collapsed.contains(top->data(0, kKeyRole).toString()));
        for (int j = 0; j < top->childCount(); ++j) {
            top->child(j)->setExpanded(!collapsed.contains(top->child(j)->data(0, kKeyRole).toString()));
        }
    }
    if (current) m_tree->setCurrentItem(current);
}

QJsonObject PeerRoomsDebugDialog::dataOf(const QString& key) const {
    const QStringList parts = key.split(u'|');
    if (parts.size() < 3 || !m_peers) return {};
    Room* room = nullptr;
    for (Room* candidate : m_peers->rooms()) {
        if (QString::number(quintptr(candidate)) == parts[0]) room = candidate;
    }
    if (!room) return {};
    const QString what = parts[1];
    const QString name = parts.mid(2).join(u'|');
    if (what == u"room") {
        QJsonObject info{{"name", room->name()},
                         {"type", roomTypeName(room->type())},
                         {"state", stateName(room->state())},
                         {"host", room->isHost()},
                         {"address", room->address()},
                         {"me", peerObject(room->me())},
                         {"hostPeer", peerObject(room->hostPeer())},
                         {"channels", QJsonArray::fromStringList(room->channelNames())},
                         {"maps", QJsonArray::fromStringList(room->stateNames())},
                         {"peerStates", QJsonArray::fromStringList(room->peerStateNames())}};
        if (room->isHost()) {
            info.insert("port", int(room->port()));
            info.insert("admission", admissionName(room->admission()));
            info.insert("password", room->hasPassword());
        }
        return info;
    }
    if (what == u"peers") {
        QJsonArray list;
        for (const Peer& peer : room->peers()) list.append(peerObject(peer));
        return {{"peers", list}};
    }
    if (what == u"peer") {
        const Peer peer = room->peer(name);
        QJsonObject object = peerObject(peer);
        QJsonObject states;
        for (const QString& state : room->peerStateNames()) {
            const QJsonValue value = room->peerState(state)->value(peer.id);
            if (!value.isUndefined()) states.insert(state, value);
        }
        object.insert("states", states);
        if (room->isHost() && peer.id != room->me().id) object.insert("queuedBytes", room->queuedBytes(peer.id));
        return object;
    }
    if (what == u"channel") {
        Channel* channel = room->findChannel(name);
        QJsonArray messages;
        if (channel) {
            for (const QJsonObject& message : m_messages.value(channel)) messages.append(message);
        }
        return {{"channel", name}, {"messages", messages}};
    }
    if (what == u"map") {
        StateMap* map = room->state(name);
        return {{"map", name}, {"items", map ? map->items() : QJsonObject()}};
    }
    if (what == u"peerState") {
        PeerState* state = room->peerState(name);
        QJsonObject values;
        if (state) {
            const QHash<PeerId, QJsonValue> all = state->values();
            for (auto it = all.cbegin(); it != all.cend(); ++it) {
                const Peer peer = room->peer(it.key());
                values.insert(peer.isValid() ? peer.name + QStringLiteral(" (") + it.key() + u')' : it.key(), it.value());
            }
        }
        return {{"peerState", name}, {"values", values}};
    }
    // The groups: everything below them.
    QJsonObject all;
    if (what == u"channels") {
        for (const QString& channel : room->channelNames()) all.insert(channel, dataOf(keyOf(room, "channel", channel)));
    } else if (what == u"maps") {
        for (const QString& map : room->stateNames()) all.insert(map, dataOf(keyOf(room, "map", map)));
    } else if (what == u"peerStates") {
        for (const QString& state : room->peerStateNames()) all.insert(state, dataOf(keyOf(room, "peerState", state)));
    }
    return all;
}

void PeerRoomsDebugDialog::showSelected() {
    QTreeWidgetItem* item = m_tree->currentItem();
    const QString text =
        item ? QString::fromUtf8(QJsonDocument(dataOf(item->data(0, kKeyRole).toString())).toJson(QJsonDocument::Indented))
             : QString();
    if (text != m_text->toPlainText()) {
        const int scroll = m_text->verticalScrollBar() ? m_text->verticalScrollBar()->value() : 0;
        m_text->setPlainText(text);
        if (m_text->verticalScrollBar()) m_text->verticalScrollBar()->setValue(scroll);
    }
    // The value column of the leaves: a glance without selecting.
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        const QString key = (*it)->data(0, kKeyRole).toString();
        if (key.contains(QLatin1String("|map|")) || key.contains(QLatin1String("|peerState|")) ||
            key.contains(QLatin1String("|channel|"))) {
            const QJsonObject data = dataOf(key);
            const QJsonValue shown = data.contains("items")    ? data.value("items")
                                     : data.contains("values") ? data.value("values")
                                                               : QJsonValue(qint64(data.value("messages").toArray().size()));
            (*it)->setText(1, shortJson(shown));
        }
    }
}

}  // namespace usdcc::ui
