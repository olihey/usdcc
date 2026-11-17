#pragma once

#include <QAbstractHttpServer>
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace usdcc::peers {

// The room's HTTP side (PEERS.md §6.1): the guests' page on `/`, and the WebSocket
// upgrade on `/ws`, both on the room's one port. An upgrade from a page (it has an
// `Origin`) is accepted only from the room's own page or an origin the host allowed,
// so another site open in a guest's browser can't connect; the secret in the first
// message is checked after.
class PageServer : public QAbstractHttpServer {
public:
    explicit PageServer(QObject* parent = nullptr);

    bool guests = false;
    QByteArray page;
    QString pageDirectory;
    QStringList allowedOrigins;

    // Whether a WebSocket upgrade from `origin` (empty for a program) is let through,
    // given the request's Host header.
    static bool originAllowed(const QByteArray& origin, const QByteArray& host, const QStringList& allowed,
                              bool guests);

protected:
    bool handleRequest(const QHttpServerRequest& request, QHttpServerResponder& responder) override;
    void missingHandler(const QHttpServerRequest& request, QHttpServerResponder& responder) override;
};

// The page served when the host gives none: joins, lists who is there, and shows and
// sends `chat` if the room opened it to guests.
QByteArray builtInGuestPage();

}  // namespace usdcc::peers
