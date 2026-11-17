#include "usdcc/peers/room.h"

#include "usdcc/peers/channel.h"
#include "usdcc/peers/state.h"

#include "log.h"
#include "page_server.h"
#include "room_private.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QWebSocket>
#include <QtEndian>

#include <algorithm>

namespace usdcc::peers {

namespace {

constexpr qint64 kHandshakeFrameBytes = 64 * 1024;
constexpr int kNameLength = 64;
constexpr int kCloseGraceMs = 2000;
const QByteArray kHostProof = "usdcc-peers-1 host";
const QByteArray kPeerProof = "usdcc-peers-1 peer";

QString randomSecret() {
    return QString::fromLatin1(
        randomBytes(16).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString b64(const QByteArray& data) {
    return QString::fromLatin1(data.toBase64());
}

QByteArray unb64(const QJsonValue& value) {
    return QByteArray::fromBase64(value.toString().toLatin1());
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

QString cleanColor(const QString& color) {
    static const QRegularExpression pattern(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return pattern.match(color).hasMatch() ? color.toLower() : QString();
}

qint64 jsonSize(const QJsonValue& value) {
    return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).size();
}

qint64 qtLimit(const Limits& limits) {
    return std::max<qint64>(16 * 1024 * 1024, 2 * limits.maxFrameBytes);
}

QString firstNetworkAddress() {
    for (const QHostAddress& candidate : QNetworkInterface::allAddresses()) {
        if (!candidate.isLoopback() && candidate.protocol() == QAbstractSocket::IPv4Protocol &&
            !candidate.isLinkLocal()) {
            return candidate.toString();
        }
    }
    return QStringLiteral("127.0.0.1");
}

}  // namespace

QJsonObject peerJson(const Peer& peer) {
    QJsonObject result{{QStringLiteral("id"), peer.id},
                       {QStringLiteral("name"), peer.name},
                       {QStringLiteral("kind"), kindName(peer.kind)}};
    if (!peer.fingerprint.isEmpty()) {
        result.insert(QStringLiteral("fingerprint"), peer.fingerprint);
        result.insert(QStringLiteral("instance"), peer.instance);
    }
    if (!peer.color.isEmpty()) {
        result.insert(QStringLiteral("color"), peer.color);
    }
    if (!peer.app.isEmpty()) {
        result.insert(QStringLiteral("app"), peer.app);
    }
    return result;
}

Peer peerFromJson(const QJsonObject& object) {
    Peer peer;
    peer.id = object.value(QLatin1String("id")).toString();
    peer.name = object.value(QLatin1String("name")).toString();
    const QString kind = object.value(QLatin1String("kind")).toString();
    peer.kind = kind == u"host" ? Peer::Kind::Host : kind == u"guest" ? Peer::Kind::Guest : Peer::Kind::Peer;
    peer.fingerprint = object.value(QLatin1String("fingerprint")).toString();
    peer.instance = object.value(QLatin1String("instance")).toString();
    peer.color = object.value(QLatin1String("color")).toString();
    peer.app = object.value(QLatin1String("app")).toString();
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

QString makeJoinLink(const JoinLink& link) {
    QUrlQuery fragment;
    fragment.addQueryItem(QStringLiteral("k"), link.fingerprint);
    fragment.addQueryItem(QStringLiteral("s"), link.secret);
    return QStringLiteral("usdcc://join/") + link.address + u'#' + fragment.toString(QUrl::FullyEncoded);
}

std::optional<JoinLink> parseJoinLink(const QString& text) {
    const QString trimmed = text.trimmed();
    const QString prefix = QStringLiteral("usdcc://join/");
    if (!trimmed.startsWith(prefix, Qt::CaseInsensitive)) {
        return std::nullopt;
    }
    const qsizetype hash = trimmed.indexOf(u'#');
    JoinLink link;
    link.address = trimmed.mid(prefix.size(), hash < 0 ? -1 : hash - prefix.size());
    if (link.address.endsWith(u'/')) {
        link.address.chop(1);
    }
    if (hash >= 0) {
        const QUrlQuery fragment(trimmed.mid(hash + 1));
        link.fingerprint = fragment.queryItemValue(QStringLiteral("k"), QUrl::FullyDecoded);
        link.secret = fragment.queryItemValue(QStringLiteral("s"), QUrl::FullyDecoded);
    }
    const qsizetype colon = link.address.lastIndexOf(u':');
    bool portOk = false;
    if (colon > 0) {
        link.address.mid(colon + 1).toUShort(&portOk);
    }
    if (!portOk) {
        return std::nullopt;
    }
    return link;
}

// ---------------------------------------------------------------------------------
// JoinRequest

JoinRequest::JoinRequest(Room* room, const Peer& peer) : QObject(room), m_room(room), m_peer(peer) {}

void JoinRequest::allow() {
    if (!m_settled) {
        m_room->d->settle(this, true, {});
    }
}

void JoinRequest::deny(const QString& reason) {
    if (!m_settled) {
        m_room->d->settle(this, false, reason);
    }
}

// ---------------------------------------------------------------------------------
// Room

Room::Room(QObject* parent) : QObject(parent), d(std::make_unique<Impl>(this)) {}

Room::~Room() {
    d->shutdown();
    // Before d goes: their destructors look for their room's lists.
    const QList<Channel*> channels = d->channels.values();
    d->channels.clear();
    qDeleteAll(channels);
}

namespace {

Profile completed(Profile profile, const QString& fallbackName) {
    if (!profile.identity.isValid()) {
        profile.identity = Identity::generate();
    }
    if (profile.instance.isEmpty()) {
        profile.instance = randomId(6);
    }
    profile.name = cleanName(profile.name, fallbackName);
    profile.color = cleanColor(profile.color);
    return profile;
}

Peer peerOf(const Profile& profile, Peer::Kind kind) {
    const QString fingerprint = profile.identity.fingerprint();
    return {fingerprint + u'-' + profile.instance, profile.name, kind, fingerprint, profile.instance, profile.color,
            profile.app};
}

}  // namespace

Room* Room::host(const HostOptions& options, QObject* parent, QString* error) {
    auto* room = new Room(parent);
    Impl& d = *room->d;
    d.isHost = true;
    d.options = options;
    d.name = options.name;
    d.profile = completed(options.profile, QStringLiteral("Host"));
    d.secret = randomSecret();
    d.guestSecret = options.guests ? randomSecret() : QString();
    d.me = peerOf(d.profile, Peer::Kind::Host);
    d.host = d.me;
    d.roomEpoch = 1;
    d.roomKey = randomBytes(32);
    for (const QString& fingerprint : options.trustedFingerprints) {
        d.admitted.insert(fingerprint);
    }
    if (!d.listen(error)) {
        delete room;
        return nullptr;
    }
    d.state = State::Open;
    d.pingTimer = new QTimer(room);
    d.pingTimer->setInterval(std::max(100, std::min({options.limits.pingIntervalMs, options.limits.helloTimeoutMs,
                                                     options.limits.reconnectGraceMs})));
    QObject::connect(d.pingTimer, &QTimer::timeout, room, [&d] { d.hostTick(); });
    d.pingTimer->start();
    qCInfo(lcPeers).noquote() << "Hosting room" << d.name << "on" << options.address.toString() << "port"
                              << d.tcp->serverPort() << "admission" << admissionName(options.admission)
                              << (options.guests ? "with guests" : "without guests");
    return room;
}

Room* Room::join(const QString& address, const JoinOptions& options, QObject* parent) {
    auto* room = new Room(parent);
    Impl& d = *room->d;
    d.joinOptions = options;
    d.hostAddress = address;
    d.profile = completed(options.profile, QStringLiteral("Peer"));
    d.socket = new QWebSocket(options.origin, QWebSocketProtocol::VersionLatest, room);
    d.queue = std::make_unique<SendQueue>(d.socket);
    d.queue->setOpen(false);
    d.queue->setSealer([&d](const QByteArray& plain) { return d.tx.seal(plain); });
    QWebSocket* socket = d.socket;
    QObject::connect(socket, &QWebSocket::connected, room, [&d] {
        d.heard.start();
        d.kx = kxKeyPair();
        d.challenged = false;
        d.socket->sendBinaryMessage(encodeFrame(control(
            QStringLiteral("hello"), {{QStringLiteral("protocol"), kProtocolVersion}, {QStringLiteral("eph"), b64(d.kx.publicKey)}})));
    });
    QObject::connect(socket, &QWebSocket::binaryMessageReceived, room,
                     [&d](const QByteArray& data) { d.onHostData(data, false); });
    QObject::connect(socket, &QWebSocket::textMessageReceived, room,
                     [&d](const QString& text) { d.onHostData(text.toUtf8(), true); });
    QObject::connect(socket, &QWebSocket::bytesWritten, room, [&d] { d.queue->pump(); });
    QObject::connect(socket, &QWebSocket::pong, room, [&d] { d.heard.restart(); });
    QObject::connect(socket, &QWebSocket::disconnected, room, [&d] { d.onDisconnected(); });
    QObject::connect(socket, &QWebSocket::errorOccurred, room, [&d](QAbstractSocket::SocketError) {
        // Before the first welcome, nothing else will end it: a refused connection
        // emits no disconnected().
        if (d.state == State::Connecting && !d.everOpen) {
            if (d.endReason.isEmpty()) {
                d.endReason = QStringLiteral("Couldn't reach the room: %1").arg(d.socket->errorString());
            }
            d.finish(d.endReason);
        } else if (d.state == State::Reconnecting) {
            d.onDisconnected();  // A refused retry emits no disconnected() either
        }
    });
    d.pingTimer = new QTimer(room);
    d.pingTimer->setInterval(std::max(100, options.pingIntervalMs));
    QObject::connect(d.pingTimer, &QTimer::timeout, room, [&d] { d.joinerTick(); });
    d.pingTimer->start();
    // From the event loop: a connection refused at once would otherwise close the
    // room before the caller could connect to closed().
    QTimer::singleShot(0, room, [&d] { d.connectToHost(); });
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
    for (const Member* member : d->welcomedMembers(true)) {
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

QString Room::address() const {
    if (d->isHost) {
        return QStringLiteral("127.0.0.1:%1").arg(port());
    }
    return d->hostAddress;
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

StateMap* Room::state(const QString& name) {
    if (StateMap* existing = d->maps.value(name)) {
        return existing;
    }
    if (!isValidChannelName(name)) {
        return nullptr;
    }
    auto* map = new StateMap(this, name);
    d->maps.insert(name, map);
    return map;
}

PeerState* Room::peerState(const QString& name) {
    if (PeerState* existing = d->peerStates.value(name)) {
        return existing;
    }
    if (!isValidChannelName(name)) {
        return nullptr;
    }
    auto* state = new PeerState(this, name);
    d->peerStates.insert(name, state);
    return state;
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
            if (member->phase == Member::Phase::Welcomed && !member->away) {
                d->dropMember(member, why, Impl::Drop::Close, QStringLiteral("closed"));
            } else {
                d->dropMember(member, why, Impl::Drop::Abort, {});
            }
        }
    } else if (d->socket) {
        d->ended = true;
        if (d->state == State::Open) {
            d->queue->push(encodeFrame(control(QStringLiteral("leave"), {{QStringLiteral("reason"), why}})), {}, true);
        }
        QObject::disconnect(d->socket, nullptr, this, nullptr);
        d->socket->close(QWebSocketProtocol::CloseCodeNormal, why);
    }
    d->finish(why);
}

quint16 Room::port() const {
    return d->tcp ? d->tcp->serverPort() : 0;
}

Admission Room::admission() const {
    return d->options.admission;
}

bool Room::hasPassword() const {
    return !d->options.password.isEmpty();
}

QString Room::secret() const {
    return d->secret;
}

QString Room::guestSecret() const {
    return d->guestSecret;
}

namespace {

QString linkHost(const QHostAddress& listening, const QString& address) {
    if (!address.isEmpty()) {
        return address;
    }
    if (listening.isLoopback()) {
        return QStringLiteral("127.0.0.1");
    }
    if (listening == QHostAddress::Any || listening == QHostAddress::AnyIPv4 || listening == QHostAddress::AnyIPv6) {
        return firstNetworkAddress();
    }
    return listening.toString();
}

}  // namespace

QString Room::link(const QString& address) const {
    if (!d->isHost) {
        return {};
    }
    return makeJoinLink({linkHost(d->options.address, address) + u':' + QString::number(port()), d->me.fingerprint,
                         d->secret});
}

QUrl Room::guestLink(const QString& address) const {
    if (!d->isHost || d->guestSecret.isEmpty()) {
        return {};
    }
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(linkHost(d->options.address, address));
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
        if (member->away) {
            d->leave(member, why);
        } else {
            d->dropMember(member, why, Impl::Drop::Close, QStringLiteral("removed"));
        }
    }
}

void Room::trust(const QString& fingerprint) {
    d->admitted.insert(fingerprint);
}

qint64 Room::queuedBytes(const PeerId& id) const {
    const Member* member = d->welcomedMember(id);
    return member && member->queue ? member->queue->queuedBytes() : 0;
}

// ---------------------------------------------------------------------------------
// The host: connections and the handshake

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
    Member* raw = member.get();
    member->socket = socket;
    member->queue = std::make_unique<SendQueue>(socket);
    member->queue->setSealer([raw](const QByteArray& plain) { return raw->tx.seal(plain); });
    member->browser = !socket->origin().isEmpty();
    member->tokens = options.limits.messageBurst;
    member->refill.start();
    member->heard.start();
    // Qt's own limit only guards memory, well above ours: when it trips, Qt reads what
    // is left of the frame as more messages. The limits of PEERS.md §7.4 are checked
    // on whole messages in onMemberData().
    socket->setMaxAllowedIncomingFrameSize(quint64(qtLimit(options.limits)));
    socket->setMaxAllowedIncomingMessageSize(quint64(qtLimit(options.limits)));
    bySocket.insert(socket, raw);
    members.push_back(std::move(member));

    QObject::connect(socket, &QWebSocket::binaryMessageReceived, q,
                     [this, socket](const QByteArray& data) { onMemberData(socket, data, false); });
    QObject::connect(socket, &QWebSocket::textMessageReceived, q,
                     [this, socket](const QString& text) { onMemberData(socket, text.toUtf8(), true); });
    QObject::connect(socket, &QWebSocket::bytesWritten, q, [this, socket] {
        if (Member* member = bySocket.value(socket); member && member->queue) {
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

void Room::Impl::onMemberData(QWebSocket* socket, const QByteArray& data, bool text) {
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
        // Told, so that it doesn't come straight back.
        dropMember(member, QStringLiteral("Sent too many messages"), Drop::Close, QStringLiteral("removed"));
        return;
    }
    member->tokens -= 1;

    // A sealed message is a frame and 17 bytes more.
    const qint64 limit = member->phase == Member::Phase::Welcomed ? options.limits.maxFrameBytes : kHandshakeFrameBytes;
    if (data.size() > limit + 17) {
        dropMember(member, QStringLiteral("Sent a message over the size limit"), Drop::Close, QStringLiteral("removed"));
        return;
    }

    QString error;
    std::optional<Frame> frame;
    if (member->encrypted) {
        // After the handshake, only what the connection's key sealed.
        std::optional<QByteArray> plain = text ? std::nullopt : member->rx.open(data);
        if (!plain) {
            dropMember(member,
                       QStringLiteral("Sent something that isn't sealed for this connection (%1 bytes)").arg(data.size()),
                       Drop::Abort, {});
            return;
        }
        frame = decodeFrame(*plain, &error);
    } else {
        frame = text ? decodeTextFrame(QString::fromUtf8(data), &error) : decodeFrame(data, &error);
    }
    if (!frame) {
        dropMember(member, QStringLiteral("Sent something that isn't a frame: %1").arg(error), Drop::Close,
                   QStringLiteral("removed"));
        return;
    }
    const Message& message = frame->message;
    const bool control = message.channel == QLatin1String(kRoomChannel);
    switch (member->phase) {
    case Member::Phase::Hello:
        if (control && message.type == u"hello") {
            onHello(member, message);
        } else {
            deny(member, QStringLiteral("The first message has to be hello"));
        }
        return;
    case Member::Phase::Auth:
        if (control && message.type == u"auth") {
            onAuth(member, message);
        } else {
            deny(member, QStringLiteral("Expected auth"));
        }
        return;
    case Member::Phase::Asking:
        return;  // Waiting for the host's answer
    case Member::Phase::Welcomed:
        break;
    }
    if (control) {
        onMemberControl(member, message);
        return;
    }
    route(message, frame->latest, member);
}

void Room::Impl::onMemberDisconnected(QWebSocket* socket) {
    Member* member = bySocket.value(socket);
    if (!member || member->gone) {
        return;
    }
    if (member->phase != Member::Phase::Welcomed) {
        dropMember(member, QStringLiteral("Went away while joining"), Drop::Abort, {});
        return;
    }
    if (!member->leaveReason.isEmpty()) {
        dropMember(member, member->leaveReason, Drop::Abort, {});
        return;
    }
    if (socket->closeCode() == QWebSocketProtocol::CloseCodeTooMuchData) {
        dropMember(member, QStringLiteral("Sent a message over the size limit"), Drop::Abort, {});
        return;
    }
    // A peer may come back (PEERS.md §7.4); a guest can't resume.
    if (member->peer.kind == Peer::Kind::Peer && options.limits.reconnectGraceMs > 0) {
        goAway(member);
    } else {
        dropMember(member, QStringLiteral("The connection was lost"), Drop::Abort, {});
    }
}

void Room::Impl::onHello(Member* member, const Message& message) {
    const int protocol = message.body.value(QLatin1String("protocol")).toInt(0);
    if (protocol != kProtocolVersion) {
        deny(member, QStringLiteral("This room speaks protocol %1, and this member protocol %2")
                         .arg(kProtocolVersion)
                         .arg(protocol));
        return;
    }
    const QByteArray eph = unb64(message.body.value(QLatin1String("eph")));
    if (eph.isEmpty()) {
        // A page on a plain link (PEERS.md §6.1): unencrypted, and only ever a guest.
        if (!options.guests || !sameSecret(message.body.value(QLatin1String("secret")).toString(), guestSecret)) {
            deny(member, QStringLiteral("Wrong secret: the link isn't this room's"));
            return;
        }
        member->auth = message.body;
        admit(member, Peer::Kind::Guest);
        return;
    }
    // The handshake: our half for this connection, signed with the room's identity.
    const KxKeys keys = kxKeyPair();
    QByteArray receive, send;
    if (!kxSessionKeys(true, keys, eph, &receive, &send)) {
        deny(member, QStringLiteral("The handshake's key is no good"));
        return;
    }
    member->memberEph = eph;
    member->hostEph = keys.publicKey;
    const QByteArray proof = kHostProof + keys.publicKey + eph;
    sendTo(member, control(QStringLiteral("challenge"),
                           {{QStringLiteral("protocol"), kProtocolVersion},
                            {QStringLiteral("eph"), b64(keys.publicKey)},
                            {QStringLiteral("key"), b64(profile.identity.publicKey())},
                            {QStringLiteral("sig"), b64(profile.identity.sign(proof))}}));
    member->tx = Sealer(send);
    member->rx = Sealer(receive);
    member->encrypted = true;
    member->phase = Member::Phase::Auth;
}

void Room::Impl::onAuth(Member* member, const Message& message) {
    const QJsonObject& body = message.body;
    const QByteArray key = unb64(body.value(QLatin1String("key")));
    const QByteArray signature = unb64(body.value(QLatin1String("sig")));
    if (!Identity::verify(key, kPeerProof + member->memberEph + member->hostEph, signature)) {
        deny(member, QStringLiteral("The member's key didn't sign the handshake"));
        return;
    }
    member->auth = body;
    const QString fingerprint = fingerprintOf(key);
    QString instance = body.value(QLatin1String("instance")).toString().left(32);
    if (instance.isEmpty()) {
        instance = randomId(6);
    }
    member->peer = {fingerprint + u'-' + instance,
                    cleanName(body.value(QLatin1String("name")).toString(), QStringLiteral("Peer")),
                    Peer::Kind::Peer,
                    fingerprint,
                    instance,
                    cleanColor(body.value(QLatin1String("color")).toString()),
                    body.value(QLatin1String("app")).toString().left(64)};

    // The same peer back after a lost connection (PEERS.md §7.4): its key proves it.
    if (Member* previous = welcomedMember(member->peer.id); previous && previous != member &&
                                                            previous->peer.kind == Peer::Kind::Peer) {
        const qint64 lastSeq = body.value(QLatin1String("resume")).toObject().value(QLatin1String("seq")).toInteger(-1);
        member->peer = previous->peer;
        member->phase = Member::Phase::Welcomed;
        member->roomKey = true;
        // The old connection goes quietly: nobody hears it leave.
        previous->gone = true;
        if (previous->socket) {
            QObject::disconnect(previous->socket, nullptr, q, nullptr);
            bySocket.remove(previous->socket);
            previous->socket->abort();
            previous->socket->deleteLater();
            previous->socket = nullptr;
            rotateRoomKey();  // It had the key until now
        }
        const auto found = std::find_if(members.begin(), members.end(),
                                        [previous](const auto& m) { return m.get() == previous; });
        if (found != members.end()) {
            dead.push_back(std::move(*found));
            members.erase(found);
            QTimer::singleShot(0, q, [this] { dead.clear(); });
        }
        qCInfo(lcPeers).noquote() << member->peer.name << member->peer.id << "is back in room" << name;
        welcome(member, true, lastSeq);
        return;
    }

    const QString given = body.value(QLatin1String("secret")).toString();
    if (!given.isEmpty()) {
        if (!member->browser && sameSecret(given, secret)) {
            if (options.askBeforeLinkJoin && !admitted.contains(fingerprint)) {
                member->phase = Member::Phase::Asking;
                auto* request = new JoinRequest(q, member->peer);
                member->request = request;
                Q_EMIT q->joinRequested(request);
                return;
            }
            admit(member, Peer::Kind::Peer);
        } else if (options.guests && sameSecret(given, guestSecret)) {
            admit(member, Peer::Kind::Guest);
        } else {
            deny(member, QStringLiteral("Wrong secret: the link isn't this room's"));
        }
        return;
    }
    // Found on this machine or the network, without the secret.
    if (member->browser || options.admission == Admission::Invited) {
        deny(member, QStringLiteral("This room takes only those invited with its link"));
        return;
    }
    if (!options.password.isEmpty() &&
        !sameSecret(body.value(QLatin1String("password")).toString(), options.password)) {
        deny(member, QStringLiteral("Wrong password"));
        return;
    }
    if (options.admission == Admission::Open || admitted.contains(fingerprint)) {
        admit(member, Peer::Kind::Peer);
        return;
    }
    member->phase = Member::Phase::Asking;
    auto* request = new JoinRequest(q, member->peer);
    member->request = request;
    qCInfo(lcPeers).noquote() << member->peer.name << "asks to join room" << name;
    Q_EMIT q->joinRequested(request);
    if (member->phase == Member::Phase::Asking && !member->gone) {
        // Unanswered, it's a no.
        QPointer<JoinRequest> guard = request;
        QTimer::singleShot(options.limits.joinRequestTimeoutMs, q, [guard] {
            if (guard) {
                guard->deny(QStringLiteral("The host didn't answer"));
            }
        });
    }
}

void Room::Impl::settle(JoinRequest* request, bool allow, const QString& reason) {
    request->m_settled = true;
    Member* asking = nullptr;
    for (const auto& member : members) {
        if (member->request == request && !member->gone) {
            asking = member.get();
        }
    }
    request->deleteLater();
    if (!asking || asking->phase != Member::Phase::Asking) {
        return;
    }
    if (allow) {
        admit(asking, Peer::Kind::Peer);
    } else {
        deny(asking, reason.isEmpty() ? QStringLiteral("The host said no") : reason);
    }
}

void Room::Impl::admit(Member* member, Peer::Kind kind) {
    const QList<Member*> present = welcomedMembers(true);
    if (present.size() >= options.limits.maxMembers) {
        deny(member, QStringLiteral("The room is full"));
        return;
    }
    if (kind == Peer::Kind::Guest &&
        std::count_if(present.begin(), present.end(),
                      [](const Member* m) { return m->peer.kind == Peer::Kind::Guest; }) >= options.limits.maxGuests) {
        deny(member, QStringLiteral("The room has as many guests as it takes"));
        return;
    }
    if (kind == Peer::Kind::Guest) {
        // A guest is known only while it is connected (PEERS.md §6.1).
        const QString given = member->auth.value(QLatin1String("name")).toString();
        member->peer = Peer{guestId(), cleanName(given, QStringLiteral("Guest")), Peer::Kind::Guest};
    } else {
        admitted.insert(member->peer.fingerprint);
        member->roomKey = member->encrypted;
    }
    member->peer.kind = kind;
    member->phase = Member::Phase::Welcomed;
    qCInfo(lcPeers).noquote() << kindName(kind) << member->peer.name << member->peer.id << "joined room" << name;
    welcome(member, false, -1);
    broadcast(control(QStringLiteral("joined"), peerJson(member->peer)), true, {}, member, {});
    const Peer peer = member->peer;
    Q_EMIT q->peerJoined(peer);
}

void Room::Impl::welcome(Member* member, bool resumed, qint64 lastSeq) {
    const bool guest = member->peer.kind == Peer::Kind::Guest;
    // A peer back in time gets what it missed; one back too late gets it all afresh.
    const bool caughtUp = resumed && lastSeq >= 0 &&
                          (lastSeq == seq || (!history.empty() && history.front().seq <= lastSeq + 1));
    QJsonArray peerList;
    for (const Peer& peer : q->peers()) {
        peerList.append(peerJson(peer));
    }
    QJsonArray channelList;
    for (const Channel* channel : std::as_const(channels)) {
        if (!guest || channel->isOpenToGuests()) {
            channelList.append(QJsonObject{{QStringLiteral("name"), channel->name()},
                                           {QStringLiteral("delivery"), deliveryName(channel->delivery())}});
        }
    }
    QJsonObject body{{QStringLiteral("protocol"), kProtocolVersion},
                     {QStringLiteral("id"), member->peer.id},
                     {QStringLiteral("name"), name},
                     {QStringLiteral("host"), me.id},
                     {QStringLiteral("seq"), seq},
                     {QStringLiteral("peers"), peerList},
                     {QStringLiteral("channels"), channelList},
                     {QStringLiteral("admission"), admissionName(options.admission)}};
    if (resumed) {
        body.insert(QStringLiteral("resumed"), true);
        body.insert(QStringLiteral("caughtUp"), caughtUp);
    }
    if (member->roomKey) {
        body.insert(QStringLiteral("roomKey"),
                    QJsonObject{{QStringLiteral("epoch"), qint64(roomEpoch)}, {QStringLiteral("key"), b64(roomKey)}});
    }
    sendTo(member, control(QStringLiteral("welcome"), body));
    if (caughtUp) {
        for (const HistoryEntry& entry : history) {
            if (entry.seq > lastSeq) {
                enqueue(member, entry.frame, {}, member->encrypted);
            }
        }
        // Per-peer state isn't ordered, so it isn't in the history.
        for (PeerState* state : std::as_const(peerStates)) {
            if (!guest || state->isOpenToGuests()) {
                QJsonObject values;
                for (auto it = state->m_values.cbegin(); it != state->m_values.cend(); ++it) {
                    values.insert(it.key(), it.value());
                }
                sendTo(member, control(QStringLiteral("peers.state"),
                                       {{QStringLiteral("map"), state->name()}, {QStringLiteral("values"), values}}));
            }
        }
    } else {
        sendSnapshots(member);
    }
    dropSlowMembers();
}

void Room::Impl::sendSnapshots(Member* member) {
    const bool guest = member->peer.kind == Peer::Kind::Guest;
    if (!guest) {
        for (StateMap* map : std::as_const(maps)) {
            sendTo(member, control(QStringLiteral("state"),
                                   {{QStringLiteral("map"), map->name()}, {QStringLiteral("items"), map->items()}}));
        }
    }
    for (PeerState* state : std::as_const(peerStates)) {
        if (!guest || state->isOpenToGuests()) {
            QJsonObject values;
            for (auto it = state->m_values.cbegin(); it != state->m_values.cend(); ++it) {
                values.insert(it.key(), it.value());
            }
            sendTo(member, control(QStringLiteral("peers.state"),
                                   {{QStringLiteral("map"), state->name()}, {QStringLiteral("values"), values}}));
        }
    }
    for (Channel* channel : std::as_const(channels)) {
        if (channel->m_snapshot && (!guest || channel->isOpenToGuests())) {
            const Channel::Snapshot snapshot = channel->m_snapshot();
            Message message;
            message.channel = channel->name();
            message.type = QStringLiteral("snapshot");
            message.from = me.id;
            message.to = member->peer.id;
            message.body = snapshot.body;
            message.bytes = snapshot.bytes;
            sendTo(member, message);
        }
    }
}

void Room::Impl::deny(Member* member, const QString& reason) {
    qCInfo(lcPeers).noquote() << "Refused a member:" << reason;
    dropMember(member, reason, Drop::Close, QStringLiteral("denied"));
}

// ---------------------------------------------------------------------------------
// The host: messages

void Room::Impl::onMemberControl(Member* member, const Message& message) {
    const QJsonObject& body = message.body;
    if (message.type == u"leave") {
        member->leaveReason = body.value(QLatin1String("reason")).toString().left(200);
        if (member->leaveReason.isEmpty()) {
            member->leaveReason = QStringLiteral("Left");
        }
        dropMember(member, member->leaveReason, Drop::Close, {});
        return;
    }
    if (member->peer.kind == Peer::Kind::Guest) {
        return;  // Guests have no shared state to write
    }
    const QString mapName = body.value(QLatin1String("map")).toString();
    if (message.type == u"set" || message.type == u"remove") {
        const QString key = body.value(QLatin1String("key")).toString();
        StateMap* map = isValidChannelName(mapName) ? q->state(mapName) : nullptr;
        const auto refuse = [&](const QString& reason) {
            sendTo(member, control(QStringLiteral("refused"), {{QStringLiteral("channel"), QLatin1String(kRoomChannel)},
                                                               {QStringLiteral("type"), message.type},
                                                               {QStringLiteral("map"), mapName},
                                                               {QStringLiteral("key"), key},
                                                               {QStringLiteral("reason"), reason}}));
        };
        if (!map) {
            refuse(QStringLiteral("Not a map's name"));
            return;
        }
        if (!map->maySend(member->peer)) {
            refuse(QStringLiteral("Not allowed to change this map"));
            return;
        }
        const QJsonValue value = message.type == u"set" ? body.value(QLatin1String("value")) : QJsonValue();
        if (message.type == u"set" && value.isUndefined()) {
            refuse(QStringLiteral("No value"));
            return;
        }
        if (message.type == u"set") {
            const qint64 size = jsonSize(value);
            const qint64 before = map->contains(key) ? jsonSize(map->value(key)) + key.size() : 0;
            if (size > StateMap::kMaxValueBytes) {
                refuse(QStringLiteral("The value is over 256 KB"));
                return;
            }
            if (mapBytes.value(mapName) - before + size + key.size() > StateMap::kMaxMapBytes) {
                refuse(QStringLiteral("The map would be over 16 MB"));
                return;
            }
        }
        setState(map, key, value, member->peer, member);
        return;
    }
    if (message.type == u"peer.set") {
        PeerState* state = isValidChannelName(mapName) ? q->peerState(mapName) : nullptr;
        if (state) {
            setPeerState(state, member->peer, body.value(QLatin1String("value")), member);
        }
    }
}

void Room::Impl::setState(StateMap* map, const QString& key, const QJsonValue& value, const Peer& by,
                          Member* sender) {
    Q_UNUSED(sender);
    const qint64 before = map->contains(key) ? jsonSize(map->value(key)) + key.size() : 0;
    const qint64 after = value.isUndefined() || value.isNull() ? 0 : jsonSize(value) + key.size();
    mapBytes[map->name()] += after - before;
    QJsonObject body{{QStringLiteral("map"), map->name()}, {QStringLiteral("key"), key}, {QStringLiteral("by"), by.id}};
    if (!value.isUndefined()) {
        body.insert(QStringLiteral("value"), value);
    }
    Message message = control(QStringLiteral("changed"), body);
    message.from = me.id;
    // Guests don't see maps.
    broadcast(message, true, {}, nullptr, [](const Member* m) { return m->peer.kind != Peer::Kind::Guest; });
    map->apply(key, value, by, false);
}

void Room::Impl::setPeerState(PeerState* state, const Peer& peer, const QJsonValue& value, const Member* sender) {
    state->apply(peer, value);
    if (welcomedMembers().size() > options.limits.maxPeerStateMembers) {
        return;  // Members times members: too much for a large room (PEERS.md §7.5)
    }
    QJsonObject body{{QStringLiteral("map"), state->name()}, {QStringLiteral("peer"), peer.id}};
    if (!value.isUndefined() && !value.isNull()) {
        body.insert(QStringLiteral("value"), value);
    }
    Message message = control(QStringLiteral("peer.changed"), body);
    message.from = me.id;
    const bool open = state->isOpenToGuests();
    broadcast(message, false, QStringLiteral("peer.changed\n") + state->name() + u'\n' + peer.id, sender,
              [open](const Member* m) { return m->peer.kind != Peer::Kind::Guest || open; });
}

void Room::Impl::forgetPeerState(const PeerId& id) {
    for (PeerState* state : std::as_const(peerStates)) {
        if (state->m_values.contains(id)) {
            Peer peer = q->peer(id);
            if (!peer.isValid()) {
                peer.id = id;
            }
            state->apply(peer, QJsonValue());
        }
    }
}

void Room::Impl::route(Message message, bool latest, Member* sender) {
    const Peer from = sender ? sender->peer : me;
    message.from = from.id;
    message.seq = 0;
    Channel* channel = channels.value(message.channel);
    const auto refuse = [&](const QString& reason) {
        if (sender) {
            sendTo(sender, control(QStringLiteral("refused"), {{QStringLiteral("channel"), message.channel},
                                                               {QStringLiteral("type"), message.type},
                                                               {QStringLiteral("reason"), reason}}));
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
    const auto reaches = [channel](const Member* member) {
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
        sendTo(target, message, delivery == Delivery::LatestOnly ? latestKey(message) : QString());
        dropSlowMembers();
        return;
    }

    if (delivery == Delivery::LatestOnly) {
        broadcast(message, false, latestKey(message), sender, reaches);
        if (sender && channel) {
            Q_EMIT channel->received(message);
        }
        return;
    }
    // Ordered: numbered here, which is what makes it one order for everyone, the
    // sender included.
    message.seq = seq + 1;
    broadcast(message, true, {}, nullptr, reaches);
    if (channel) {
        Q_EMIT channel->received(message);
    }
}

void Room::Impl::broadcast(Message message, bool ordered, const QString& latestKey, const Member* except,
                           const std::function<bool(const Member*)>& reaches) {
    if (ordered) {
        message.seq = ++seq;
    }
    const QByteArray plain = encodeFrame(message);
    if (ordered) {
        history.push_back({message.seq, plain});
        historyBytes += plain.size();
        while (!history.empty() && (int(history.size()) > options.limits.historyMessages ||
                                    historyBytes > options.limits.historyBytes)) {
            historyBytes -= history.front().frame.size();
            history.pop_front();
        }
    }
    QByteArray envelope;  // Under the room key, once for everyone who has it
    for (Member* member : welcomedMembers()) {
        if (member == except || (reaches && !reaches(member))) {
            continue;
        }
        if (member->roomKey) {
            if (envelope.isEmpty()) {
                envelope = sealForRoom(roomKey, roomEpoch, plain, profile.identity);
            }
            enqueue(member, envelope, latestKey, false);
        } else {
            enqueue(member, plain, latestKey, member->encrypted);
        }
    }
    dropSlowMembers();
}

void Room::Impl::sendTo(Member* member, const Message& message, const QString& latestKey) {
    enqueue(member, encodeFrame(message), latestKey, member->encrypted);
}

void Room::Impl::enqueue(Member* member, const QByteArray& data, const QString& latestKey, bool seal) {
    if (member->gone || member->away || !member->queue) {
        return;
    }
    member->queue->push(data, latestKey, seal);
    if (member->queue->queuedBytes() > options.limits.maxQueuedBytes &&
        std::find(slow.begin(), slow.end(), member) == slow.end()) {
        slow.push_back(member);
    }
}

void Room::Impl::dropSlowMembers() {
    while (!slow.empty()) {
        Member* member = slow.back();
        slow.pop_back();
        if (!member->gone && !member->away) {
            qCInfo(lcPeers).noquote() << member->peer.name << member->peer.id << "fell behind by"
                                      << member->queue->queuedBytes() << "bytes";
            dropMember(member, QStringLiteral("Too slow: fell too far behind"), Drop::Abort, {});
        }
    }
}

// ---------------------------------------------------------------------------------
// The host: members leaving

void Room::Impl::dropMember(Member* member, const QString& reason, Drop how, const QString& tellType) {
    if (member->gone) {
        return;
    }
    QWebSocket* socket = member->socket;
    if (socket) {
        QObject::disconnect(socket, nullptr, q, nullptr);
        bySocket.remove(socket);
        if (member->queue) {
            member->queue->clear();
        }
        if (!tellType.isEmpty() && socket->isValid()) {
            Message tell = control(tellType, {{QStringLiteral("reason"), reason},
                                              {QStringLiteral("protocol"), kProtocolVersion}});
            const QByteArray plain = encodeFrame(tell);
            socket->sendBinaryMessage(member->encrypted ? member->tx.seal(plain) : plain);
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
        member->socket = nullptr;
    }
    if (member->request) {
        JoinRequest* request = member->request;
        request->m_settled = true;
        Q_EMIT request->cancelled();
        request->deleteLater();
    }
    if (member->phase == Member::Phase::Welcomed) {
        leave(member, reason);
    } else {
        member->gone = true;
        const auto found =
            std::find_if(members.begin(), members.end(), [member](const auto& m) { return m.get() == member; });
        if (found != members.end()) {
            dead.push_back(std::move(*found));
            members.erase(found);
            QTimer::singleShot(0, q, [this] { dead.clear(); });
        }
    }
}

void Room::Impl::goAway(Member* member) {
    qCInfo(lcPeers).noquote() << member->peer.name << member->peer.id << "lost its connection; waiting for it";
    if (member->socket) {
        QObject::disconnect(member->socket, nullptr, q, nullptr);
        bySocket.remove(member->socket);
        member->socket->deleteLater();
        member->socket = nullptr;
    }
    member->queue.reset();
    member->away = true;
    member->awaySince.start();
    if (member->roomKey) {
        rotateRoomKey();
    }
}

// Out of the room for good: everyone hears it, and its key no longer opens anything new.
void Room::Impl::leave(Member* member, const QString& reason) {
    if (member->gone) {
        return;
    }
    member->gone = true;
    const bool hadKey = member->roomKey && !member->away;
    const Peer peer = member->peer;
    const auto found = std::find_if(members.begin(), members.end(), [member](const auto& m) { return m.get() == member; });
    if (found != members.end()) {
        dead.push_back(std::move(*found));
        members.erase(found);
        QTimer::singleShot(0, q, [this] { dead.clear(); });
    }
    if (closing) {
        return;
    }
    if (hadKey) {
        rotateRoomKey();
    }
    broadcast(control(QStringLiteral("left"), {{QStringLiteral("id"), peer.id}, {QStringLiteral("reason"), reason}}),
              true, {}, nullptr, {});
    forgetPeerState(peer.id);
    Q_EMIT q->peerLeft(peer, reason);
}

// A new room key for those still connected (PEERS.md §6): whoever left can't read
// what follows. Each gets it on its own connection, before anything under it.
void Room::Impl::rotateRoomKey() {
    if (closing) {
        return;
    }
    ++roomEpoch;
    roomKey = randomBytes(32);
    const Message message = control(QStringLiteral("key"), {{QStringLiteral("epoch"), qint64(roomEpoch)},
                                                            {QStringLiteral("key"), b64(roomKey)}});
    for (Member* member : welcomedMembers()) {
        if (member->roomKey) {
            sendTo(member, message);
        }
    }
}

Member* Room::Impl::welcomedMember(const PeerId& id) const {
    for (const auto& member : members) {
        if (member->phase == Member::Phase::Welcomed && !member->gone && member->peer.id == id) {
            return member.get();
        }
    }
    return nullptr;
}

QList<Member*> Room::Impl::welcomedMembers(bool withAway) const {
    QList<Member*> result;
    for (const auto& member : members) {
        if (member->phase == Member::Phase::Welcomed && !member->gone && (withAway || !member->away)) {
            result.append(member.get());
        }
    }
    return result;
}

QString Room::Impl::guestId() const {
    for (;;) {
        const QString id = QStringLiteral("guest-") + randomId(8);
        if (!welcomedMember(id)) {
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
        if (member->away) {
            if (member->awaySince.elapsed() > limits.reconnectGraceMs) {
                leave(member, QStringLiteral("The connection was lost"));
            }
        } else if (member->phase == Member::Phase::Hello || member->phase == Member::Phase::Auth) {
            if (member->heard.elapsed() > limits.helloTimeoutMs) {
                deny(member, QStringLiteral("Didn't finish the handshake"));
            }
        } else if (member->phase == Member::Phase::Asking) {
            continue;
        } else if (member->heard.elapsed() > limits.silenceTimeoutMs) {
            dropMember(member, QStringLiteral("Silent for too long"), Drop::Abort, {});
        } else if (member->socket && member->socket->isValid()) {
            member->socket->ping();
        }
    }
}

// ---------------------------------------------------------------------------------
// A member that joined

void Room::Impl::connectToHost() {
    if (state == State::Closed) {
        return;
    }
    challenged = false;
    tx = Sealer();
    rx = Sealer();
    socket->open(QUrl(QStringLiteral("ws://") + hostAddress + QStringLiteral("/ws")));
}

void Room::Impl::onHostData(const QByteArray& data, bool text) {
    heard.restart();
    std::optional<QByteArray> plain;
    if (!challenged) {
        plain = data;
    } else if (!text && !data.isEmpty() && data[0] == kSealedMarker) {
        plain = rx.open(data);
    } else if (!text && data.size() > 5 && data[0] == kRoomMarker) {
        // A latest-only message may overtake the new key it was sealed under, by taking
        // the place of an older one that waited ahead of it: then it is skipped.
        if (!roomKeys.contains(qFromBigEndian<quint32>(data.constData() + 1))) {
            return;
        }
        plain = openFromRoom(roomKeys, hostKey, data);
    }
    if (!plain) {
        qCWarning(lcPeers) << "The host sent something this member can't open";
        endReason = QStringLiteral("The host sent something that couldn't be opened");
        socket->abort();
        return;
    }
    QString error;
    std::optional<Frame> frame = text ? decodeTextFrame(QString::fromUtf8(*plain), &error) : decodeFrame(*plain, &error);
    if (!frame) {
        qCWarning(lcPeers).noquote() << "The host sent something that isn't a frame:" << error;
        return;
    }
    if (!challenged) {
        const Message& message = frame->message;
        if (message.channel == QLatin1String(kRoomChannel) && message.type == u"challenge") {
            onChallenge(message);
        } else if (message.channel == QLatin1String(kRoomChannel) && message.type == u"denied") {
            onHostFrame(message);
        }
        return;
    }
    onHostFrame(frame->message);
}

void Room::Impl::onChallenge(const Message& message) {
    const QJsonObject& body = message.body;
    const QByteArray eph = unb64(body.value(QLatin1String("eph")));
    const QByteArray key = unb64(body.value(QLatin1String("key")));
    const QByteArray signature = unb64(body.value(QLatin1String("sig")));
    const auto fail = [this](const QString& reason) {
        ended = true;
        endReason = reason;
        socket->abort();
        finish(reason);
    };
    if (body.value(QLatin1String("protocol")).toInt() != kProtocolVersion) {
        fail(QStringLiteral("The host speaks protocol %1, and this member protocol %2")
                 .arg(body.value(QLatin1String("protocol")).toInt())
                 .arg(kProtocolVersion));
        return;
    }
    if (!Identity::verify(key, kHostProof + eph + kx.publicKey, signature)) {
        fail(QStringLiteral("The host's key didn't sign the handshake"));
        return;
    }
    const QString fingerprint = fingerprintOf(key);
    if (!joinOptions.expectedFingerprint.isEmpty() && fingerprint != joinOptions.expectedFingerprint) {
        fail(QStringLiteral("Wrong key: the host isn't the one the link names"));
        return;
    }
    if (!hostKey.isEmpty() && key != hostKey) {
        fail(QStringLiteral("Wrong key: another program answers at the host's address"));
        return;
    }
    QByteArray receive, send;
    if (!kxSessionKeys(false, kx, eph, &receive, &send)) {
        fail(QStringLiteral("The host's handshake key is no good"));
        return;
    }
    hostKey = key;
    rx = Sealer(receive);
    tx = Sealer(send);
    challenged = true;
    QJsonObject auth{{QStringLiteral("key"), b64(profile.identity.publicKey())},
                     {QStringLiteral("sig"), b64(profile.identity.sign(kPeerProof + kx.publicKey + eph))},
                     {QStringLiteral("instance"), profile.instance},
                     {QStringLiteral("name"), profile.name},
                     {QStringLiteral("color"), profile.color},
                     {QStringLiteral("app"), profile.app}};
    if (!joinOptions.secret.isEmpty()) {
        auth.insert(QStringLiteral("secret"), joinOptions.secret);
    }
    if (!joinOptions.password.isEmpty()) {
        auth.insert(QStringLiteral("password"), joinOptions.password);
    }
    if (everOpen) {
        auth.insert(QStringLiteral("resume"), QJsonObject{{QStringLiteral("id"), me.id}, {QStringLiteral("seq"), lastSeq}});
    }
    socket->sendBinaryMessage(tx.seal(encodeFrame(control(QStringLiteral("auth"), auth))));
}

void Room::Impl::onWelcome(const QJsonObject& body) {
    name = body.value(QLatin1String("name")).toString();
    const PeerId id = body.value(QLatin1String("id")).toString();
    const PeerId hostId = body.value(QLatin1String("host")).toString();
    const QJsonObject keyObject = body.value(QLatin1String("roomKey")).toObject();
    if (!keyObject.isEmpty()) {
        roomKeys.insert(quint32(keyObject.value(QLatin1String("epoch")).toInteger()),
                        unb64(keyObject.value(QLatin1String("key"))));
    }
    const bool resumed = body.value(QLatin1String("resumed")).toBool();
    const bool caughtUp = body.value(QLatin1String("caughtUp")).toBool();
    QList<Peer> peers;
    for (const QJsonValue& value : body.value(QLatin1String("peers")).toArray()) {
        const Peer peer = peerFromJson(value.toObject());
        peers.append(peer);
        if (peer.id == id) {
            me = peer;
        }
        if (peer.id == hostId) {
            host = peer;
        }
    }
    const QList<Peer> before = joinedPeers;
    joinedPeers = peers;
    if (!caughtUp) {
        // What follows (maps, snapshots, the ordered messages) starts at the host's seq.
        lastSeq = body.value(QLatin1String("seq")).toInteger();
        for (StateMap* map : std::as_const(maps)) {
            map->replace({});
        }
    }
    const bool wasOpen = everOpen;
    everOpen = true;
    retries = 0;
    queue->setOpen(true);
    setRoomState(State::Open);
    if (!wasOpen) {
        Q_EMIT q->opened();
        return;
    }
    // Back: who came and went meanwhile.
    for (const Peer& peer : before) {
        if (std::none_of(peers.begin(), peers.end(), [&](const Peer& p) { return p.id == peer.id; })) {
            Q_EMIT q->peerLeft(peer, QStringLiteral("Left while this member was away"));
        }
    }
    for (const Peer& peer : peers) {
        if (std::none_of(before.begin(), before.end(), [&](const Peer& p) { return p.id == peer.id; })) {
            Q_EMIT q->peerJoined(peer);
        }
    }
    Q_UNUSED(resumed);
    Q_EMIT q->resumed(caughtUp);
}

void Room::Impl::onHostFrame(const Message& message) {
    if (message.seq > 0) {
        if (message.seq <= lastSeq) {
            return;  // Already had it, before a reconnect
        }
        lastSeq = message.seq;
    }
    if (message.channel == QLatin1String(kRoomChannel)) {
        const QJsonObject& body = message.body;
        const QString reason = body.value(QLatin1String("reason")).toString();
        const QString type = message.type;
        if (type == u"welcome") {
            onWelcome(body);
        } else if (type == u"joined") {
            const Peer peer = peerFromJson(body);
            if (peer.isValid() && peer.id != me.id &&
                std::none_of(joinedPeers.begin(), joinedPeers.end(), [&](const Peer& p) { return p.id == peer.id; })) {
                joinedPeers.append(peer);
                Q_EMIT q->peerJoined(peer);
            }
        } else if (type == u"left") {
            removePeerLocally(body.value(QLatin1String("id")).toString(), reason);
        } else if (type == u"denied" || type == u"removed" || type == u"closed") {
            ended = true;
            endReason = reason.isEmpty() ? QStringLiteral("The host ended the connection") : reason;
            finish(endReason);
        } else if (type == u"refused") {
            const QString map = body.value(QLatin1String("map")).toString();
            if (!map.isEmpty()) {
                if (StateMap* state = maps.value(map)) {
                    state->refuse(body.value(QLatin1String("key")).toString(), reason);
                }
            } else if (Channel* channel = channels.value(body.value(QLatin1String("channel")).toString())) {
                Q_EMIT channel->refused(body.value(QLatin1String("type")).toString(), reason);
            }
        } else if (type == u"key") {
            roomKeys.insert(quint32(body.value(QLatin1String("epoch")).toInteger()), unb64(body.value(QLatin1String("key"))));
            // The one before stays for what was sealed before the change; older ones go.
            const quint32 newest = quint32(body.value(QLatin1String("epoch")).toInteger());
            for (auto it = roomKeys.begin(); it != roomKeys.end();) {
                it = it.key() + 1 < newest ? roomKeys.erase(it) : std::next(it);
            }
        } else if (type == u"state") {
            if (StateMap* map = q->state(body.value(QLatin1String("map")).toString())) {
                map->replace(body.value(QLatin1String("items")).toObject());
            }
        } else if (type == u"changed") {
            if (StateMap* map = q->state(body.value(QLatin1String("map")).toString())) {
                const PeerId by = body.value(QLatin1String("by")).toString();
                Peer peer = q->peer(by);
                if (!peer.isValid()) {
                    peer.id = by;
                }
                map->apply(body.value(QLatin1String("key")).toString(), body.value(QLatin1String("value")), peer,
                           by == me.id);
            }
        } else if (type == u"peers.state") {
            if (PeerState* state = q->peerState(body.value(QLatin1String("map")).toString())) {
                const QJsonObject values = body.value(QLatin1String("values")).toObject();
                for (const PeerId& id : state->m_values.keys()) {
                    if (!values.contains(id) && id != me.id) {
                        Peer peer = q->peer(id);
                        peer.id = id;
                        state->apply(peer, QJsonValue());
                    }
                }
                for (auto it = values.begin(); it != values.end(); ++it) {
                    if (it.key() != me.id) {
                        Peer peer = q->peer(it.key());
                        peer.id = it.key();
                        state->apply(peer, it.value());
                    }
                }
            }
        } else if (type == u"peer.changed") {
            if (PeerState* state = q->peerState(body.value(QLatin1String("map")).toString())) {
                const PeerId id = body.value(QLatin1String("peer")).toString();
                Peer peer = q->peer(id);
                peer.id = id;
                state->apply(peer, body.value(QLatin1String("value")));
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

void Room::Impl::removePeerLocally(const PeerId& id, const QString& reason) {
    for (qsizetype i = 0; i < joinedPeers.size(); ++i) {
        if (joinedPeers[i].id == id) {
            const Peer peer = joinedPeers.takeAt(i);
            for (PeerState* state : std::as_const(peerStates)) {
                if (state->m_values.contains(id)) {
                    state->apply(peer, QJsonValue());
                }
            }
            Q_EMIT q->peerLeft(peer, reason);
            return;
        }
    }
}

void Room::Impl::onDisconnected() {
    if (state == State::Closed) {
        return;
    }
    if (ended || !everOpen || joinOptions.reconnectForMs <= 0) {
        finish(endReason.isEmpty() ? QStringLiteral("The connection to the host was lost") : endReason);
        return;
    }
    // Lost the host: try again for a while (PEERS.md §7.4).
    if (state != State::Reconnecting) {
        lostSince.start();
        queue->setOpen(false);
        setRoomState(State::Reconnecting);
        qCInfo(lcPeers).noquote() << "Lost the host of room" << name << "; reconnecting";
    }
    if (lostSince.elapsed() > joinOptions.reconnectForMs) {
        finish(QStringLiteral("Lost the connection to the host"));
        return;
    }
    if (retryScheduled) {
        return;
    }
    retryScheduled = true;
    const int delay = std::min(8000, 250 << std::min(retries, 5));
    ++retries;
    QTimer::singleShot(delay, q, [this] {
        retryScheduled = false;
        connectToHost();
    });
}

void Room::Impl::joinerTick() {
    if (state == State::Closed || !socket || !socket->isValid()) {
        return;
    }
    if (heard.isValid() && heard.elapsed() > joinOptions.silenceTimeoutMs) {
        qCInfo(lcPeers) << "The host went silent";
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
    if (message.to.isEmpty() && channel->delivery() == Delivery::Direct && host.isValid()) {
        message.to = host.id;  // Before the welcome, the host's own Direct channel decides
    }
    queue->push(encodeFrame(message, latest), latest ? latestKey(message) : QString(), true);
}

void Room::Impl::sendState(StateMap* map, const QString& key, const QJsonValue& value) {
    if (state == State::Closed) {
        return;
    }
    if (isHost) {
        setState(map, key, value, me, nullptr);
        return;
    }
    QJsonObject body{{QStringLiteral("map"), map->name()}, {QStringLiteral("key"), key}};
    if (!value.isUndefined()) {
        body.insert(QStringLiteral("value"), value);
    }
    queue->push(encodeFrame(control(value.isUndefined() ? QStringLiteral("remove") : QStringLiteral("set"), body)), {},
                true);
}

void Room::Impl::sendPeerState(PeerState* peerState, const QJsonValue& value) {
    if (state == State::Closed) {
        return;
    }
    if (isHost) {
        setPeerState(peerState, me, value, nullptr);
        return;
    }
    peerState->apply(me, value);
    QJsonObject body{{QStringLiteral("map"), peerState->name()}};
    if (!value.isUndefined() && !value.isNull()) {
        body.insert(QStringLiteral("value"), value);
    }
    queue->push(encodeFrame(control(QStringLiteral("peer.set"), body)),
                QStringLiteral("peer.set\n") + peerState->name(), true);
}

void Room::Impl::setRoomState(State next) {
    if (state == next) {
        return;
    }
    state = next;
    Q_EMIT q->stateChanged(next);
}

void Room::Impl::finish(const QString& reason) {
    if (state == State::Closed) {
        return;
    }
    if (pingTimer) {
        pingTimer->stop();
    }
    if (isHost && tcp) {
        tcp->close();
    }
    if (queue) {
        queue->clear();
    }
    setRoomState(State::Closed);
    Q_EMIT q->closed(reason);
}

void Room::Impl::shutdown() {
    // Quietly: the room is being destroyed, so no signals.
    if (pingTimer) {
        pingTimer->stop();
    }
    for (const auto& member : members) {
        if (member->socket) {
            QObject::disconnect(member->socket, nullptr, q, nullptr);
            member->socket->abort();
        }
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
