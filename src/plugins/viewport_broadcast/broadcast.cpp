#include "broadcast.h"

#include "usdcc/peers/channel.h"
#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QImageWriter>
#include <QLoggingCategory>
#include <QThreadPool>

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

    connect(room, &Room::peerJoined, broadcast, [broadcast](const Peer& peer) {
        if (!isWatcher(peer)) return;
        qCInfo(lcBroadcast).noquote() << peer.name << "is watching";
        Q_EMIT broadcast->watcherJoined(peer);
        Q_EMIT broadcast->watchersChanged();
        // Nothing is grabbed while nobody watches: the newcomer's snapshot may be old,
        // and a change since then is sent now.
        broadcast->sendNext();
    });
    connect(room, &Room::peerLeft, broadcast, [broadcast](const Peer& peer, const QString&) {
        if (isWatcher(peer)) Q_EMIT broadcast->watchersChanged();
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

QUrl Broadcast::link() const { return m_room ? m_room->guestLink() : QUrl(); }

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
        m_sharpPending = false;
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
