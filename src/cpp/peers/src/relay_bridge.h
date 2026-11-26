#pragma once

// A room hosted through a relay (PEERS.md §5.4, PEERS_PROTOCOL.md §11). The host connects
// out to the relay and gets a room id there; each member that connects to that room comes
// in over the same connection. The bridge gives each such member a WebSocket of its own to
// the room's port on the loopback, so the room takes it like any member, and passes the
// bytes both ways. The relay can't read them: they are sealed for the connection or the
// room before they leave.
//
// A message to everyone, sealed once for the room (the same bytes for each member), goes
// to the relay once with the list of members it is for, and the relay copies it: a host on
// a home connection doesn't upload it once per member. A member's own messages keep their
// order: what waits to go as one copy is sent before anything else for that member.
//
// If the connection to the relay is lost, the members' loopback connections end (they
// reconnect through the relay and catch up, PEERS_PROTOCOL.md §6), and the bridge connects
// again, asking for the same room id.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QTimer;
class QWebSocket;

namespace usdcc::peers {

class RelayBridge : public QObject {
    Q_OBJECT

public:
    // `relay`: the relay's base URL (wss://relay.example or ws://host:port); `token`: what
    // it asks of a host, if anything; `port`: the room's own, on 127.0.0.1.
    RelayBridge(const QUrl& relay, const QString& token, quint16 port, QObject* parent = nullptr);
    ~RelayBridge() override;

    void start();
    // The room id the relay gave, once it has (ready()); empty before.
    QString roomId() const { return m_roomId; }
    // <relay>/r/<room id>: where members connect.
    QString roomUrl() const;
    // The relay has the room now: given an id, and not lost since.
    bool isReady() const { return m_onRelay && !m_roomId.isEmpty(); }

    // Bytes to and from the relay, as counted here (for the log and the tests).
    qint64 bytesSent() const { return m_bytesSent; }
    qint64 copiesSaved() const { return m_copiesSaved; }

Q_SIGNALS:
    void ready(const QString& roomId);
    // The relay can't be reached, was lost, or turned the host down (a wrong token: the
    // relay answers 401). The bridge keeps trying, waiting longer each time.
    void failed(const QString& reason, bool final);

private:
    struct Member {
        QWebSocket* socket = nullptr;
        QList<QByteArray> waiting;  // From the member, before the loopback connection is open
        bool open = false;
    };

    void connectToRelay();
    void onRelayData(const QByteArray& data);
    void onRelayText(const QString& text);
    void onRelayLost();
    void openMember(quint32 id);
    void closeMember(quint32 id, int code, const QString& reason);
    void fromRoom(quint32 id, const QByteArray& data);
    void flush();
    void send(const QByteArray& frame);

    QUrl m_relay;
    QString m_token;
    quint16 m_port;
    QWebSocket* m_socket = nullptr;
    QString m_roomId;  // Kept when the relay is lost, to ask for it again
    bool m_onRelay = false;
    QHash<quint32, Member> m_members;
    // Room envelopes waiting to go as one copy, in the order they came: the bytes, and
    // the members they are for.
    QList<QPair<QByteArray, QList<quint32>>> m_batch;
    bool m_flushScheduled = false;
    int m_retries = 0;
    bool m_stopped = false;
    QTimer* m_retry = nullptr;
    qint64 m_bytesSent = 0;
    qint64 m_copiesSaved = 0;
};

}  // namespace usdcc::peers
