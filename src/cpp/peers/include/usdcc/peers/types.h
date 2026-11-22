#pragma once

// The words of docs/PEERS.md: a peer, a message, how a channel delivers, who may join.

#include "usdcc/peers/export.h"

#include <QByteArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>

namespace usdcc::peers {

// The version of the protocol, sent in the first message each way. A member that
// speaks another is refused with a message naming both (PEERS.md §6).
inline constexpr int kProtocolVersion = 1;

// The host's own channel: joining, leaving, keys, shared state. Nobody else may open it.
inline constexpr char kRoomChannel[] = "room";

// Who a member is. A peer's is its key's fingerprint and its instance, so it stays the
// same when it reconnects (PEERS.md §4); a guest's is the host's, while it is connected.
using PeerId = QString;

struct USDCC_PEERS_API Peer {
    enum class Kind {
        Host,   // The one that hosts the room and orders its messages
        Peer,   // A program or usdcc instance with a key pair
        Guest,  // A web page, or a program invited with the guests' secret (PEERS.md §6.1)
    };
    PeerId id;
    QString name;
    Kind kind = Kind::Peer;
    QString fingerprint;  // Of its public key; empty for a page
    QString instance;     // Which running instance of that key
    QString color;        // "#rrggbb"; empty for none
    QString app;          // What it is: "usdcc 0.3.0", a program's own name
    // Its Ed25519 public key, which the host passes on (its fingerprint is of this). Empty
    // for a guest, and for a peer whose host is older. Needed to send it something
    // privately (Channel::sendPrivate()).
    QByteArray publicKey;

    bool isValid() const { return !id.isEmpty(); }
    bool operator==(const Peer&) const = default;
};

// How a channel delivers (PEERS.md §7.1).
enum class Delivery {
    Ordered,     // Everyone gets every message, in the host's order. Comes back to its sender too
    LatestOnly,  // A newer message replaces an older one still waiting, of the same type and sender
    Direct,      // To one member; with no one named, to the host
};

// Who may send on a channel the host has opened, or write a map (PEERS.md §6). The
// host may always.
enum class Senders {
    Everyone,  // Every member the channel reaches (the default)
    Host,      // Only the host
    Named,     // The members the host named
};

// Who may join without the room's secret (PEERS.md §6). With it, anyone invited may.
enum class Admission {
    Invited,  // Only with the secret from a link. Not announced on the network
    Ask,      // Found on this machine or network: the host is asked (joinRequested())
    Open,     // Found on this machine or network: in at once. A public room, until there's a hub
};

// What a room is for (PEERS.md §5.5). The host says, in its welcome and announcement; a
// room that doesn't say (an older host) is a chat room.
enum class RoomType {
    Chat,         // People talking: the Peers panel's
    Broadcast,    // A viewport shown to watchers
    RenderQueue,  // Instances rendering each other's jobs (FINAL_RENDER.md §7)
};

// One message: the frame of PEERS.md §7.1.
struct USDCC_PEERS_API Message {
    QString channel;
    QString type;     // The channel's own
    PeerId from;      // Set by the host, never taken from the sender
    PeerId to;        // Empty for everyone
    qint64 seq = 0;   // The host's number for an ordered message to everyone; 0 for the others
    QJsonObject body;
    QByteArray bytes; // A body that isn't JSON (a picture), sent raw after the header
    // Sent with Channel::sendPrivate() and opened here: only its sender and this member
    // could read it, not the host that passed it on.
    bool sealed = false;
};

// A channel's name (PEERS.md §7): letters, digits and `.`, `_`, `-`, `/`, at most
// 128 characters, starting with a letter or digit, not `room`. Names without a dot are
// usdcc's own (`chat`, `presence`, `stage/<id>`); a plugin's start with its id. Maps
// (StateMap, PeerState) are named the same way.
USDCC_PEERS_API bool isValidChannelName(const QString& name);
USDCC_PEERS_API bool isUsdccChannel(const QString& name);

USDCC_PEERS_API QString kindName(Peer::Kind kind);
USDCC_PEERS_API QString admissionName(Admission admission);  // "invited", "ask", "open"
USDCC_PEERS_API Admission admissionFromName(const QString& name);
USDCC_PEERS_API QString roomTypeName(RoomType type);  // "chat", "broadcast", "render_queue"
USDCC_PEERS_API RoomType roomTypeFromName(const QString& name);  // Chat for anything else

}  // namespace usdcc::peers

Q_DECLARE_METATYPE(usdcc::peers::Peer)
Q_DECLARE_METATYPE(usdcc::peers::Message)
