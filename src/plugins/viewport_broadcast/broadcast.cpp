#include "broadcast.h"

#include "usdcc/peers/channel.h"
#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"

#include <QBuffer>
#include <QJsonArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QImageWriter>
#include <QLoggingCategory>
#include <QThreadPool>

#include <cmath>

namespace usdcc::broadcast {

namespace {

Q_LOGGING_CATEGORY(lcBroadcast, "usdcc.broadcast")

using usdcc::peers::Channel;
using usdcc::peers::Delivery;
using usdcc::peers::Peer;
using usdcc::peers::Room;

constexpr char kChatChannel[] = "chat";

bool isWatcher(const Peer& peer) { return peer.kind == Peer::Kind::Guest; }

QByteArray toJpeg(const QImage& image, int quality) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(quality);
    // Empty when Qt's JPEG plugin (imageformats/qjpeg.dll) isn't beside the app.
    return writer.write(image) ? bytes : QByteArray();
}

}  // namespace

Broadcast::Broadcast(QObject* parent) : QObject(parent) {
    m_still.setSingleShot(true);
    connect(&m_still, &QTimer::timeout, this, [this] {
        // The picture stopped changing: the same one again, sharper (§3).
        m_sharpPending = true;
        sendNext();
    });
}

Broadcast* Broadcast::start(usdcc::peers::Peers* peers, FrameSource* source, const Options& options, QString* error,
                            QObject* parent) {
    if (!peers || !source) {
        if (error) *error = QStringLiteral("Nothing to broadcast");
        return nullptr;
    }
    usdcc::peers::HostOptions host;
    host.name = options.name;
    host.address = options.address;
    host.port = options.port;
    host.admission = usdcc::peers::Admission::Invited;  // The link is the invitation, and it isn't announced
    host.guests = true;
    host.pageDirectory = options.pageDirectory;
    host.allowedOrigins = options.allowedOrigins;
    // The tunnel's pages come from its origin: they may open the WebSocket.
    if (options.publicAddress.isValid() && !options.publicAddress.host().isEmpty()) {
        const QString origin = options.publicAddress.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment |
                                                              QUrl::RemoveUserInfo | QUrl::StripTrailingSlash)
                                   .toString();
        if (!host.allowedOrigins.contains(origin)) host.allowedOrigins.append(origin);
    }
    host.limits.maxGuests = options.maxGuests;
    Room* room = peers->host(host, error);
    if (!room) return nullptr;

    auto* broadcast = new Broadcast(parent);
    broadcast->m_room = room;
    broadcast->m_source = source;
    broadcast->m_options = options;

    Channel* view = room->channel(QString::fromLatin1(kViewChannel), Delivery::LatestOnly);
    view->setSenders(usdcc::peers::Senders::Host);
    view->setOpenToGuests(true);
    // A newcomer sees the picture at once, not at the next change.
    view->setSnapshotProvider([broadcast] {
        Channel::Snapshot snapshot;
        snapshot.body = broadcast->frameBody();
        snapshot.bytes = broadcast->m_latest;
        return snapshot;
    });
    broadcast->m_view = view;
    // The room's chat, which the Peers panel shows: watchers talk there too.
    room->channel(QString::fromLatin1(kChatChannel))->setOpenToGuests(true);
    // Navigating: watchers ask and move, to the host only.
    Channel* nav = room->channel(QString::fromLatin1(kNavChannel), Delivery::Direct);
    nav->setOpenToGuests(true);
    broadcast->m_nav = nav;
    connect(nav, &Channel::received, broadcast, &Broadcast::onNav);

    connect(room, &Room::peerJoined, broadcast, [broadcast](const Peer& peer) {
        if (!isWatcher(peer)) return;
        qCInfo(lcBroadcast).noquote() << peer.name << "is watching";
        Q_EMIT broadcast->watcherJoined(peer);
        Q_EMIT broadcast->watchersChanged();
        broadcast->tellNavigator(peer.id);
        // Nothing is grabbed while nobody watches: the newcomer's snapshot may be old,
        // and a change since then is sent now.
        broadcast->sendNext();
    });
    connect(room, &Room::peerLeft, broadcast, [broadcast](const Peer& peer, const QString&) {
        if (!isWatcher(peer)) return;
        if (broadcast->m_asking.removeAll(peer.id) > 0 && peer.id != broadcast->m_navigator) {
            Q_EMIT broadcast->navigatorChanged();
        }
        if (peer.id == broadcast->m_navigator) broadcast->setNavigator({});
        Q_EMIT broadcast->watchersChanged();
    });
    connect(room, &Room::closed, broadcast, [broadcast](const QString& reason) {
        if (broadcast->m_stopped) return;
        broadcast->m_stopped = true;
        broadcast->m_still.stop();
        qCInfo(lcBroadcast).noquote() << "Broadcast ended:" << reason;
        Q_EMIT broadcast->stopped(reason);
    });
    connect(source, &FrameSource::frameChanged, broadcast, &Broadcast::frameChanged);
    connect(source, &FrameSource::gone, broadcast, [broadcast](const QString& reason) { broadcast->stop(reason); });
    connect(source, &QObject::destroyed, broadcast, [broadcast] { broadcast->stop(QStringLiteral("The viewport closed")); });

    qCInfo(lcBroadcast).noquote() << "Broadcasting" << source->title() << "on port" << room->port();
    broadcast->frameChanged();
    return broadcast;
}

Broadcast::~Broadcast() {
    m_still.stop();
    if (!m_room) return;
    // Not told of its own closing: nothing of this is left to tell.
    disconnect(m_room, nullptr, this, nullptr);
    if (!m_stopped && m_room->state() != Room::State::Closed) m_room->close(QStringLiteral("The broadcast ended"));
}

usdcc::peers::Room* Broadcast::room() const { return m_room; }

QUrl Broadcast::localLink() const { return m_room ? m_room->guestLink() : QUrl(); }

QUrl Broadcast::link() const {
    const QUrl local = localLink();
    const QUrl& address = m_options.publicAddress;
    if (local.isEmpty() || !address.isValid() || address.host().isEmpty()) return local;
    // The tunnel's address, its path ending in "/" so the page's own files resolve under it.
    QUrl url = address.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo);
    if (!url.path().endsWith(u'/')) url.setPath(url.path() + u'/');
    url.setFragment(local.fragment());
    return url;
}

QList<Peer> Broadcast::watchers() const {
    QList<Peer> result;
    if (!m_room) return result;
    for (const Peer& peer : m_room->peers()) {
        if (isWatcher(peer)) result.append(peer);
    }
    return result;
}

bool Broadcast::isLive() const { return !m_stopped && m_room && m_room->state() != Room::State::Closed; }

void Broadcast::stop(const QString& reason) {
    if (!isLive()) return;
    m_room->close(reason.isEmpty() ? QStringLiteral("The broadcast ended") : reason);
}

bool Broadcast::allowNavigating(const QString& watcher) {
    if (!isLive() || !m_room->peer(watcher).isValid()) return false;
    m_asking.removeAll(watcher);
    setNavigator(watcher);
    return true;
}

void Broadcast::denyNavigating(const QString& watcher) {
    if (m_asking.removeAll(watcher) == 0) return;
    if (m_nav && isLive()) m_nav->send(QStringLiteral("denied"), {}, {}, watcher);
    Q_EMIT navigatorChanged();
}

void Broadcast::takeBack() { setNavigator({}); }

void Broadcast::setNavigator(const QString& id) {
    if (id == m_navigator) return;
    m_navigator = id;
    qCInfo(lcBroadcast).noquote() << (id.isEmpty() ? QStringLiteral("Nobody navigates")
                                                   : QStringLiteral("%1 navigates").arg(m_room->peer(id).name));
    tellNavigator();
    Q_EMIT navigatorChanged();
}

// Who navigates, to each watcher (or to `to` alone): the page shows it, and the one
// named moves the camera.
void Broadcast::tellNavigator(const QString& to) {
    if (!m_nav || !isLive()) return;
    const QJsonObject body{{QStringLiteral("id"), m_navigator},
                           {QStringLiteral("name"), m_navigator.isEmpty() ? QString() : m_room->peer(m_navigator).name}};
    for (const Peer& watcher : watchers()) {
        if (to.isEmpty() || watcher.id == to) m_nav->send(QStringLiteral("navigator"), body, {}, watcher.id);
    }
}

// What a watcher sends on the nav channel: a closed list (§9), each checked.
void Broadcast::onNav(const usdcc::peers::Message& message) {
    if (!isLive() || !m_room) return;
    const Peer from = m_room->peer(message.from);
    if (!isWatcher(from)) return;
    const QString& type = message.type;
    if (type == QLatin1String("ask")) {
        if (from.id == m_navigator || m_asking.contains(from.id)) return;
        m_asking.append(from.id);
        qCInfo(lcBroadcast).noquote() << from.name << "asks to navigate";
        Q_EMIT navigateAsked(from);
        Q_EMIT navigatorChanged();
        return;
    }
    if (type == QLatin1String("cancel")) {
        // Asked and changed their mind, or navigated and stopped.
        if (m_asking.removeAll(from.id) > 0) Q_EMIT navigatorChanged();
        if (from.id == m_navigator) setNavigator({});
        return;
    }
    static const QStringList moves{QStringLiteral("orbit"), QStringLiteral("pan"), QStringLiteral("zoom"),
                                   QStringLiteral("fit")};
    if (!moves.contains(type)) {
        m_room->remove(from.id, QStringLiteral("The page sent something a watcher can't"));
        return;
    }
    // Fractions of the picture, at most a whole one a move; zoom in notches.
    const double x = message.body.value(type == QLatin1String("zoom") ? QStringLiteral("steps") : QStringLiteral("x")).toDouble();
    const double y = message.body.value(QStringLiteral("y")).toDouble();
    const double limit = type == QLatin1String("zoom") ? 10.0 : 1.0;
    if (from.id != m_navigator || !std::isfinite(x) || !std::isfinite(y) || std::abs(x) > limit || std::abs(y) > limit ||
        !m_source || !m_source->navigate(type, x, y)) {
        ++m_stats.movesRefused;
        return;
    }
    ++m_stats.moves;
}

QJsonObject Broadcast::frameBody() const {
    return {{QStringLiteral("w"), m_latestSize.width()},
            {QStringLiteral("h"), m_latestSize.height()},
            {QStringLiteral("title"), m_source ? m_source->title() : QString()}};
}

void Broadcast::frameChanged() {
    m_dirty = true;
    m_still.start(m_options.stillAfterMs);
    sendNext();
}

// Grabs and compresses the newest picture when the last one is done: at the encoder's
// pace, not the viewport's, and never two at once. Sending is latest only, so a slow
// watcher gets fewer pictures, never older ones.
void Broadcast::sendNext() {
    if (!isLive() || m_encoding || !m_source) return;
    if (watchers().isEmpty()) return;  // Grabbing costs the UI thread: only for someone
    if (m_dirty) {
        m_dirty = false;
        // Once the picture has been still for a while (the timer ran out before anyone
        // watched), the sharp one follows this one at once; else when the timer runs out.
        m_sharpPending = !m_still.isActive();
        const QImage image = m_source->grab();
        if (image.isNull()) return;
        m_lastImage = image;
        encode(image, m_options.movingQuality);
    } else if (m_sharpPending) {
        m_sharpPending = false;
        if (!m_lastImage.isNull() && m_lastQuality < m_options.stillQuality) encode(m_lastImage, m_options.stillQuality);
    }
}

void Broadcast::encode(const QImage& image, int quality) {
    m_encoding = true;
    const size_t lastHash = m_lastHash;
    const int lastQuality = m_lastQuality;
    const int maxWidth = m_options.maxWidth;
    const bool sharp = quality >= m_options.stillQuality;
    QPointer<Broadcast> self(this);
    QThreadPool::globalInstance()->start([self, image, quality, lastHash, lastQuality, maxWidth, sharp] {
        QElapsedTimer timer;
        timer.start();
        const size_t hash = qHashBits(image.constBits(), static_cast<size_t>(image.sizeInBytes()));
        // The same picture again, no sharper than it went: nothing to send.
        const bool unchanged = hash == lastHash && quality <= lastQuality;
        QByteArray jpeg;
        QSize size = image.size();
        if (!unchanged) {
            const QImage scaled = image.width() > maxWidth
                                      ? image.scaledToWidth(maxWidth, sharp ? Qt::SmoothTransformation : Qt::FastTransformation)
                                      : image;
            jpeg = toJpeg(scaled, quality);
            size = scaled.size();
        }
        const double ms = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        QMetaObject::invokeMethod(
            qApp,
            [self, jpeg, size, hash, quality, unchanged, ms] {
                if (self) self->encoded(jpeg, size, hash, quality, unchanged, ms);
            },
            Qt::QueuedConnection);
    });
}

void Broadcast::encoded(const QByteArray& jpeg, QSize size, size_t hash, int quality, bool unchanged, double ms) {
    m_encoding = false;
    if (unchanged) {
        ++m_stats.unchanged;
    } else if (jpeg.isEmpty()) {
        if (!m_warnedEncoder) qCWarning(lcBroadcast) << "No JPEG: is imageformats/qjpeg.dll beside the app?";
        m_warnedEncoder = true;
    } else if (isLive() && m_view) {
        m_lastHash = hash;
        m_lastQuality = quality;
        m_latest = jpeg;
        m_latestSize = size;
        m_view->send(QStringLiteral("frame"), frameBody(), jpeg);
        ++m_stats.sent;
        m_stats.bytes += jpeg.size();
        m_stats.encodeMs = ms;
        m_stats.size = size;
    }
    sendNext();
}

}  // namespace usdcc::broadcast
