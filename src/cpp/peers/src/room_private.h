#pragma once

#include "usdcc/peers/frame.h"
#include "usdcc/peers/room.h"

#include "send_queue.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QPointer>
#include <QTimer>

#include <memory>
#include <optional>
#include <vector>

class QTcpServer;
class QWebSocket;

namespace usdcc::peers {

class PageServer;

// One connection to the host: a member once welcomed, before that only a socket that
// has to say hello.
struct Member {
    Peer peer;
    QWebSocket* socket = nullptr;
    std::unique_ptr<SendQueue> queue;
    bool browser = false;  // Came with an Origin: a page, so at most a guest
    bool welcomed = false;
    bool gone = false;
    QString leaveReason;
    double tokens = 0;
    QElapsedTimer refill;
    QElapsedTimer heard;
};

struct Room::Impl {
    explicit Impl(Room* room) : q(room) {}

    Room* q;
    bool isHost = false;
    State state = State::Connecting;
    QString name;
    Peer me;
    Peer host;
    QMap<QString, Channel*> channels;
    QTimer* pingTimer = nullptr;

    // --- The host ---
    HostOptions options;
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

    bool listen(QString* error);
    void accept(QWebSocket* socket);
    void onMemberFrame(QWebSocket* socket, std::optional<Frame> frame, const QString& error);
    void onMemberDisconnected(QWebSocket* socket);
    void hello(Member* member, const Message& message);
    void deny(Member* member, const QString& reason);
    void route(Message message, bool latest, Member* sender);
    void enqueue(Member* member, const QByteArray& frame, const QString& latestKey = {});
    void dropSlowMembers();
    void broadcastControl(const QString& type, const QJsonObject& body, const Member* except);
    enum class Drop { Close, Abort };
    void dropMember(Member* member, const QString& reason, Drop how, const QString& tellType);
    Member* welcomedMember(const PeerId& id) const;
    QList<Member*> welcomedMembers() const;
    QString newId() const;
    void hostTick();

    // --- A member that joined ---
    QWebSocket* socket = nullptr;
    std::unique_ptr<SendQueue> queue;
    QString joinSecret;
    JoinOptions joinOptions;
    QList<Peer> joinedPeers;
    QElapsedTimer heard;
    QString endReason;

    void onHostFrame(std::optional<Frame> frame, const QString& error);
    void joinerTick();

    // --- Both ---
    void send(Channel* channel, const QString& type, const QJsonObject& body, const QByteArray& bytes,
              const PeerId& to);
    void finish(const QString& reason);
    void shutdown();
};

QJsonObject peerJson(const Peer& peer);
Peer peerFromJson(const QJsonObject& object);
QString deliveryName(Delivery delivery);

}  // namespace usdcc::peers
