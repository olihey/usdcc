#pragma once

// Finding rooms (PEERS.md §5): on this machine through a folder of small files (§5.1),
// on this network through mDNS (§5.2). Only rooms that take members without an
// invitation (Admission::Ask, Admission::Open) are found; an invited room is reached
// by its link.

#include "usdcc/peers/export.h"
#include "usdcc/peers/types.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

namespace usdcc::peers {

// What a host tells others about its room.
struct USDCC_PEERS_API Announcement {
    QString fingerprint;  // The host's key's
    QString instance;     // The host's instance; with the fingerprint, the host's peer id
    QString name;         // The room's
    QString hostName;
    quint16 port = 0;
    Admission admission = Admission::Ask;
    bool password = false;

    QString hostId() const { return fingerprint + u'-' + instance; }
};

// A room someone else hosts, as found.
struct USDCC_PEERS_API FoundRoom {
    enum class Where { ThisMachine, Network };

    Announcement announcement;
    QString address;  // "host:port" to join
    Where where = Where::Network;
    int protocol = kProtocolVersion;

    QString hostId() const { return announcement.hostId(); }
    bool operator==(const FoundRoom&) const;
};

// The network half, behind an interface: multicast isn't dependable in CI (PEERS.md
// §15), so tests use a fake.
class USDCC_PEERS_API NetworkDiscovery : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Answers for the room from now on; again with the same instance, it changes it.
    virtual void announce(const Announcement& announcement) = 0;
    virtual void withdraw(const QString& instance) = 0;
    // Asks the network now and then, while true. Only a host answers; a member that
    // is looking only asks (PEERS.md §5.2).
    virtual void setLooking(bool looking) = 0;
    // Why it doesn't work, if it doesn't (no network, the port refused); empty if fine.
    virtual QString problem() const { return {}; }

Q_SIGNALS:
    void found(const usdcc::peers::FoundRoom& room);  // Also when it changed
    void lost(const QString& hostId);
};

// mDNS and DNS-SD, service type `_usdcc._tcp` (PEERS.md §5.2), with the public-domain
// `mdns` library. A host answers on port 5353 beside the system's own responder; a
// member that looks asks from a port of its own and gets answers sent to it.
USDCC_PEERS_API std::unique_ptr<NetworkDiscovery> createMdnsDiscovery(QObject* parent = nullptr);

// A network inside the process, for tests: every FakeNetworkDiscovery made with the
// same `network` name sees the others' rooms, as if on one network.
class USDCC_PEERS_API FakeNetworkDiscovery : public NetworkDiscovery {
    Q_OBJECT

public:
    explicit FakeNetworkDiscovery(const QString& network, QObject* parent = nullptr);
    ~FakeNetworkDiscovery() override;
    void announce(const Announcement& announcement) override;
    void withdraw(const QString& instance) override;
    void setLooking(bool looking) override;
    bool isLooking() const { return m_looking; }

private:
    void notifyAll();
    QString m_network;
    QHash<QString, Announcement> m_announced;
    QHash<QString, FoundRoom> m_seen;
    bool m_looking = false;
};

// This machine's rooms (PEERS.md §5.1): a hosting instance writes
// `<folder>/<instance>.json` and removes it when the room ends; the others watch the
// folder. A file whose port doesn't answer is stale and is removed.
class USDCC_PEERS_API RoomDirectory : public QObject {
    Q_OBJECT

public:
    explicit RoomDirectory(const QString& folder, QObject* parent = nullptr);
    ~RoomDirectory() override;

    QString folder() const { return m_folder; }
    void publish(const Announcement& announcement);
    void unpublish(const QString& instance);
    // Others' rooms, as last read.
    QList<FoundRoom> rooms() const { return m_rooms.values(); }
    // Reads the folder again (it is watched; this is for tests and a start).
    void rescan();

Q_SIGNALS:
    void found(const usdcc::peers::FoundRoom& room);
    void lost(const QString& hostId);

private:
    void checkStale(const QString& file, quint16 port);
    QString m_folder;
    QFileSystemWatcher m_watcher;
    QTimer m_rescan;
    QHash<QString, Announcement> m_published;  // instance ->
    QHash<QString, FoundRoom> m_rooms;         // hostId ->
};

}  // namespace usdcc::peers

Q_DECLARE_METATYPE(usdcc::peers::FoundRoom)
