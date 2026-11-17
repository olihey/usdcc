#include "page_server.h"

#include "log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpHeaders>
#include <QHttpServerRequest>
#include <QHttpServerResponder>
#include <QHttpServerWebSocketUpgradeResponse>
#include <QRegularExpression>

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

void respond(QHttpServerResponder& responder, const QByteArray& data, const QByteArray& type,
             QHttpServerResponder::StatusCode status = QHttpServerResponder::StatusCode::Ok) {
    QHttpHeaders headers;
    headers.append(QHttpHeaders::WellKnownHeader::ContentType, type);
    // The page holds nothing secret (the secret is in the link's #, which no request
    // carries), but it shouldn't be kept, framed by another site, or sniffed as
    // something else.
    headers.append(QHttpHeaders::WellKnownHeader::CacheControl, "no-store");
    headers.append("X-Content-Type-Options", "nosniff");
    headers.append("X-Frame-Options", "DENY");
    headers.append("Referrer-Policy", "no-referrer");
    responder.write(data, headers, status);
}

void notFound(QHttpServerResponder& responder) {
    respond(responder, "Not found\n", "text/plain; charset=utf-8", QHttpServerResponder::StatusCode::NotFound);
}

}  // namespace

PageServer::PageServer(QObject* parent) : QAbstractHttpServer(parent) {
    addWebSocketUpgradeVerifier(this, [this](const QHttpServerRequest& request) {
        if (request.url().path() != u"/ws") {
            return QHttpServerWebSocketUpgradeResponse::deny(404, "Not found");
        }
        const QByteArray origin = request.value("origin");
        if (!originAllowed(origin, request.value("host"), allowedOrigins, guests)) {
            // Who tried, not the secret: there is none in an upgrade.
            qCWarning(lcPeers) << "Refused a WebSocket from the page at" << origin;
            return QHttpServerWebSocketUpgradeResponse::deny(403, "Forbidden");
        }
        return QHttpServerWebSocketUpgradeResponse::accept();
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

bool PageServer::handleRequest(const QHttpServerRequest& request, QHttpServerResponder& responder) {
    if (!guests) {
        notFound(responder);
        return true;
    }
    if (request.method() != QHttpServerRequest::Method::Get) {
        respond(responder, "Method not allowed\n", "text/plain; charset=utf-8",
                QHttpServerResponder::StatusCode::MethodNotAllowed);
        return true;
    }
    const QString path = request.url().path();
    if (path == u"/" || path == u"/index.html") {
        if (!pageDirectory.isEmpty()) {
            QFile file(QDir(pageDirectory).filePath(QStringLiteral("index.html")));
            if (file.open(QIODevice::ReadOnly)) {
                respond(responder, file.readAll(), "text/html; charset=utf-8");
                return true;
            }
        }
        respond(responder, page.isEmpty() ? builtInGuestPage() : page, "text/html; charset=utf-8");
        return true;
    }
    // A file directly in the page's folder, by a plain name: nothing above it or in
    // a folder of it.
    static const QRegularExpression plainName(QStringLiteral("^/[A-Za-z0-9][A-Za-z0-9._-]*$"));
    if (!pageDirectory.isEmpty() && plainName.match(path).hasMatch() && !path.contains(QLatin1String(".."))) {
        const QString name = path.mid(1);
        QFile file(QDir(pageDirectory).filePath(name));
        if (QFileInfo(file).isFile() && file.open(QIODevice::ReadOnly)) {
            respond(responder, file.readAll(), mimeType(name));
            return true;
        }
    }
    notFound(responder);
    return true;
}

void PageServer::missingHandler(const QHttpServerRequest&, QHttpServerResponder& responder) {
    notFound(responder);
}

}  // namespace usdcc::peers
