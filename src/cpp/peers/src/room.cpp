#include "usdcc/peers/room.h"

#include "usdcc/peers/channel.h"

#include "log.h"
#include "page_server.h"
#include "room_private.h"

#include <QJsonArray>
#include <QNetworkInterface>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QWebSocket>

#include <algorithm>

namespace usdcc::peers {

namespace {

constexpr qint64 kHelloFrameBytes = 64 * 1024;
constexpr int kNameLength = 64;
constexpr int kCloseGraceMs = 2000;

QString randomText(int bytes) {
    QByteArray data(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->generate(data.begin(), data.end());
    return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

// Compared without stopping at the first difference, so the time taken doesn't tell
// how much of a guess was right.
bool sameSecret(const QString& given, const QString& secret) {
    const QByteArray a = given.toUtf8();
    const QByteArray b = secret.toUtf8();
    if (b.isEmpty() || a.size() != b.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return difference == 0;
}

Message control(const QString& type, const QJsonObject& body) {
    Message message;
    message.channel = QLatin1String(kRoomChannel);
    message.type = type;
    message.body = body;
    return message;
}

QString latestKey(const Message& message) {
    return message.channel + u'\n' + message.type + u'\n' + message.from + u'\n' + message.to;
}

QString cleanName(const QString& name, const QString& fallback) {
    QString result = name.simplified().left(kNameLength);
    return result.isEmpty() ? fallback : result;
}

}  // namespace

QJsonObject peerJson(const Peer& peer) {
    return {{QStringLiteral("id"), peer.id},
            {QStringLiteral("name"), peer.name},
            {QStringLiteral("kind"), kindName(peer.kind)}};
}

Peer peerFromJson(const QJsonObject& object) {
    Peer peer;
    peer.id = object.value(QLatin1String("id")).toString();
    peer.name = object.value(QLatin1String("name")).toString();
    const QString kind = object.value(QLatin1String("kind")).toString();
    peer.kind = kind == u"host" ? Peer::Kind::Host : kind == u"guest" ? Peer::Kind::Guest : Peer::Kind::Peer;
    return peer;
}

QString deliveryName(Delivery delivery) {
    switch (delivery) {
    case Delivery::Ordered:
        return QStringLiteral("ordered");
    case Delivery::LatestOnly:
        return QStringLiteral("latest");
    case Delivery::Direct:
        return QStringLiteral("direct");
    }
    return {};
}

// ---------------------------------------------------------------------------------
// Room

Room::Room(QObject* parent) : QObject(parent), d(std::make_unique<Impl>(this)) {}

Room::~Room() {
    d->shutdown();
    // Before d goes: a channel's destructor looks for its room's list.
    const QList<Channel*> channels = d->channels.values();
    d->channels.clear();
    qDeleteAll(channels);
}

Room* Room::host(const HostOptions& options, QObject* parent, QString* error) {
    auto* room = new Room(parent);
    Impl& d = *room->d;
    d.isHost = true;
    d.options = options;
    d.name = options.name;
    d.secret = randomText(16);
    d.guestSecret = options.guests ? randomText(16) : QString();
    d.me = {d.newId(), cleanName(options.hostName, QStringLiteral("Host")), Peer::Kind::Host};
    d.host = d.me;
    if (!d.listen(error)) {
        delete room;
        return nullptr;
    }
    d.state = State::Open;
    d.pingTimer = new QTimer(room);
    d.pingTimer->setInterval(std::max(100, std::min(options.limits.pingIntervalMs, options.limits.helloTimeoutMs)));
    QObject::connect(d.pingTimer, &QTimer::timeout, room, [&d] { d.hostTick(); });
    d.pingTimer->start();
    qCInfo(lcPeers).noquote() << "Hosting room" << d.name << "on" << options.address.toString() << "port"
                              << d.tcp->serverPort() << (options.guests ? "with guests" : "without guests");
    return room;
}

Room* Room::join(const QString& address, const QString& secret, const JoinOptions& options, QObject* parent) {
    auto* room = new Room(parent);
    Impl& d = *room->d;
    d.joinSecret = secret;
    d.joinOptions = options;
    d.socket = new QWebSocket(options.origin, QWebSocketProtocol::VersionLatest, room);
    d.queue = std::make_unique<SendQueue>(d.socket);
    d.queue->setOpen(false);
    QWebSocket* socket = d.socket;
    QObject::connect(socket, &QWebSocket::connected, room, [&d] {
        d.heard.start();
        Message hello = control(QStringLiteral("hello"),
                                {{QStringLiteral("protocol"), kProtocolVersion},
                                 {QStringLiteral("secret"), d.joinSecret},
                                 {QStringLiteral("name"), cleanName(d.joinOptions.name, QStringLiteral("Peer"))}});
        d.socket->sendBinaryMessage(encodeFrame(hello));
    });
    QObject::connect(socket, &QWebSocket::binaryMessageReceived, room, [&d](const QByteArray& data) {
        QString error;
        std::optional<Frame> frame = decodeFrame(data, &error);
        d.onHostFrame(std::move(frame), error);
    });
    QObject::connect(socket, &QWebSocket::textMessageReceived, room, [&d](const QString& text) {
        QString error;
        std::optional<Frame> frame = decodeTextFrame(text, &error);
        d.onHostFrame(std::move(frame), error);
    });
    QObject::connect(socket, &QWebSocket::bytesWritten, room, [&d] { d.queue->pump(); });
    QObject::connect(socket, &QWebSocket::pong, room, [&d] { d.heard.restart(); });
    QObject::connect(socket, &QWebSocket::disconnected, room, [&d] {
        d.finish(d.endReason.isEmpty() ? QStringLiteral("The connection to the host was lost") : d.endReason);
    });
    QObject::connect(socket, &QWebSocket::errorOccurred, room, [&d](QAbstractSocket::SocketError) {
        // Before the welcome, nothing else will end it: a refused connection emits no
        // disconnected().
        if (d.state == State::Connecting) {
            if (d.endReason.isEmpty()) {
                d.endReason = QStringLiteral("Couldn't reach the room: %1").arg(d.socket->errorString());
            }
            d.finish(d.endReason);
        }
    });
    d.pingTimer = new QTimer(room);
    d.pingTimer->setInterval(std::max(100, options.pingIntervalMs));
    QObject::connect(d.pingTimer, &QTimer::timeout, room, [&d] { d.joinerTick(); });
    d.pingTimer->start();

    // From the event loop: a connection refused at once would otherwise close the
    // room before the caller could connect to closed().
    const QUrl url(QStringLiteral("ws://") + address + QStringLiteral("/ws"));
    QTimer::singleShot(0, socket, [socket, url] { socket->open(url); });
    return room;
}

Room::State Room::state() const {
    return d->state;
}

bool Room::isHost() const {
    return d->isHost;
}

QString Room::name() const {
    return d->name;
}

Peer Room::me() const {
    return d->me;
}

Peer Room::hostPeer() const {
    return d->host;
}

QList<Peer> Room::peers() const {
    if (!d->isHost) {
        return d->joinedPeers;
    }
    QList<Peer> result{d->me};
    for (const Member* member : d->welcomedMembers()) {
        result.append(member->peer);
    }
    return result;
}

Peer Room::peer(const PeerId& id) const {
    for (const Peer& peer : peers()) {
        if (peer.id == id) {
            return peer;
        }
    }
    return {};
}

Channel* Room::channel(const QString& name, Delivery delivery) {
    if (Channel* existing = d->channels.value(name)) {
        if (existing->delivery() != delivery) {
            qCWarning(lcPeers).noquote() << "Channel" << name << "is already open with another delivery";
        }
        return existing;
    }
    if (!isValidChannelName(name)) {
        qCWarning(lcPeers).noquote() << "Not a channel name:" << name;
        return nullptr;
    }
    auto* channel = new Channel(this, name, delivery);
    d->channels.insert(name, channel);
    return channel;
}

Channel* Room::findChannel(const QString& name) const {
    return d->channels.value(name);
}

void Room::close(const QString& reason) {
    if (d->state == State::Closed) {
        return;
    }
    const QString why = reason.isEmpty() ? (d->isHost ? QStringLiteral("The host closed the room")
                                                      : QStringLiteral("Left the room"))
                                         : reason;
    if (d->isHost) {
        qCInfo(lcPeers).noquote() << "Closing room" << d->name << "-" << why;
        d->closing = true;  // Members hear "closed", not each other leave
        std::vector<Member*> all;
        for (const auto& member : d->members) {
            all.push_back(member.get());
        }
        for (Member* member : all) {
            if (member->welcomed) {
                d->dropMember(member, why, Impl::Drop::Close, QStringLiteral("closed"));
            } else {
                d->dropMember(member, why, Impl::Drop::Abort, {});
            }
        }
    } else if (d->socket) {
        if (d->state == State::Open) {
            d->socket->sendBinaryMessage(encodeFrame(control(QStringLiteral("leave"), {{QStringLiteral("reason"), why}})));
        }
        QObject::disconnect(d->socket, nullptr, this, nullptr);
        d->socket->close(QWebSocketProtocol::CloseCodeNormal, why);
    }
    d->finish(why);
}

quint16 Room::port() const {
    return d->tcp ? d->tcp->serverPort() : 0;
}

QString Room::secret() const {
    return d->secret;
}

QString Room::guestSecret() const {
    return d->guestSecret;
}

QUrl Room::guestLink(const QString& address) const {
    if (!d->isHost || d->guestSecret.isEmpty()) {
        return {};
    }
    QString hostName = address;
    if (hostName.isEmpty()) {
        const QHostAddress listening = d->options.address;
        if (listening.isLoopback()) {
            hostName = QStringLiteral("127.0.0.1");
        } else if (listening == QHostAddress::Any || listening == QHostAddress::AnyIPv4 ||
                   listening == QHostAddress::AnyIPv6) {
            hostName = QStringLiteral("127.0.0.1");
            for (const QHostAddress& candidate : QNetworkInterface::allAddresses()) {
                if (!candidate.isLoopback() && candidate.protocol() == QAbstractSocket::IPv4Protocol &&
                    !candidate.isLinkLocal()) {
                    hostName = candidate.toString();
                    break;
                }
            }
        } else {
            hostName = listening.toString();
        }
    }
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(hostName);
    url.setPort(port());
    url.setPath(QStringLiteral("/"));
    url.setFragment(QStringLiteral("s=") + d->guestSecret);
    return url;
}

void Room::remove(const PeerId& id, const QString& reason) {
    if (!d->isHost) {
        return;
    }
    if (Member* member = d->welcomedMember(id)) {
        const QString why = reason.isEmpty() ? QStringLiteral("Removed by the host") : reason;
        qCInfo(lcPeers).noquote() << "Removing" << member->peer.name << member->peer.id << "-" << why;
        d->dropMember(member, why, Impl::Drop::Close, QStringLiteral("removed"));
    }
}

qint64 Room::queuedBytes(const PeerId& id) const {
    const Member* member = d->welcomedMember(id);
    return member ? member->queue->queuedBytes() : 0;
}

// ---------------------------------------------------------------------------------
// The host

bool Room::Impl::listen(QString* error) {
    tcp = new QTcpServer(q);
    if (!tcp->listen(options.address, options.port)) {
        if (error) {
            *error = QStringLiteral("Couldn't listen on %1 port %2: %3")
                         .arg(options.address.toString())
                         .arg(options.port)
                         .arg(tcp->errorString());
        }
        return false;
    }
    http = new PageServer(q);
    http->guests = options.guests;
    http->page = options.page;
    http->pageDirectory = options.pageDirectory;
    http->allowedOrigins = options.allowedOrigins;
    if (!http->bind(tcp)) {
        if (error) {
            *error = QStringLiteral("Couldn't serve the room's port");
        }
        return false;
    }
    QObject::connect(http, &QAbstractHttpServer::newWebSocketConnection, q, [this] {
        while (http->hasPendingWebSocketConnections()) {
            std::unique_ptr<QWebSocket> socket = http->nextPendingWebSocketConnection();
            if (socket) {
                accept(socket.release());
            }
        }
    });
    return true;
}

void Room::Impl::accept(QWebSocket* socket) {
    socket->setParent(q);
    if (state != State::Open) {
        socket->abort();
        socket->deleteLater();
        return;
    }
    auto member = std::make_unique<Member>();
    member->socket = socket;
    member->queue = std::make_unique<SendQueue>(socket);
    member->browser = !socket->origin().isEmpty();
    member->tokens = options.limits.messageBurst;
    member->refill.start();
    member->heard.start();
    socket->setMaxAllowedIncomingFrameSize(kHelloFrameBytes);
    socket->setMaxAllowedIncomingMessageSize(kHelloFrameBytes);
    bySocket.insert(socket, member.get());
    members.push_back(std::move(member));

    QObject::connect(socket, &QWebSocket::binaryMessageReceived, q, [this, socket](const QByteArray& data) {
        QString error;
        std::optional<Frame> frame = decodeFrame(data, &error);
        onMemberFrame(socket, std::move(frame), error);
    });
    QObject::connect(socket, &QWebSocket::textMessageReceived, q, [this, socket](const QString& text) {
        QString error;
        std::optional<Frame> frame = decodeTextFrame(text, &error);
        onMemberFrame(socket, std::move(frame), error);
    });
    QObject::connect(socket, &QWebSocket::bytesWritten, q, [this, socket] {
        if (Member* member = bySocket.value(socket)) {
            member->queue->pump();
        }
    });
    QObject::connect(socket, &QWebSocket::pong, q, [this, socket] {
        if (Member* member = bySocket.value(socket)) {
            member->heard.restart();
        }
    });
    QObject::connect(socket, &QWebSocket::disconnected, q, [this, socket] { onMemberDisconnected(socket); });
}

void Room::Impl::onMemberFrame(QWebSocket* socket, std::optional<Frame> frame, const QString& error) {
    Member* member = bySocket.value(socket);
    if (!member || member->gone) {
        return;
    }
    member->heard.restart();

    // The rate limit (PEERS.md §7.4): a bucket that fills at the allowed rate.
    const Limits& limits = options.limits;
    member->tokens = std::min<double>(limits.messageBurst,
                                      member->tokens + member->refill.restart() * limits.messagesPerSecond / 1000.0);
    if (member->tokens < 1) {
        dropMember(member, QStringLiteral("Sent too many messages"), Drop::Abort, {});
        return;
    }
    member->tokens -= 1;

    if (!frame) {
        dropMember(member, QStringLiteral("Sent something that isn't a frame: %1").arg(error), Drop::Close,
                   QStringLiteral("removed"));
        return;
    }
    const Message& message = frame->message;
    if (!member->welcomed) {
        if (message.channel == QLatin1String(kRoomChannel) && message.type == u"hello") {
            hello(member, message);
        } else {
            deny(member, QStringLiteral("The first message has to be hello"));
        }
        return;
    }
    if (message.channel == QLatin1String(kRoomChannel)) {
        if (message.type == u"leave") {
            member->leaveReason = message.body.value(QLatin1String("reason")).toString().left(200);
            if (member->leaveReason.isEmpty()) {
                member->leaveReason = QStringLiteral("Left");
            }
            dropMember(member, member->leaveReason, Drop::Close, {});
        }
        return;  // Nothing else on the room's channel is a member's to send
    }
    route(message, frame->latest, member);
}

void Room::Impl::onMemberDisconnected(QWebSocket* socket) {
    Member* member = bySocket.value(socket);
    if (!member || member->gone) {
        return;
    }
    QString reason = member->leaveReason;
    if (reason.isEmpty()) {
        reason = socket->closeCode() == QWebSocketProtocol::CloseCodeTooMuchData
                     ? QStringLiteral("Sent a message over the size limit")
                     : QStringLiteral("The connection was lost");
    }
    dropMember(member, reason, Drop::Abort, {});
}

void Room::Impl::hello(Member* member, const Message& message) {
    const int protocol = message.body.value(QLatin1String("protocol")).toInt(0);
    if (protocol != kProtocolVersion) {
        deny(member, QStringLiteral("This room speaks protocol %1, and this member protocol %2")
                         .arg(kProtocolVersion)
                         .arg(protocol));
        return;
    }
    const QString given = message.body.value(QLatin1String("secret")).toString();
    std::optional<Peer::Kind> kind;
    // A page can only be a guest, whatever secret it has.
    if (!member->browser && sameSecret(given, secret)) {
        kind = Peer::Kind::Peer;
    } else if (options.guests && sameSecret(given, guestSecret)) {
        kind = Peer::Kind::Guest;
    }
    if (!kind) {
        deny(member, QStringLiteral("Wrong secret: the link isn't this room's"));
        return;
    }
    const QList<Member*> present = welcomedMembers();
    if (present.size() >= options.limits.maxMembers) {
        deny(member, QStringLiteral("The room is full"));
        return;
    }
    if (*kind == Peer::Kind::Guest &&
        std::count_if(present.begin(), present.end(),
                      [](const Member* m) { return m->peer.kind == Peer::Kind::Guest; }) >= options.limits.maxGuests) {
        deny(member, QStringLiteral("The room has as many guests as it takes"));
        return;
    }

    const QString fallback = *kind == Peer::Kind::Guest ? QStringLiteral("Guest") : QStringLiteral("Peer");
    member->peer = {newId(), cleanName(message.body.value(QLatin1String("name")).toString(), fallback), *kind};
    member->welcomed = true;
    member->socket->setMaxAllowedIncomingFrameSize(quint64(options.limits.maxFrameBytes));
    member->socket->setMaxAllowedIncomingMessageSize(quint64(options.limits.maxFrameBytes));

    QJsonArray peerList;
    for (const Peer& peer : q->peers()) {
        peerList.append(peerJson(peer));
    }
    QJsonArray channelList;
    for (const Channel* channel : std::as_const(channels)) {
        if (*kind != Peer::Kind::Guest || channel->isOpenToGuests()) {
            channelList.append(QJsonObject{{QStringLiteral("name"), channel->name()},
                                           {QStringLiteral("delivery"), deliveryName(channel->delivery())}});
        }
    }
    const QJsonObject welcome{{QStringLiteral("protocol"), kProtocolVersion},
                              {QStringLiteral("id"), member->peer.id},
                              {QStringLiteral("name"), name},
                              {QStringLiteral("host"), me.id},
                              {QStringLiteral("seq"), seq},
                              {QStringLiteral("peers"), peerList},
                              {QStringLiteral("channels"), channelList}};
    member->queue->push(encodeFrame(control(QStringLiteral("welcome"), welcome)));
    qCInfo(lcPeers).noquote() << kindName(*kind) << member->peer.name << member->peer.id << "joined room" << name;
    broadcastControl(QStringLiteral("joined"), peerJson(member->peer), member);
    const Peer peer = member->peer;
    Q_EMIT q->peerJoined(peer);
}

void Room::Impl::deny(Member* member, const QString& reason) {
    qCInfo(lcPeers).noquote() << "Refused a member:" << reason;
    dropMember(member, reason, Drop::Close, QStringLiteral("denied"));
}

void Room::Impl::route(Message message, bool latest, Member* sender) {
    const Peer from = sender ? sender->peer : me;
    message.from = from.id;
    message.seq = 0;
    Channel* channel = channels.value(message.channel);
    const auto refuse = [&](const QString& reason) {
        if (sender) {
            enqueue(sender, encodeFrame(control(QStringLiteral("refused"),
                                                {{QStringLiteral("channel"), message.channel},
                                                 {QStringLiteral("type"), message.type},
                                                 {QStringLiteral("reason"), reason}})));
            dropSlowMembers();
        } else if (channel) {
            Q_EMIT channel->refused(message.type, reason);
        }
    };
    if (!isValidChannelName(message.channel)) {
        refuse(QStringLiteral("Not a channel name"));
        return;
    }
    if (from.kind == Peer::Kind::Guest && !(channel && channel->isOpenToGuests())) {
        refuse(QStringLiteral("This channel isn't open to guests"));
        return;
    }
    if (channel && !channel->maySend(from)) {
        refuse(QStringLiteral("Not allowed to send on this channel"));
        return;
    }
    const Delivery delivery = channel ? channel->delivery() : latest ? Delivery::LatestOnly : Delivery::Ordered;
    if (message.to.isEmpty() && delivery == Delivery::Direct) {
        message.to = me.id;
    }
    const auto reaches = [&](const Member* member) {
        return member->peer.kind != Peer::Kind::Guest || (channel && channel->isOpenToGuests());
    };

    if (!message.to.isEmpty()) {
        if (message.to == me.id) {
            if (channel) {
                Q_EMIT channel->received(message);
            }
            return;
        }
        Member* target = welcomedMember(message.to);
        if (!target) {
            refuse(QStringLiteral("Nobody in the room has this id"));
            return;
        }
        if (!reaches(target)) {
            refuse(QStringLiteral("This channel isn't open to guests"));
            return;
        }
        enqueue(target, encodeFrame(message),
                delivery == Delivery::LatestOnly ? latestKey(message) : QString());
        dropSlowMembers();
        return;
    }

    if (delivery == Delivery::LatestOnly) {
        const QByteArray frame = encodeFrame(message);
        const QString key = latestKey(message);
        for (Member* member : welcomedMembers()) {
            if (member != sender && reaches(member)) {
                enqueue(member, frame, key);
            }
        }
        dropSlowMembers();
        if (sender && channel) {
            Q_EMIT channel->received(message);
        }
        return;
    }

    // Ordered: numbered here, which is what makes it one order for everyone, the
    // sender included.
    message.seq = ++seq;
    const QByteArray frame = encodeFrame(message);
    for (Member* member : welcomedMembers()) {
        if (reaches(member)) {
            enqueue(member, frame);
        }
    }
    dropSlowMembers();
    if (channel) {
        Q_EMIT channel->received(message);
    }
}

void Room::Impl::enqueue(Member* member, const QByteArray& frame, const QString& latestKey) {
    if (member->gone) {
        return;
    }
    member->queue->push(frame, latestKey);
    if (member->queue->queuedBytes() > options.limits.maxQueuedBytes &&
        std::find(slow.begin(), slow.end(), member) == slow.end()) {
        slow.push_back(member);
    }
}

void Room::Impl::dropSlowMembers() {
    while (!slow.empty()) {
        Member* member = slow.back();
        slow.pop_back();
        if (!member->gone) {
            qCInfo(lcPeers).noquote() << member->peer.name << member->peer.id << "fell behind by"
                                      << member->queue->queuedBytes() << "bytes";
            dropMember(member, QStringLiteral("Too slow: fell too far behind"), Drop::Abort, {});
        }
    }
}

void Room::Impl::broadcastControl(const QString& type, const QJsonObject& body, const Member* except) {
    Message message = control(type, body);
    message.from = me.id;
    message.seq = ++seq;
    const QByteArray frame = encodeFrame(message);
    for (Member* member : welcomedMembers()) {
        if (member != except) {
            enqueue(member, frame);
        }
    }
    dropSlowMembers();
}

void Room::Impl::dropMember(Member* member, const QString& reason, Drop how, const QString& tellType) {
    if (member->gone) {
        return;
    }
    member->gone = true;
    QWebSocket* socket = member->socket;
    QObject::disconnect(socket, nullptr, q, nullptr);
    member->queue->clear();
    bySocket.remove(socket);
    if (!tellType.isEmpty() && socket->isValid()) {
        socket->sendBinaryMessage(encodeFrame(control(tellType, {{QStringLiteral("reason"), reason},
                                                                 {QStringLiteral("protocol"), kProtocolVersion}})));
    }
    if (how == Drop::Abort || !socket->isValid()) {
        socket->abort();
        socket->deleteLater();
    } else {
        QObject::connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
        const bool turnedAway = tellType == u"denied" || tellType == u"removed";
        socket->close(turnedAway ? QWebSocketProtocol::CloseCodePolicyViolated : QWebSocketProtocol::CloseCodeNormal,
                      reason.left(120));
        // A member that doesn't answer the close is cut off.
        QTimer::singleShot(kCloseGraceMs, socket, [socket] {
            socket->abort();
            socket->deleteLater();
        });
    }

    const auto found = std::find_if(members.begin(), members.end(), [member](const auto& m) { return m.get() == member; });
    std::unique_ptr<Member> owned;
    if (found != members.end()) {
        owned = std::move(*found);
        members.erase(found);
    }
    // Deleted later: a caller up the stack may still hold the pointer.
    if (owned) {
        dead.push_back(std::move(owned));
        QTimer::singleShot(0, q, [this] { dead.clear(); });
    }

    if (member->welcomed && !closing) {
        broadcastControl(QStringLiteral("left"),
                         {{QStringLiteral("id"), member->peer.id}, {QStringLiteral("reason"), reason}}, nullptr);
        const Peer peer = member->peer;
        Q_EMIT q->peerLeft(peer, reason);
    }
}

Member* Room::Impl::welcomedMember(const PeerId& id) const {
    for (const auto& member : members) {
        if (member->welcomed && !member->gone && member->peer.id == id) {
            return member.get();
        }
    }
    return nullptr;
}

QList<Member*> Room::Impl::welcomedMembers() const {
    QList<Member*> result;
    for (const auto& member : members) {
        if (member->welcomed && !member->gone) {
            result.append(member.get());
        }
    }
    return result;
}

QString Room::Impl::newId() const {
    for (;;) {
        const QString id = QString::number(QRandomGenerator::system()->generate64() & 0xffffffffffffULL, 36);
        if (id != me.id && !welcomedMember(id)) {
            return id;
        }
    }
}

void Room::Impl::hostTick() {
    const Limits& limits = options.limits;
    QList<Member*> all;
    for (const auto& member : members) {
        all.append(member.get());
    }
    for (Member* member : all) {
        if (member->gone) {
            continue;
        }
        if (!member->welcomed) {
            if (member->heard.elapsed() > limits.helloTimeoutMs) {
                deny(member, QStringLiteral("Said no hello"));
            }
        } else if (member->heard.elapsed() > limits.silenceTimeoutMs) {
            dropMember(member, QStringLiteral("Silent for too long"), Drop::Abort, {});
        } else if (member->socket->isValid()) {
            member->socket->ping();
        }
    }
}

// ---------------------------------------------------------------------------------
// A member that joined

void Room::Impl::onHostFrame(std::optional<Frame> frame, const QString& error) {
    heard.restart();
    if (!frame) {
        qCWarning(lcPeers).noquote() << "The host sent something that isn't a frame:" << error;
        return;
    }
    const Message& message = frame->message;
    if (message.channel == QLatin1String(kRoomChannel)) {
        const QJsonObject& body = message.body;
        const QString reason = body.value(QLatin1String("reason")).toString();
        if (message.type == u"welcome") {
            if (state != State::Connecting) {
                return;
            }
            name = body.value(QLatin1String("name")).toString();
            const PeerId id = body.value(QLatin1String("id")).toString();
            const PeerId hostId = body.value(QLatin1String("host")).toString();
            joinedPeers.clear();
            for (const QJsonValue& value : body.value(QLatin1String("peers")).toArray()) {
                const Peer peer = peerFromJson(value.toObject());
                joinedPeers.append(peer);
                if (peer.id == id) {
                    me = peer;
                }
                if (peer.id == hostId) {
                    host = peer;
                }
            }
            state = State::Open;
            queue->setOpen(true);
            Q_EMIT q->opened();
        } else if (message.type == u"joined") {
            const Peer peer = peerFromJson(body);
            if (peer.isValid() && peer.id != me.id) {
                joinedPeers.append(peer);
                Q_EMIT q->peerJoined(peer);
            }
        } else if (message.type == u"left") {
            const PeerId id = body.value(QLatin1String("id")).toString();
            for (qsizetype i = 0; i < joinedPeers.size(); ++i) {
                if (joinedPeers[i].id == id) {
                    const Peer peer = joinedPeers.takeAt(i);
                    Q_EMIT q->peerLeft(peer, reason);
                    break;
                }
            }
        } else if (message.type == u"denied" || message.type == u"removed" || message.type == u"closed") {
            endReason = reason.isEmpty() ? QStringLiteral("The host ended the connection") : reason;
            finish(endReason);
        } else if (message.type == u"refused") {
            if (Channel* channel = channels.value(body.value(QLatin1String("channel")).toString())) {
                Q_EMIT channel->refused(body.value(QLatin1String("type")).toString(), reason);
            }
        }
        return;
    }
    if (state != State::Open) {
        return;
    }
    if (Channel* channel = channels.value(message.channel)) {
        Q_EMIT channel->received(message);
    }
}

void Room::Impl::joinerTick() {
    if (state == State::Closed || !socket || !socket->isValid()) {
        return;
    }
    if (heard.isValid() && heard.elapsed() > joinOptions.silenceTimeoutMs) {
        endReason = QStringLiteral("The host went silent");
        socket->abort();
        return;
    }
    socket->ping();
}

// ---------------------------------------------------------------------------------
// Both

void Room::Impl::send(Channel* channel, const QString& type, const QJsonObject& body, const QByteArray& bytes,
                      const PeerId& to) {
    if (state == State::Closed) {
        qCWarning(lcPeers).noquote() << "Sent on" << channel->name() << "after the room closed";
        return;
    }
    Message message;
    message.channel = channel->name();
    message.type = type;
    message.body = body;
    message.bytes = bytes;
    message.to = to;
    const bool latest = channel->delivery() == Delivery::LatestOnly;
    if (isHost) {
        route(message, latest, nullptr);
        return;
    }
    if (message.to.isEmpty() && channel->delivery() == Delivery::Direct) {
        message.to = host.id;  // Unknown before the welcome; the host's own Direct channel then decides
    }
    queue->push(encodeFrame(message, latest), latest ? latestKey(message) : QString());
}

void Room::Impl::finish(const QString& reason) {
    if (state == State::Closed) {
        return;
    }
    state = State::Closed;
    if (pingTimer) {
        pingTimer->stop();
    }
    if (isHost && tcp) {
        tcp->close();
    }
    if (queue) {
        queue->clear();
    }
    Q_EMIT q->closed(reason);
}

void Room::Impl::shutdown() {
    // Quietly: the room is being destroyed, so no signals.
    if (pingTimer) {
        pingTimer->stop();
    }
    for (const auto& member : members) {
        QObject::disconnect(member->socket, nullptr, q, nullptr);
        member->socket->abort();
    }
    members.clear();
    bySocket.clear();
    dead.clear();
    if (socket) {
        QObject::disconnect(socket, nullptr, q, nullptr);
        socket->abort();
    }
    if (tcp) {
        tcp->close();
    }
    state = State::Closed;
}

}  // namespace usdcc::peers
