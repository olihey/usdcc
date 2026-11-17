#pragma once

// The rooms of one program (PEERS.md §2): it can host some and be in others at once,
// finds rooms on this machine and network, and announces the ones it hosts that take
// members without an invitation.

#include "usdcc/peers/discovery.h"
#include "usdcc/peers/export.h"
#include "usdcc/peers/room.h"

#include <QList>
#include <QObject>
#include <QPointer>

#include <memory>

namespace usdcc::peers {

class USDCC_PEERS_API Peers : public QObject {
    Q_OBJECT

public:
    struct Options {
        Profile profile;
        QString roomsFolder;                        // This machine's rooms (§5.1); empty: none
        std::unique_ptr<NetworkDiscovery> network;  // This network's (§5.2); null: none
        // Announce rooms on the loopback on the network too: for tests, whose rooms
        // stay off the network (listening there raises Windows Firewall's question).
        bool announceLoopback = false;
    };

    explicit Peers(Options options, QObject* parent = nullptr);
    ~Peers() override;

    Profile profile() const { return m_profile; }
    // For rooms opened from now on.
    void setProfile(const QString& name, const QString& color);

    // With this program's profile. A room on a network address that doesn't need an
    // invitation is announced there; one that doesn't need one is listed on this
    // machine. A port that is taken falls back to any free one.
    Room* host(HostOptions options, QString* error = nullptr);
    Room* join(const QString& address, JoinOptions options);
    Room* join(const FoundRoom& room, const QString& password = {});
    // usdcc://join/… (§5.3). nullptr, with why in `error`, for a link that isn't one.
    Room* joinLink(const QString& link, QString* error = nullptr);
    // Open or still connecting; a room leaves the list when it closes.
    QList<Room*> rooms() const;

    // Rooms others host that this program isn't in, this machine's first.
    QList<FoundRoom> foundRooms() const;
    // Asks the network only while something shows the list (§5.2).
    void setLooking(bool looking);
    bool isLooking() const { return m_looking; }
    NetworkDiscovery* network() const { return m_network.get(); }
    RoomDirectory* directory() const { return m_directory.get(); }

Q_SIGNALS:
    void roomAdded(usdcc::peers::Room* room);
    void roomRemoved(usdcc::peers::Room* room);
    void foundRoomsChanged();

private:
    Room* adopt(Room* room);
    void announce(Room* room, const HostOptions& options);

    Profile m_profile;
    std::unique_ptr<RoomDirectory> m_directory;
    std::unique_ptr<NetworkDiscovery> m_network;
    QList<QPointer<Room>> m_rooms;
    QHash<QString, FoundRoom> m_local;    // host id ->
    QHash<QString, FoundRoom> m_networkRooms;
    bool m_looking = false;
    bool m_announceLoopback = false;
};

}  // namespace usdcc::peers
