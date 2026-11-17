#pragma once

// The words of docs/PEERS.md: a peer, a message, and how a channel delivers.

#include "usdcc/peers/export.h"

#include <QByteArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>

namespace usdcc::peers {

// The version of the protocol, sent in the first message each way. A member that
// speaks another is refused with a message naming both (PEERS.md §6).
inline constexpr int kProtocolVersion = 1;

// The host's own channel: joining, leaving, refusals. Nobody else may open it.
inline constexpr char kRoomChannel[] = "room";

// Who a member is, as the host gave it. Phase 1a has no identity (PEERS.md §4): the
// id is the host's for as long as the member is connected.
using PeerId = QString;

struct USDCC_PEERS_API Peer {
    enum class Kind {
        Host,   // The one that hosts the room and orders its messages
        Peer,   // A program or usdcc instance, invited with the room's secret
        Guest,  // A web page, invited with the guests' secret (PEERS.md §6.1)
    };
    PeerId id;
    QString name;
    Kind kind = Kind::Peer;

    bool isValid() const { return !id.isEmpty(); }
    bool operator==(const Peer&) const = default;
};

// How a channel delivers (PEERS.md §7.1).
enum class Delivery {
    Ordered,     // Everyone gets every message, in the host's order. Comes back to its sender too
    LatestOnly,  // A newer message replaces an older one still waiting, of the same type and sender
    Direct,      // To one member; with no one named, to the host
};

// Who may send on a channel the host has opened (PEERS.md §6). The host may always.
enum class Senders {
    Everyone,  // Every member the channel reaches (the default)
    Host,      // Only the host
    Named,     // The members the host named
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
};

// A channel's name (PEERS.md §7): letters, digits and `.`, `_`, `-`, `/`, at most
// 128 characters, starting with a letter or digit, not `room`. Names without a dot are
// usdcc's own (`chat`, `presence`, `stage/<id>`); a plugin's start with its id.
USDCC_PEERS_API bool isValidChannelName(const QString& name);
USDCC_PEERS_API bool isUsdccChannel(const QString& name);

USDCC_PEERS_API QString kindName(Peer::Kind kind);

}  // namespace usdcc::peers

Q_DECLARE_METATYPE(usdcc::peers::Peer)
Q_DECLARE_METATYPE(usdcc::peers::Message)
