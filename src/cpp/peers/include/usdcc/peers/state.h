#pragma once

// Shared state (PEERS.md §7.2), from Room::state() and Room::peerState().

#include "usdcc/peers/export.h"
#include "usdcc/peers/types.h"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace usdcc::peers {

class Room;

// A named set of keys with JSON values, equal on every peer. A change applies here at
// once and goes to the host, which orders it: for one key the last writer in the host's
// order wins, and a peer whose own change lost hears so through changed() like any other.
// A joiner gets the whole map, then the changes. Guests don't see maps.
class USDCC_PEERS_API StateMap : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 kMaxValueBytes = 256 * 1024;
    static constexpr qint64 kMaxMapBytes = 16 * 1024 * 1024;

    QString name() const { return m_name; }
    Room* room() const { return m_room; }

    // Undefined for a key that isn't there.
    QJsonValue value(const QString& key) const;
    bool contains(const QString& key) const;
    QStringList keys() const;
    QJsonObject items() const;

    void set(const QString& key, const QJsonValue& value);
    void remove(const QString& key);

    // The host's: who may write (PEERS.md §6).
    void setSenders(Senders senders) { m_senders = senders; }
    void setNamedSenders(const QList<PeerId>& peers);
    bool maySend(const Peer& peer) const;

Q_SIGNALS:
    // `value` is undefined for a removed key. `by` is whoever's change it is.
    void changed(const QString& key, const QJsonValue& value, const usdcc::peers::Peer& by);
    // The host turned a change of this peer's down; the key is back to what it had.
    void refused(const QString& key, const QString& reason);

private:
    friend class Room;
    StateMap(Room* room, const QString& name);
    QJsonValue visible(const QString& key) const;
    // From the host, in its order.
    void apply(const QString& key, const QJsonValue& value, const Peer& by, bool mine);
    void replace(const QJsonObject& items);
    void refuse(const QString& key, const QString& reason);

    Room* m_room;
    QString m_name;
    QHash<QString, QJsonValue> m_confirmed;
    struct Pending {
        QJsonValue value;
        int count = 0;
    };
    QHash<QString, Pending> m_pending;  // This peer's own changes the host hasn't ordered yet
    Senders m_senders = Senders::Everyone;
    QList<PeerId> m_namedSenders;
};

// A map with one value per peer, which only that peer sets and which goes away when it
// leaves. Delivered latest only. For small rooms: the host stops passing it on above
// Limits::maxPeerStateMembers. usdcc's own is `presence`.
class USDCC_PEERS_API PeerState : public QObject {
    Q_OBJECT

public:
    QString name() const { return m_name; }
    Room* room() const { return m_room; }

    // This peer's own; null removes it.
    void set(const QJsonValue& value);
    QJsonValue value(const PeerId& peer) const;
    QHash<PeerId, QJsonValue> values() const { return m_values; }
    // The host's: whether guests see it (they never set it).
    void setOpenToGuests(bool open) { m_openToGuests = open; }
    bool isOpenToGuests() const { return m_openToGuests; }

Q_SIGNALS:
    // `value` is undefined when the peer removed it or left.
    void changed(const usdcc::peers::Peer& peer, const QJsonValue& value);

private:
    friend class Room;
    PeerState(Room* room, const QString& name);
    void apply(const Peer& peer, const QJsonValue& value);

    Room* m_room;
    QString m_name;
    QHash<PeerId, QJsonValue> m_values;
    bool m_openToGuests = false;
};

}  // namespace usdcc::peers
