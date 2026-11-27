#pragma once

#include "usdcc/peers/frame.h"
#include "usdcc/peers/room.h"

#include "crypto.h"
#include "send_queue.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QTcpServer;
class QWebSocket;

namespace usdcc::peers {

class PageServer;

// One connection to the host: a member once welcomed, before that one going through
// the handshake.
struct Member {
    enum class Phase { Hello, Auth, Asking, Welcomed };

    Peer peer;
    QWebSocket* socket = nullptr;
    std::unique_ptr<SendQueue> queue;
    Phase phase = Phase::Hello;
    bool browser = false;    // Came with an Origin: a page, so at most a guest
    bool encrypted = false;  // Did the handshake: everything after it is sealed
    bool roomKey = false;    // Has the room key: messages to everyone come under it
    Sealer tx;
    Sealer rx;
    QByteArray memberEph;
    QByteArray hostEph;
    bool gone = false;
    bool away = false;       // Lost its connection; kept for Limits::reconnectGraceMs
    QElapsedTimer awaySince;
    QString leaveReason;
    double tokens = 0;
    QElapsedTimer refill;
    QElapsedTimer heard;
    QPointer<JoinRequest> request;
    QJsonObject auth;        // What it said in `auth`, until it is let in
};

struct Room::Impl {
    explicit Impl(Room* room) : q(room) {}

    Room* q;
    bool isHost = false;
    State state = State::Connecting;
    QString name;
    RoomType type = RoomType::Chat;
    Profile profile;
    Peer me;
    Peer host;
    QMap<QString, Channel*> channels;
    QMap<QString, StateMap*> maps;
    QMap<QString, PeerState*> peerStates;
    QTimer* pingTimer = nullptr;

    // --- The host ---
    HostOptions options;
    class RelayBridge* relay = nullptr;  // Through a relay (HostOptions::relay)
    PageServer* http = nullptr;
    QTcpServer* tcp = nullptr;
    QString secret;
    QString guestSecret;
    std::vector<std::unique_ptr<Member>> members;
    QHash<QWebSocket*, Member*> bySocket;
    std::vector<std::unique_ptr<Member>> dead;
    std::vector<Member*> slow;
    qint64 seq = 0;
    bool closing = false;
    quint32 roomEpoch = 0;
    QByteArray roomKey;
    struct HistoryEntry {
        qint64 seq;
        QByteArray frame;
    };
    std::deque<HistoryEntry> history;
    qint64 historyBytes = 0;
    QSet<QString> admitted;  // Fingerprints let in once, for the room's life
    QHash<QString, qint64> mapBytes;

    bool listen(QString* error);
    void accept(QWebSocket* socket);
    void onMemberData(QWebSocket* socket, const QByteArray& data, bool text);
    void onMemberDisconnected(QWebSocket* socket);
    void onHello(Member* member, const Message& message);
    void onAuth(Member* member, const Message& message);
    void admit(Member* member, Peer::Kind kind);
    void welcome(Member* member, bool resumed, qint64 lastSeq);
    void sendSnapshots(Member* member);
    void deny(Member* member, const QString& reason);
    void onMemberControl(Member* member, const Message& message);
    void route(Message message, bool latest, Member* sender);
    void broadcast(Message message, bool ordered, const QString& latestKey, const Member* except,
                   const std::function<bool(const Member*)>& reaches);
    void sendTo(Member* member, const Message& message, const QString& latestKey = {});
    void enqueue(Member* member, const QByteArray& data, const QString& latestKey, bool seal);
    void dropSlowMembers();
    enum class Drop { Close, Abort };
    void dropMember(Member* member, const QString& reason, Drop how, const QString& tellType);
    void goAway(Member* member);
    void leave(Member* member, const QString& reason);
    void rotateRoomKey();
    void setState(StateMap* map, const QString& key, const QJsonValue& value, const Peer& by, Member* sender);
    void setPeerState(PeerState* state, const Peer& peer, const QJsonValue& value, const Member* sender);
    void forgetPeerState(const PeerId& id);
    Member* welcomedMember(const PeerId& id) const;
    QList<Member*> welcomedMembers(bool withAway = false) const;
    QString guestId() const;
    void hostTick();
    void settle(JoinRequest* request, bool allow, const QString& reason);

    // --- A member that joined ---
    QWebSocket* socket = nullptr;
    std::unique_ptr<SendQueue> queue;
    JoinOptions joinOptions;
    QString hostAddress;
    QList<Peer> joinedPeers;
    QElapsedTimer heard;
    QString endReason;
    bool challenged = false;
    bool ended = false;  // Denied, removed or closed: no reconnecting
    KxKeys kx;
    Sealer tx;
    Sealer rx;
    QByteArray hostKey;
    QHash<quint32, QByteArray> roomKeys;
    qint64 lastSeq = 0;
    bool everOpen = false;
    QElapsedTimer lostSince;
    int retries = 0;
    bool retryScheduled = false;

    void connectToHost();
    void onHostData(const QByteArray& data, bool text);
    void onChallenge(const Message& message);
    void onHostFrame(const Message& message);
    // Hands a message for this member to its channel; a sealed one opened first.
    void deliver(Channel* channel, Message message);
    bool sendPrivate(Channel* channel, const QString& type, const QJsonObject& body, const PeerId& to);
    QSet<QString> openedIds;  // Sealed messages already opened: a host can't replay one
    std::deque<QString> openedOrder;
    void onWelcome(const QJsonObject& body);
    void onDisconnected();
    void joinerTick();

    // --- Both ---
    void send(Channel* channel, const QString& type, const QJsonObject& body, const QByteArray& bytes,
              const PeerId& to);
    void sendState(StateMap* map, const QString& key, const QJsonValue& value);
    void sendPeerState(PeerState* state, const QJsonValue& value);
    void setRoomState(State next);
    void finish(const QString& reason);
    void shutdown();
    void removePeerLocally(const PeerId& id, const QString& reason);
};

QJsonObject peerJson(const Peer& peer);
Peer peerFromJson(const QJsonObject& object);
QString deliveryName(Delivery delivery);

}  // namespace usdcc::peers
