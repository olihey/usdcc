#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QTcpServer;
class QTcpSocket;
class QWebSocket;
class QWebSocketServer;

namespace usdcc::peers {

// The room's HTTP side (PEERS.md §6.1): the guests' page on `/`, and the WebSocket
// upgrade on `/ws`, both on the room's one port. An upgrade from a page (it has an
// `Origin`) is accepted only from the room's own page or an origin the host allowed,
// so another site open in a guest's browser can't connect; the secret in the first
// message is checked after.
//
// On Qt Network and Qt WebSockets only (both LGPL-3.0), not Qt HTTP Server, which is
// GPL-3.0 only (RELEASE.md §6): each connection's request head is read here; a GET is
// answered and the connection closed, and an allowed upgrade is handed, unread, to a
// QWebSocketServer.
class PageServer : public QObject {
public:
    explicit PageServer(QObject* parent = nullptr);
    ~PageServer() override;

    bool guests = false;
    QByteArray page;
    QString pageDirectory;
    QStringList allowedOrigins;
    // Each WebSocket that finished its handshake; the callee owns it.
    std::function<void(QWebSocket*)> onWebSocket;

    // Serves the connections `server` (already listening) takes.
    void bind(QTcpServer* server);

    // Whether a WebSocket upgrade from `origin` (empty for a program) is let through,
    // given the request's Host header.
    static bool originAllowed(const QByteArray& origin, const QByteArray& host, const QStringList& allowed,
                              bool guests);

private:
    void onReadyRead(QTcpSocket* socket);
    void answerGet(QTcpSocket* socket, const QByteArray& method, const QString& path);

    QWebSocketServer* m_webSockets = nullptr;
};

// The page served when the host gives none: joins, lists who is there, and shows and
// sends `chat` if the room opened it to guests.
QByteArray builtInGuestPage();

}  // namespace usdcc::peers
