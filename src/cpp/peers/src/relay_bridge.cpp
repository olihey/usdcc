#include "relay_bridge.h"

#include "crypto.h"
#include "log.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <QWebSocket>
#include <QtEndian>

#include <algorithm>

namespace usdcc::peers {

namespace {

constexpr char kData = 1;
constexpr char kDataMany = 2;
constexpr char kOpened = 3;
constexpr char kClosed = 4;
constexpr int kMaxCopies = 65535;  // Members in one data-to-several frame (a u16)

QByteArray u32(quint32 value) {
    QByteArray bytes(4, Qt::Uninitialized);
    qToBigEndian(value, bytes.data());
    return bytes;
}

QByteArray u16(quint16 value) {
    QByteArray bytes(2, Qt::Uninitialized);
    qToBigEndian(value, bytes.data());
    return bytes;
}

}  // namespace

RelayBridge::RelayBridge(const QUrl& relay, const QString& token, quint16 port, QObject* parent)
    : QObject(parent), m_relay(relay), m_token(token), m_port(port) {
    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    connect(m_retry, &QTimer::timeout, this, &RelayBridge::connectToRelay);
}

RelayBridge::~RelayBridge() {
    m_stopped = true;
    for (Member& member : m_members) {
        if (member.socket) {
            member.socket->disconnect(this);
            member.socket->abort();
            member.socket->deleteLater();
        }
    }
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->close(QWebSocketProtocol::CloseCodeNormal, QStringLiteral("The room closed"));
        m_socket->deleteLater();
    }
}

void RelayBridge::start() {
    connectToRelay();
}

QString RelayBridge::roomUrl() const {
    QUrl url = m_relay;
    QString path = url.path();
    if (path.endsWith(u'/')) {
        path.chop(1);
    }
    url.setPath(path + QStringLiteral("/r/") + m_roomId);
    url.setQuery(QString());
    return url.toString();
}

void RelayBridge::connectToRelay() {
    if (m_stopped) {
        return;
    }
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->deleteLater();
    }
    m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    m_socket->setMaxAllowedIncomingMessageSize(64 * 1024 * 1024);
    connect(m_socket, &QWebSocket::binaryMessageReceived, this, &RelayBridge::onRelayData);
    connect(m_socket, &QWebSocket::textMessageReceived, this, &RelayBridge::onRelayText);
    connect(m_socket, &QWebSocket::disconnected, this, &RelayBridge::onRelayLost);
    connect(m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (m_socket && m_socket->state() != QAbstractSocket::ConnectedState) {
            onRelayLost();
        }
    });
    QUrl url = m_relay;
    QString path = url.path();
    if (path.endsWith(u'/')) {
        path.chop(1);
    }
    url.setPath(path + QStringLiteral("/host"));
    if (!m_roomId.isEmpty()) {
        // The same room as before, so members' links still work.
        url.setQuery(QStringLiteral("room=") + m_roomId);
    }
    QNetworkRequest request(url);
    if (!m_token.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    }
    qCInfo(lcPeers).noquote() << "Connecting to the relay at" << m_relay.host();
    m_socket->open(request);
}

void RelayBridge::onRelayText(const QString& text) {
    const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString room = message.value(QStringLiteral("room")).toString();
    if (room.isEmpty()) {
        return;
    }
    const bool first = m_roomId.isEmpty();
    if (!first && room != m_roomId) {
        // Someone else took the id meanwhile: the old links lead nowhere now.
        qCWarning(lcPeers).noquote() << "The relay gave this room another id; links made before don't work";
    }
    m_roomId = room;
    m_onRelay = true;
    m_retries = 0;
    qCInfo(lcPeers).noquote() << "The relay has this room as" << room;
    Q_EMIT ready(room);
}

void RelayBridge::onRelayLost() {
    if (m_stopped || !m_socket) {
        return;
    }
    m_onRelay = false;
    const int code = m_socket->closeCode();
    const QString reason = m_socket->closeReason();
    m_socket->disconnect(this);
    m_socket->deleteLater();
    m_socket = nullptr;
    // The members reconnect through the relay once it has the room again.
    for (auto it = m_members.begin(); it != m_members.end(); ++it) {
        if (it->socket) {
            it->socket->disconnect(this);
            it->socket->abort();
            it->socket->deleteLater();
        }
    }
    m_members.clear();
    m_batch.clear();
    const int delay = std::min(30000, 500 << std::min(m_retries, 6));
    ++m_retries;
    qCInfo(lcPeers).noquote() << "Lost the relay; trying again in" << delay << "ms";
    Q_EMIT failed(code == QWebSocketProtocol::CloseCodePolicyViolated && !reason.isEmpty()
                      ? reason
                      : tr("Lost the connection to the relay"),
                  false);
    m_retry->start(delay);
}

void RelayBridge::onRelayData(const QByteArray& data) {
    if (data.isEmpty()) {
        return;
    }
    const char kind = data[0];
    if (data.size() < 5) {
        return;
    }
    const quint32 id = qFromBigEndian<quint32>(data.constData() + 1);
    if (kind == kOpened) {
        openMember(id);
    } else if (kind == kData) {
        auto found = m_members.find(id);
        if (found == m_members.end()) {
            return;
        }
        const QByteArray payload = data.mid(5);
        if (found->open) {
            found->socket->sendBinaryMessage(payload);
        } else {
            found->waiting.append(payload);
        }
    } else if (kind == kClosed) {
        auto found = m_members.find(id);
        if (found != m_members.end()) {
            QWebSocket* socket = found->socket;
            m_members.erase(found);
            if (socket) {
                socket->disconnect(this);
                // As the member's own connection would end: the room keeps a peer that
                // lost it for a while, waiting for it to come back (PEERS_PROTOCOL.md §6).
                socket->abort();
                socket->deleteLater();
            }
        }
    }
}

void RelayBridge::openMember(quint32 id) {
    auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket->setMaxAllowedIncomingMessageSize(64 * 1024 * 1024);
    m_members.insert(id, {socket, {}, false});
    connect(socket, &QWebSocket::connected, this, [this, id] {
        auto found = m_members.find(id);
        if (found == m_members.end()) {
            return;
        }
        found->open = true;
        for (const QByteArray& waiting : std::as_const(found->waiting)) {
            found->socket->sendBinaryMessage(waiting);
        }
        found->waiting.clear();
    });
    connect(socket, &QWebSocket::binaryMessageReceived, this, [this, id](const QByteArray& data) { fromRoom(id, data); });
    connect(socket, &QWebSocket::disconnected, this, [this, id, socket] {
        if (!m_members.contains(id)) {
            return;
        }
        m_members.remove(id);
        flush();  // What was meant for it before it ended goes first
        const QByteArray reason = socket->closeReason().toUtf8().left(120);
        send(QByteArray(1, kClosed) + u32(id) + u16(quint16(socket->closeCode())) + reason);
        socket->deleteLater();
    });
    socket->open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(m_port)));
}

void RelayBridge::fromRoom(quint32 id, const QByteArray& data) {
    if (!data.isEmpty() && data[0] == kRoomMarker) {
        // Sealed for the room: the same bytes go to everyone it is for. Gathered, and
        // sent once with the list.
        for (auto& entry : m_batch) {
            if (entry.first == data && !entry.second.contains(id) && entry.second.size() < kMaxCopies) {
                entry.second.append(id);
                return;
            }
        }
        m_batch.append({data, {id}});
        if (!m_flushScheduled) {
            m_flushScheduled = true;
            QTimer::singleShot(1, this, &RelayBridge::flush);
        }
        return;
    }
    // Anything else is this member's alone; what waits for it goes first.
    flush();
    send(QByteArray(1, kData) + u32(id) + data);
}

void RelayBridge::flush() {
    m_flushScheduled = false;
    const auto batch = std::exchange(m_batch, {});
    for (const auto& [bytes, members] : batch) {
        if (members.size() == 1) {
            send(QByteArray(1, kData) + u32(members.first()) + bytes);
            continue;
        }
        QByteArray frame(1, kDataMany);
        frame += u16(quint16(members.size()));
        for (const quint32 member : members) {
            frame += u32(member);
        }
        frame += bytes;
        m_copiesSaved += qint64(members.size() - 1) * bytes.size();
        send(frame);
    }
}

void RelayBridge::send(const QByteArray& frame) {
    if (m_socket && m_socket->isValid()) {
        m_bytesSent += frame.size();
        m_socket->sendBinaryMessage(frame);
    }
}

}  // namespace usdcc::peers
