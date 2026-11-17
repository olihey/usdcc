#pragma once

#include "usdcc/peers/export.h"
#include "usdcc/peers/types.h"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

namespace usdcc::peers {

class Room;

// A named stream of messages in a room (PEERS.md §7.1), from Room::channel(). It
// receives only once opened: a member that hasn't opened a channel ignores it.
//
// On the host, the channel also says who may send on it and whether guests reach it.
// The host enforces both, for every member, whatever the member's own channel says.
class USDCC_PEERS_API Channel : public QObject {
    Q_OBJECT

public:
    ~Channel() override;

    QString name() const;
    Delivery delivery() const;
    Room* room() const;

    // To everyone, or to one member. On a Direct channel, no one named means the
    // host. Before the room is open, messages wait and go out once it is.
    void send(const QString& type, const QJsonObject& body = {}, const QByteArray& bytes = {},
              const PeerId& to = {});

    // The host's rules for this channel. On a member that only joined, they change
    // nothing: the host's own channel decides.
    void setSenders(Senders senders);
    void setNamedSenders(const QList<PeerId>& peers);  // Also sets Senders::Named
    Senders senders() const;
    QList<PeerId> namedSenders() const;
    // Guests reach only the channels opened to them (PEERS.md §6.1): off by default.
    void setOpenToGuests(bool open);
    bool isOpenToGuests() const;
    // Whether the host lets `peer` send here.
    bool maySend(const Peer& peer) const;

Q_SIGNALS:
    void received(const usdcc::peers::Message& message);
    // The host refused something this member sent here: not allowed, not open to
    // guests, nobody of that id.
    void refused(const QString& type, const QString& reason);

private:
    friend class Room;
    Channel(Room* room, const QString& name, Delivery delivery);

    Room* m_room;
    QString m_name;
    Delivery m_delivery;
    Senders m_senders = Senders::Everyone;
    QList<PeerId> m_namedSenders;
    bool m_openToGuests = false;
};

}  // namespace usdcc::peers
