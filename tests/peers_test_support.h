#pragma once

// What the usdcc_peers tests share (peers_test.cpp, peers_rooms_test.cpp,
// peers_discovery_test.cpp, peers_transfer_test.cpp): rooms on 127.0.0.1, a member on a
// thread of its own that the test can stop, a raw WebSocket that speaks the protocol by
// hand, and a proxy that can cut a member off.

#include "test_harness.h"

#include "usdcc/peers/channel.h"
#include "usdcc/peers/frame.h"
#include "usdcc/peers/room.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>
#include <QWebSocket>

#include <functional>
#include <memory>

namespace usdcc::peers::test {

using namespace usdcc::peers;


inline bool waitUntil(const std::function<bool()>& done, int milliseconds = 5000) {
    return QTest::qWaitFor(done, milliseconds);
}

inline std::unique_ptr<Room> hostRoom(HostOptions options = {}) {
    if (options.name.isEmpty()) {
        options.name = QStringLiteral("Test Room");
    }
    QString error;
    std::unique_ptr<Room> room(Room::host(options, nullptr, &error));
    CHECK(room, "couldn't host: %s", qPrintable(error));
    return room;
}

inline QString address(const Room& host) {
    return QStringLiteral("127.0.0.1:%1").arg(host.port());
}

// Joins and waits until the host has welcomed or refused it.
inline std::unique_ptr<Room> joinRoom(Room& host, const QString& name, const QString& secret = {},
                               QString* closedReason = nullptr) {
    JoinOptions options;
    options.profile.name = name;
    options.secret = secret.isEmpty() ? host.secret() : secret;
    std::unique_ptr<Room> room(Room::join(address(host), options));
    if (closedReason) {
        QObject::connect(room.get(), &Room::closed, room.get(), [closedReason](const QString& reason) {
            *closedReason = reason;
        });
    }
    waitUntil([&] { return room->state() != Room::State::Connecting; });
    return room;
}

// What arrived on one channel.
struct Inbox {
    QObject context;
    QList<Message> messages;
    QStringList refusals;

    void listen(Channel* channel) {
        QObject::connect(channel, &Channel::received, &context, [this](const Message& m) { messages.append(m); });
        QObject::connect(channel, &Channel::refused, &context,
                         [this](const QString& type, const QString& reason) { refusals.append(type + ": " + reason); });
    }
    QStringList types() const {
        QStringList result;
        for (const Message& m : messages) {
            result.append(m.type);
        }
        return result;
    }
};

// A member on a thread of its own, so that blocking its event loop makes it slow.
class ThreadedMember {
public:
    ThreadedMember(Room& host, const QString& name, const QString& channelName, Delivery delivery) {
        m_worker.moveToThread(&m_thread);
        m_thread.start();
        const QString to = address(host);
        const QString secret = host.secret();
        QMetaObject::invokeMethod(
            &m_worker,
            [=, this] {
                JoinOptions options;
                options.profile.name = name;
                options.secret = secret;
                room = Room::join(to, options);
                Channel* channel = room->channel(channelName, delivery);
                QObject::connect(channel, &Channel::received, &context,
                                 [this](const Message& m) { messages.append(m); });
                QObject::connect(room, &Room::opened, &context, [this] { opened = true; });
                QObject::connect(room, &Room::resumed, &context, [this](bool caughtUp) {
                    resumedCaughtUp = caughtUp;
                    resumed = true;
                });
                QObject::connect(room, &Room::closed, &context, [this](const QString& reason) {
                    closedReason = reason;
                    closed = true;
                });
            },
            Qt::BlockingQueuedConnection);
        waitUntil([this] { return opened || closed; });
    }

    ~ThreadedMember() {
        unblock();
        QMetaObject::invokeMethod(
            &m_worker,
            [this] {
                delete room;
                room = nullptr;
            },
            Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }

    // Its event loop stops until unblock(): it reads nothing more.
    void block() {
        m_blocked = true;
        QMetaObject::invokeMethod(&m_worker, [this] { m_gate.acquire(); }, Qt::QueuedConnection);
    }
    void unblock() {
        if (m_blocked) {
            m_blocked = false;
            m_gate.release();
        }
    }

    QObject context;  // In the test's thread: what arrives is handed over to it
    Room* room = nullptr;
    QList<Message> messages;
    bool opened = false;
    bool closed = false;
    bool resumed = false;
    bool resumedCaughtUp = false;
    QString closedReason;

private:
    QThread m_thread;
    QObject m_worker;
    QSemaphore m_gate;
    bool m_blocked = false;
};

// A page's WebSocket, speaking the protocol by hand.
struct RawMember {
    QWebSocket socket;
    QList<Message> frames;
    bool connected = false;
    bool disconnected = false;  // Or never connected: the upgrade was refused

    explicit RawMember(const QString& origin = {}) : socket(origin) {
        QObject::connect(&socket, &QWebSocket::binaryMessageReceived, &socket, [this](const QByteArray& data) {
            if (std::optional<Frame> frame = decodeFrame(data)) {
                frames.append(frame->message);
            }
        });
        QObject::connect(&socket, &QWebSocket::connected, &socket, [this] { connected = true; });
        QObject::connect(&socket, &QWebSocket::disconnected, &socket, [this] { disconnected = true; });
        QObject::connect(&socket, &QWebSocket::errorOccurred, &socket, [this] {
            if (!connected) {
                disconnected = true;
            }
        });
    }
    void open(const Room& host) {
        socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(host.port())));
    }
    void sendText(const QString& channel, const QString& type, const QJsonObject& body = {}) {
        const QJsonObject header{{"channel", channel}, {"type", type}, {"body", body}};
        socket.sendTextMessage(QString::fromUtf8(QJsonDocument(header).toJson(QJsonDocument::Compact)));
    }
    // Opens, says hello, and waits for the answer.
    bool hello(const Room& host, const QString& secret, const QString& name, int protocol = kProtocolVersion) {
        open(host);
        if (!waitUntil([this] { return connected || disconnected; }) || !connected) {
            return false;
        }
        sendText(kRoomChannel, "hello", {{"protocol", protocol}, {"secret", secret}, {"name", name}});
        return waitUntil([this] { return find("room", "welcome") || find("room", "denied"); });
    }
    const Message* find(const QString& channel, const QString& type) const {
        for (const Message& m : frames) {
            if (m.channel == channel && m.type == type) {
                return &m;
            }
        }
        return nullptr;
    }
    int count(const QString& channel) const {
        int result = 0;
        for (const Message& m : frames) {
            result += m.channel == channel ? 1 : 0;
        }
        return result;
    }
};

// Passes bytes between members and a host, keeping a copy, and can cut them apart.
class Proxy : public QObject {
public:
    explicit Proxy(quint16 target) : m_target(target) {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* in = m_server.nextPendingConnection()) {
                auto* out = new QTcpSocket(this);
                in->setParent(this);
                m_pairs.append(std::make_pair(in, out));
                connect(in, &QTcpSocket::readyRead, this, [this, in, out] {
                    const QByteArray data = in->readAll();
                    captured += data;
                    out->write(data);
                });
                connect(out, &QTcpSocket::readyRead, this, [this, in, out] {
                    const QByteArray data = out->readAll();
                    captured += data;
                    in->write(data);
                });
                connect(in, &QTcpSocket::disconnected, out, &QTcpSocket::disconnectFromHost);
                connect(out, &QTcpSocket::disconnected, in, &QTcpSocket::disconnectFromHost);
                out->connectToHost(QHostAddress::LocalHost, m_target);
            }
        });
        resume();
    }
    QString address() const { return QStringLiteral("127.0.0.1:%1").arg(m_port); }
    // Drops every connection, and takes none until resume().
    void cut() {
        m_server.close();
        for (const auto& [in, out] : m_pairs) {
            in->abort();
            out->abort();
        }
        m_pairs.clear();
    }
    void resume() {
        m_server.listen(QHostAddress::LocalHost, m_port);
        m_port = m_server.serverPort();
    }

    QByteArray captured;

private:
    quint16 m_target;
    quint16 m_port = 0;
    QTcpServer m_server;
    QList<std::pair<QTcpSocket*, QTcpSocket*>> m_pairs;
};

// Joins at `address` (a proxy's), and waits until the host has welcomed or refused it.
inline std::unique_ptr<Room> joinThrough(const QString& address, const QString& name, const QString& secret,
                                         QString* closedReason = nullptr) {
    JoinOptions options;
    options.profile.name = name;
    options.secret = secret;
    std::unique_ptr<Room> room(Room::join(address, options));
    if (closedReason) {
        QObject::connect(room.get(), &Room::closed, room.get(), [closedReason](const QString& r) { *closedReason = r; });
    }
    waitUntil([&] { return room->state() != Room::State::Connecting; });
    return room;
}

inline PeerId idOf(const Room& host, const QString& name) {
    for (const Peer& peer : host.peers()) {
        if (peer.name == name) {
            return peer.id;
        }
    }
    return {};
}

}  // namespace usdcc::peers::test
