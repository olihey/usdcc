#include "page_server.h"

#include "log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

namespace usdcc::peers {

namespace {

QByteArray mimeType(const QString& file) {
    const QString suffix = QFileInfo(file).suffix().toLower();
    if (suffix == u"html" || suffix == u"htm") {
        return "text/html; charset=utf-8";
    }
    if (suffix == u"js" || suffix == u"mjs") {
        return "text/javascript; charset=utf-8";
    }
    if (suffix == u"css") {
        return "text/css; charset=utf-8";
    }
    if (suffix == u"json") {
        return "application/json";
    }
    if (suffix == u"svg") {
        return "image/svg+xml";
    }
    if (suffix == u"png") {
        return "image/png";
    }
    if (suffix == u"jpg" || suffix == u"jpeg") {
        return "image/jpeg";
    }
    if (suffix == u"ico") {
        return "image/x-icon";
    }
    if (suffix == u"wasm") {
        return "application/wasm";
    }
    return "application/octet-stream";
}

// A request head bigger than this is refused (a browser's is a few hundred bytes),
// and one that doesn't arrive in time is dropped.
constexpr qsizetype kMaxHeadBytes = 16 * 1024;
constexpr int kHeadTimeoutMs = 10'000;
const char* const kHeadTimerName = "usdccPageServerHead";

struct RequestHead {
    QByteArray method;
    QByteArray target;
    QHash<QByteArray, QByteArray> headers;  // Lower-case names

    QByteArray value(const QByteArray& name) const { return headers.value(name); }
};

bool parseHead(const QByteArray& raw, RequestHead* head) {
    const QList<QByteArray> lines = raw.split('\n');
    if (lines.isEmpty()) {
        return false;
    }
    const QList<QByteArray> request = lines.first().trimmed().split(' ');
    if (request.size() != 3 || request[0].isEmpty() || request[1].isEmpty() || !request[2].startsWith("HTTP/1.")) {
        return false;
    }
    head->method = request[0];
    head->target = request[1];
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            return false;
        }
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        QByteArray& stored = head->headers[name];
        stored = stored.isEmpty() ? value : stored + ", " + value;
    }
    return true;
}

// Whether a header holding a comma-separated list has `token`, ignoring case.
bool hasToken(const QByteArray& list, const QByteArray& token) {
    for (const QByteArray& entry : list.split(',')) {
        if (entry.trimmed().compare(token, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

// Writes the whole response and closes the connection once it is sent: one request
// per connection, which is all a guest's page needs.
void respond(QTcpSocket* socket, int status, const QByteArray& reason, const QByteArray& data,
             const QByteArray& type) {
    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    out += "Content-Type: " + type + "\r\n";
    out += "Content-Length: " + QByteArray::number(data.size()) + "\r\n";
    // The page holds nothing secret (the secret is in the link's #, which no request
    // carries), but it shouldn't be kept, framed by another site, or sniffed as
    // something else.
    out += "Cache-Control: no-store\r\n"
           "X-Content-Type-Options: nosniff\r\n"
           "X-Frame-Options: DENY\r\n"
           "Referrer-Policy: no-referrer\r\n"
           "Connection: close\r\n\r\n";
    out += data;
    socket->write(out);
    socket->disconnectFromHost();
}

void notFound(QTcpSocket* socket) {
    respond(socket, 404, "Not Found", "Not found\n", "text/plain; charset=utf-8");
}

}  // namespace

PageServer::PageServer(QObject* parent)
    : QObject(parent), m_webSockets(new QWebSocketServer(QString(), QWebSocketServer::NonSecureMode, this)) {
    connect(m_webSockets, &QWebSocketServer::newConnection, this, [this] {
        while (m_webSockets->hasPendingConnections()) {
            QWebSocket* socket = m_webSockets->nextPendingConnection();
            if (!socket) {
                continue;
            }
            if (onWebSocket) {
                onWebSocket(socket);
            } else {
                socket->abort();
                socket->deleteLater();
            }
        }
    });
}

PageServer::~PageServer() = default;

void PageServer::bind(QTcpServer* server) {
    connect(server, &QTcpServer::newConnection, this, [this, server] {
        while (QTcpSocket* socket = server->nextPendingConnection()) {
            auto* timer = new QTimer(socket);
            timer->setObjectName(QLatin1String(kHeadTimerName));
            timer->setSingleShot(true);
            connect(timer, &QTimer::timeout, this, [socket] { socket->abort(); });
            timer->start(kHeadTimeoutMs);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [socket] { socket->deleteLater(); });
            if (socket->bytesAvailable() > 0) {
                onReadyRead(socket);
            }
        }
    });
}

bool PageServer::originAllowed(const QByteArray& origin, const QByteArray& host, const QStringList& allowed,
                               bool guests) {
    if (origin.isEmpty()) {
        return true;  // A program, not a page; the secret decides
    }
    if (!guests) {
        return false;  // A page can only be a guest
    }
    const QString text = QString::fromUtf8(origin).toLower();
    const QString own = QString::fromUtf8(host).toLower();
    if (!own.isEmpty() && (text == u"http://" + own || text == u"https://" + own)) {
        return true;
    }
    for (const QString& entry : allowed) {
        if (text == entry.toLower()) {
            return true;
        }
    }
    return false;
}

void PageServer::onReadyRead(QTcpSocket* socket) {
    // Peeked, not read: an upgrade goes to the QWebSocketServer with the head still in
    // the socket, for it to read again.
    const QByteArray buffered = socket->peek(kMaxHeadBytes + 1);
    const qsizetype end = buffered.indexOf("\r\n\r\n");
    if (end < 0) {
        if (buffered.size() > kMaxHeadBytes) {
            disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
            respond(socket, 431, "Request Header Fields Too Large", "Request header fields too large\n",
                    "text/plain; charset=utf-8");
        }
        return;  // Not all there yet
    }
    // From here on this connection's head is decided: nothing more is read here.
    disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
    delete socket->findChild<QTimer*>(QLatin1String(kHeadTimerName), Qt::FindDirectChildrenOnly);

    RequestHead head;
    if (!parseHead(buffered.left(end), &head)) {
        respond(socket, 400, "Bad Request", "Bad request\n", "text/plain; charset=utf-8");
        return;
    }
    const QString path = QUrl::fromEncoded(head.target).path();
    if (hasToken(head.value("upgrade"), "websocket")) {
        if (path != u"/ws") {
            notFound(socket);
            return;
        }
        const QByteArray origin = head.value("origin");
        if (!originAllowed(origin, head.value("host"), allowedOrigins, guests)) {
            // Who tried, not the secret: there is none in an upgrade.
            qCWarning(lcPeers) << "Refused a WebSocket from the page at" << origin;
            respond(socket, 403, "Forbidden", "Forbidden\n", "text/plain; charset=utf-8");
            return;
        }
        // Ours no more: the QWebSocket it becomes owns it.
        disconnect(socket, nullptr, this, nullptr);
        m_webSockets->handleConnection(socket);
        return;
    }
    socket->read(end + 4);  // The head; any body isn't wanted
    answerGet(socket, head.method, path);
}

void PageServer::answerGet(QTcpSocket* socket, const QByteArray& method, const QString& path) {
    if (!guests) {
        notFound(socket);
        return;
    }
    if (method != "GET") {
        respond(socket, 405, "Method Not Allowed", "Method not allowed\n", "text/plain; charset=utf-8");
        return;
    }
    if (path == u"/" || path == u"/index.html") {
        if (!pageDirectory.isEmpty()) {
            QFile file(QDir(pageDirectory).filePath(QStringLiteral("index.html")));
            if (file.open(QIODevice::ReadOnly)) {
                respond(socket, 200, "OK", file.readAll(), "text/html; charset=utf-8");
                return;
            }
        }
        respond(socket, 200, "OK", page.isEmpty() ? builtInGuestPage() : page, "text/html; charset=utf-8");
        return;
    }
    // A file directly in the page's folder, by a plain name: nothing above it or in
    // a folder of it.
    static const QRegularExpression plainName(QStringLiteral("^/[A-Za-z0-9][A-Za-z0-9._-]*$"));
    if (!pageDirectory.isEmpty() && plainName.match(path).hasMatch() && !path.contains(QLatin1String(".."))) {
        const QString name = path.mid(1);
        QFile file(QDir(pageDirectory).filePath(name));
        if (QFileInfo(file).isFile() && file.open(QIODevice::ReadOnly)) {
            respond(socket, 200, "OK", file.readAll(), mimeType(name));
            return;
        }
    }
    notFound(socket);
}

}  // namespace usdcc::peers
