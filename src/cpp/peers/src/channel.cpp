#include "usdcc/peers/channel.h"

#include "usdcc/peers/room.h"

#include "room_private.h"

namespace usdcc::peers {

Channel::Channel(Room* room, const QString& name, Delivery delivery)
    : QObject(room), m_room(room), m_name(name), m_delivery(delivery) {}

Channel::~Channel() {
    if (m_room && m_room->d) {
        m_room->d->channels.remove(m_name);
    }
}

QString Channel::name() const {
    return m_name;
}

Delivery Channel::delivery() const {
    return m_delivery;
}

Room* Channel::room() const {
    return m_room;
}

void Channel::send(const QString& type, const QJsonObject& body, const QByteArray& bytes, const PeerId& to) {
    m_room->d->send(this, type, body, bytes, to);
}

bool Channel::sendPrivate(const QString& type, const QJsonObject& body, const PeerId& to) {
    return m_room->d->sendPrivate(this, type, body, to);
}

void Channel::setSenders(Senders senders) {
    m_senders = senders;
}

void Channel::setNamedSenders(const QList<PeerId>& peers) {
    m_senders = Senders::Named;
    m_namedSenders = peers;
}

Senders Channel::senders() const {
    return m_senders;
}

QList<PeerId> Channel::namedSenders() const {
    return m_namedSenders;
}

void Channel::setOpenToGuests(bool open) {
    m_openToGuests = open;
}

bool Channel::isOpenToGuests() const {
    return m_openToGuests;
}

bool Channel::maySend(const Peer& peer) const {
    if (peer.kind == Peer::Kind::Host) {
        return true;
    }
    if (peer.kind == Peer::Kind::Guest && !m_openToGuests) {
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

}  // namespace usdcc::peers
