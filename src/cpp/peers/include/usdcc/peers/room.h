#pragma once

// A room (docs/PEERS.md): one member hosts it and orders its messages, the others
// join it. A peer proves who it is with its key and everything it says is encrypted
// (§6); a guest is a web page, unencrypted on a plain link (§6.1).
//
//     HostOptions options;
//     options.name = "Shot 12";
//     options.profile = profile;               // Identity, instance, name, color
//     options.admission = Admission::Ask;      // Or Open: anyone who finds it
//     Room* room = Room::host(options);
//     Channel* chat = room->channel("chat");
//     QString link = room->link();             // usdcc://join/<address>:<port>#k=…&s=…
//
//     JoinOptions join;
//     join.profile = profile;
//     join.secret = parsed->secret;            // From the link; empty when found on the network
//     join.expectedFingerprint = parsed->fingerprint;
//     Room* joined = Room::join(parsed->address, join);
//
// Everything happens on the thread the room lives on, through its event loop.

#include "usdcc/peers/export.h"
#include "usdcc/peers/identity.h"
#include "usdcc/peers/types.h"

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <memory>
#include <optional>

namespace usdcc::peers {

class Channel;
class PeerState;
class Room;
class StateMap;

// Who this member is: the profile's key and how it shows (PEERS.md §4). Without a
// valid identity a room makes one that lasts as long as it does.
struct USDCC_PEERS_API Profile {
    Identity identity;
    QString instance;  // Which running program; random if empty
    QString name;
    QString color;     // "#rrggbb"
    QString app;       // "usdcc 0.3.0"
};

// What a host lets each member do (PEERS.md §7.4, §7.5).
struct USDCC_PEERS_API Limits {
    int maxMembers = 50;                      // Besides the host, guests included
    int maxGuests = 8;
    qint64 maxFrameBytes = 4 * 1024 * 1024;   // One message from a member
    double messagesPerSecond = 200;           // From one member, on average
    int messageBurst = 400;                   // ... and at once
    qint64 maxQueuedBytes = 16 * 1024 * 1024; // Waiting for one member; more and it is dropped as too slow
    int helloTimeoutMs = 10000;               // To finish the handshake
    int pingIntervalMs = 10000;
    int silenceTimeoutMs = 30000;             // Heard nothing, not even a pong: gone
    int reconnectGraceMs = 30000;             // A peer whose connection broke is kept this long
    int historyMessages = 2000;               // Ordered messages kept for a peer that reconnects
    qint64 historyBytes = 8 * 1024 * 1024;
    int maxPeerStateMembers = 50;             // Above this, per-peer state isn't passed on
    int joinRequestTimeoutMs = 120000;        // A question to the host nobody answers is a no
};

struct USDCC_PEERS_API HostOptions {
    QString name;                             // The room's
    Profile profile;                          // The host's
    QHostAddress address = QHostAddress(QHostAddress::LocalHost);  // Any to be reached from the network
    quint16 port = 0;                         // 0: any free port
    // Who may join without the secret: found on this machine or network.
    Admission admission = Admission::Invited;
    QString password;                         // Asked of those without the secret; empty: none
    bool askBeforeLinkJoin = false;           // Ask even when someone has the secret
    QStringList trustedFingerprints;          // "Always allow": never asked about
    bool guests = false;                      // Whether web pages may join (PEERS.md §6.1)
    QByteArray page;                          // The guests' page at `/`; empty: a plain one built in
    QString pageDirectory;                    // Or a folder: `/` is its index.html, `/<file>` its files
    QStringList allowedOrigins;               // Pages besides the room's own that may connect (a tunnel's)
    Limits limits;
};

struct USDCC_PEERS_API JoinOptions {
    Profile profile;
    QString secret;                           // From the link: the invitation. Empty when found
    QString password;                         // If the room has one and there's no secret
    QString expectedFingerprint;              // The host's key must be this one (the link's `k`)
    QString origin;                           // Sent as the Origin header, as a browser would; empty: none
    int pingIntervalMs = 10000;
    int silenceTimeoutMs = 30000;
    int reconnectForMs = 60000;               // Keeps trying this long after losing the host; 0: never
};

// usdcc://join/<address>:<port>#k=<fingerprint>&s=<secret> (PEERS.md §5.3).
struct USDCC_PEERS_API JoinLink {
    QString address;  // "host:port"
    QString fingerprint;
    QString secret;
};
USDCC_PEERS_API QString makeJoinLink(const JoinLink& link);
USDCC_PEERS_API std::optional<JoinLink> parseJoinLink(const QString& text);

// Someone without the secret wants in, and the room asks (Admission::Ask). Answer with
// allow() or deny(); unanswered, it is denied after Limits::joinRequestTimeoutMs. It is
// deleted once settled, or when the one asking goes away (cancelled()).
class USDCC_PEERS_API JoinRequest : public QObject {
    Q_OBJECT

public:
    Peer peer() const { return m_peer; }
    void allow();
    void deny(const QString& reason = {});

Q_SIGNALS:
    void cancelled();

private:
    friend class Room;
    JoinRequest(Room* room, const Peer& peer);
    Room* m_room;
    Peer m_peer;
    bool m_settled = false;
};

class USDCC_PEERS_API Room : public QObject {
    Q_OBJECT

public:
    enum class State {
        Connecting,    // Not yet welcomed
        Open,
        Reconnecting,  // A member that lost the host, trying again (JoinOptions::reconnectForMs)
        Closed,
    };

    // Starts listening. nullptr, with why in `error`, when it can't (the port is taken).
    static Room* host(const HostOptions& options, QObject* parent = nullptr, QString* error = nullptr);
    // Connects to `address` ("host:port"). The room opens (opened()) when the host
    // welcomes it, or closes (closed()) if refused or unreachable.
    static Room* join(const QString& address, const JoinOptions& options, QObject* parent = nullptr);

    ~Room() override;

    State state() const;
    bool isHost() const;
    QString name() const;
    Peer me() const;
    Peer hostPeer() const;
    // Everyone in the room, this member included.
    QList<Peer> peers() const;
    Peer peer(const PeerId& id) const;
    // The host's address as joined ("host:port"); the host's own listening one.
    QString address() const;

    // Opens a channel, or returns the one already open (its delivery stays as it
    // was). nullptr for a name that isn't valid (isValidChannelName()).
    Channel* channel(const QString& name, Delivery delivery = Delivery::Ordered);
    Channel* findChannel(const QString& name) const;
    // A map, made on first use (PEERS.md §7.2). nullptr for an invalid name.
    StateMap* state(const QString& name);
    PeerState* peerState(const QString& name);

    // Leaves the room, or on the host, closes it for everyone.
    void close(const QString& reason = {});

    // Host only.
    quint16 port() const;
    Admission admission() const;
    bool hasPassword() const;
    QString secret() const;       // For peers; never logged
    QString guestSecret() const;  // For guests, if the room takes them
    // A link that invites a peer: usdcc://join/<address>:<port>#k=…&s=…. Without an
    // address: 127.0.0.1 for a room on the loopback, else this machine's first network
    // address. Likewise the guests' web link, http://<address>:<port>/#s=<guest secret>.
    QString link(const QString& address = {}) const;
    QUrl guestLink(const QString& address = {}) const;
    void remove(const PeerId& id, const QString& reason = {});
    void trust(const QString& fingerprint);
    // What waits to be sent to a member: what makes it too slow. On a member that
    // joined, what waits to go to the host, whoever it is for.
    qint64 queuedBytes(const PeerId& id) const;

Q_SIGNALS:
    void opened();
    void stateChanged(usdcc::peers::Room::State state);
    void peerJoined(const usdcc::peers::Peer& peer);
    void peerLeft(const usdcc::peers::Peer& peer, const QString& reason);
    // A member back after a lost connection: `caughtUp` if it got every ordered message
    // it missed, false if the host no longer had them and sent its state afresh.
    void resumed(bool caughtUp);
    void closed(const QString& reason);
    // Host only: someone asks to join (Admission::Ask).
    void joinRequested(usdcc::peers::JoinRequest* request);

private:
    friend class Channel;
    friend class JoinRequest;
    friend class PeerState;
    friend class StateMap;
    struct Impl;
    explicit Room(QObject* parent);
    std::unique_ptr<Impl> d;
};

}  // namespace usdcc::peers
