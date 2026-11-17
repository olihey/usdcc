#include "usdcc/peers/state.h"

#include "usdcc/peers/room.h"

#include "room_private.h"

#include <QSet>

namespace usdcc::peers {

// ---------------------------------------------------------------------------------
// StateMap

StateMap::StateMap(Room* room, const QString& name) : QObject(room), m_room(room), m_name(name) {}

QJsonValue StateMap::visible(const QString& key) const {
    const auto pending = m_pending.constFind(key);
    if (pending != m_pending.constEnd()) {
        return pending->value;
    }
    return m_confirmed.value(key, QJsonValue(QJsonValue::Undefined));
}

QJsonValue StateMap::value(const QString& key) const {
    return visible(key);
}

bool StateMap::contains(const QString& key) const {
    return !visible(key).isUndefined();
}

QStringList StateMap::keys() const {
    QSet<QString> all(m_confirmed.keyBegin(), m_confirmed.keyEnd());
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        all.insert(it.key());
    }
    QStringList result;
    for (const QString& key : all) {
        if (contains(key)) {
            result.append(key);
        }
    }
    result.sort();
    return result;
}

QJsonObject StateMap::items() const {
    QJsonObject result;
    for (const QString& key : keys()) {
        result.insert(key, visible(key));
    }
    return result;
}

void StateMap::set(const QString& key, const QJsonValue& value) {
    if (m_room->isHost()) {
        m_room->d->sendState(this, key, value);
        return;
    }
    if (m_room->state() == Room::State::Closed) {
        return;
    }
    // Here at once; the host's order decides in the end.
    const QJsonValue before = visible(key);
    Pending& pending = m_pending[key];
    pending.value = value;
    ++pending.count;
    if (before != value) {
        Q_EMIT changed(key, value, m_room->me());
    }
    m_room->d->sendState(this, key, value);
}

void StateMap::remove(const QString& key) {
    set(key, QJsonValue(QJsonValue::Undefined));
}

void StateMap::setNamedSenders(const QList<PeerId>& peers) {
    m_senders = Senders::Named;
    m_namedSenders = peers;
}

bool StateMap::maySend(const Peer& peer) const {
    if (peer.kind == Peer::Kind::Host) {
        return true;
    }
    if (peer.kind == Peer::Kind::Guest) {
        return false;
    }
    switch (m_senders) {
    case Senders::Everyone:
        return true;
    case Senders::Host:
        return false;
    case Senders::Named:
        return m_namedSenders.contains(peer.id);
    }
    return false;
}

void StateMap::apply(const QString& key, const QJsonValue& value, const Peer& by, bool mine) {
    const QJsonValue before = visible(key);
    if (value.isUndefined()) {
        m_confirmed.remove(key);
    } else {
        m_confirmed.insert(key, value);
    }
    if (mine) {
        const auto pending = m_pending.find(key);
        if (pending != m_pending.end() && --pending->count <= 0) {
            m_pending.erase(pending);
        }
    }
    const QJsonValue after = visible(key);
    if (before != after) {
        Q_EMIT changed(key, after, by);
    }
}

void StateMap::replace(const QJsonObject& items) {
    QSet<QString> touched(m_confirmed.keyBegin(), m_confirmed.keyEnd());
    for (auto it = items.begin(); it != items.end(); ++it) {
        touched.insert(it.key());
    }
    QHash<QString, QJsonValue> before;
    for (const QString& key : touched) {
        before.insert(key, visible(key));
    }
    m_confirmed.clear();
    for (auto it = items.begin(); it != items.end(); ++it) {
        m_confirmed.insert(it.key(), it.value());
    }
    const Peer by = m_room->hostPeer();
    for (const QString& key : touched) {
        const QJsonValue after = visible(key);
        if (before.value(key) != after) {
            Q_EMIT changed(key, after, by);
        }
    }
}

void StateMap::refuse(const QString& key, const QString& reason) {
    const QJsonValue before = visible(key);
    const auto pending = m_pending.find(key);
    if (pending != m_pending.end() && --pending->count <= 0) {
        m_pending.erase(pending);
    }
    const QJsonValue after = visible(key);
    if (before != after) {
        Q_EMIT changed(key, after, m_room->hostPeer());
    }
    Q_EMIT refused(key, reason);
}

// ---------------------------------------------------------------------------------
// PeerState

PeerState::PeerState(Room* room, const QString& name) : QObject(room), m_room(room), m_name(name) {}

void PeerState::set(const QJsonValue& value) {
    m_room->d->sendPeerState(this, value);
}

QJsonValue PeerState::value(const PeerId& peer) const {
    return m_values.value(peer, QJsonValue(QJsonValue::Undefined));
}

void PeerState::apply(const Peer& peer, const QJsonValue& value) {
    if (value.isUndefined() || value.isNull()) {
        if (m_values.remove(peer.id)) {
            Q_EMIT changed(peer, QJsonValue(QJsonValue::Undefined));
        }
        return;
    }
    if (m_values.value(peer.id) == value && m_values.contains(peer.id)) {
        return;
    }
    m_values.insert(peer.id, value);
    Q_EMIT changed(peer, value);
}

}  // namespace usdcc::peers
