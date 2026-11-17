#pragma once

// A room (docs/PEERS.md): one member hosts it and orders its messages, the others
// join it by address. Phase 1a: no identity, encryption, reconnecting or shared
// state yet (PEERS.md §14).
//
//     auto* room = Room::host({.name = "Shot 12", .guests = true});
//     Channel* chat = room->channel("chat");
//     chat->setOpenToGuests(true);
//     connect(chat, &Channel::received, ..., [](const Message& m) { ... });
//     QUrl link = room->guestLink();   // http://127.0.0.1:<port>/#s=<secret>
//
//     auto* joined = Room::join("127.0.0.1:47800", secret, {.name = "Anna"});
//     connect(joined, &Room::opened, ...);
//
// Everything happens on the thread the room lives on, through its event loop.

#include "usdcc/peers/export.h"
#include "usdcc/peers/types.h"

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <memory>

namespace usdcc::peers {

class Channel;

// What a host lets each member do (PEERS.md §7.4).
struct USDCC_PEERS_API Limits {
    int maxMembers = 50;                      // Besides the host, guests included
    int maxGuests = 8;
    qint64 maxFrameBytes = 4 * 1024 * 1024;   // One message from a member
    double messagesPerSecond = 200;           // From one member, on average
    int messageBurst = 400;                   // ... and at once
    qint64 maxQueuedBytes = 16 * 1024 * 1024; // Waiting for one member; more and it is dropped as too slow
    int helloTimeoutMs = 10000;               // To send the first message
    int pingIntervalMs = 10000;
    int silenceTimeoutMs = 30000;             // Heard nothing, not even a pong: gone
};

struct USDCC_PEERS_API HostOptions {
    QString name;                             // The room's
    QString hostName;                         // The host's own, as members see it
    QHostAddress address = QHostAddress(QHostAddress::LocalHost);  // Any to be reached from the network
    quint16 port = 0;                         // 0: any free port
    bool guests = false;                      // Whether web pages may join (PEERS.md §6.1)
    QByteArray page;                          // The guests' page at `/`; empty: a plain one built in
    QString pageDirectory;                    // Or a folder: `/` is its index.html, `/<file>` its files
    QStringList allowedOrigins;               // Pages besides the room's own that may connect (a tunnel's)
    Limits limits;
};

struct USDCC_PEERS_API JoinOptions {
    QString name;
    QString origin;                           // Sent as the Origin header, as a browser would; empty: none
    int pingIntervalMs = 10000;
    int silenceTimeoutMs = 30000;
};

class USDCC_PEERS_API Room : public QObject {
    Q_OBJECT

public:
    enum class State { Connecting, Open, Closed };

    // Starts listening. nullptr, with why in `error`, when it can't (the port is taken).
    static Room* host(const HostOptions& options, QObject* parent = nullptr, QString* error = nullptr);
    // Connects to `address` ("host:port") with the secret from the link. The room
    // opens (opened()) when the host welcomes it, or closes (closed()) if refused.
    static Room* join(const QString& address, const QString& secret, const JoinOptions& options = {},
                      QObject* parent = nullptr);

    ~Room() override;

    State state() const;
    bool isHost() const;
    QString name() const;
    Peer me() const;
    Peer hostPeer() const;
    // Everyone in the room, this member included.
    QList<Peer> peers() const;
    Peer peer(const PeerId& id) const;

    // Opens a channel, or returns the one already open (its delivery stays as it
    // was). nullptr for a name that isn't valid (isValidChannelName()).
    Channel* channel(const QString& name, Delivery delivery = Delivery::Ordered);
    Channel* findChannel(const QString& name) const;

    // Leaves the room, or on the host, closes it for everyone.
    void close(const QString& reason = {});

    // Host only.
    quint16 port() const;
    QString secret() const;       // For peers; never logged
    QString guestSecret() const;  // For guests, if the room takes them
    // http://<address>:<port>/#s=<guest secret>. Without an address: 127.0.0.1 for a
    // room on the loopback, else this machine's first network address.
    QUrl guestLink(const QString& address = {}) const;
    void remove(const PeerId& id, const QString& reason = {});
    // What waits to be sent to a member: what makes it too slow.
    qint64 queuedBytes(const PeerId& id) const;

Q_SIGNALS:
    void opened();
    void peerJoined(const usdcc::peers::Peer& peer);
    void peerLeft(const usdcc::peers::Peer& peer, const QString& reason);
    void closed(const QString& reason);

private:
    friend class Channel;
    struct Impl;
    explicit Room(QObject* parent);
    std::unique_ptr<Impl> d;
};

}  // namespace usdcc::peers
