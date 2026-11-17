#pragma once

// A viewport broadcast (docs/VIEWPORT_BROADCAST.md): a room of usdcc_peers that takes
// guests, serves them the page, and sends them a picture whenever it changes. Qt only;
// what the picture is comes from a FrameSource, so the tests give it a fake one.

#include <QHostAddress>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

namespace usdcc::peers {
class Channel;
class Peers;
class Room;
struct Peer;
}  // namespace usdcc::peers

namespace usdcc::broadcast {

// What a broadcast shows (VIEWPORT_BROADCAST.md §8): a picture that changes now and then.
class FrameSource : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    // The picture now; null when there is none yet.
    virtual QImage grab() = 0;
    // What the page calls it: "Viewport 1 · shot.usda".
    virtual QString title() const = 0;

Q_SIGNALS:
    // There is a new picture to grab().
    void frameChanged();
    void titleChanged();
    // It went away (the viewport closed): the broadcast ends.
    void gone(const QString& reason);
};

class Broadcast : public QObject {
    Q_OBJECT

public:
    // The pictures: latest only, from the host, open to guests. The type is "frame", and
    // "snapshot" for the picture a newcomer gets first. The body has the picture's size
    // and the title; the raw bytes are a JPEG.
    static constexpr char kViewChannel[] = "org.usdcc.viewport-broadcast/view";

    struct Options {
        QString name;  // The room's
        QHostAddress address = QHostAddress(QHostAddress::Any);
        quint16 port = 47810;  // Taken: any free one
        QString pageDirectory;  // The page: its index.html and the files beside it
        QStringList allowedOrigins;  // A tunnel's address, for its pages to connect
        int maxGuests = 8;
        int maxWidth = 1920;  // Wider pictures are scaled down
        int movingQuality = 60;
        int stillQuality = 85;
        int stillAfterMs = 250;  // No new picture this long: one sharp one follows
    };

    // Hosts the room through `peers`, so it shows in the Peers panel with its watchers
    // and chat. nullptr, with why in `error`, when it can't listen.
    static Broadcast* start(usdcc::peers::Peers* peers, FrameSource* source, const Options& options,
                            QString* error = nullptr, QObject* parent = nullptr);
    ~Broadcast() override;

    usdcc::peers::Room* room() const;
    FrameSource* source() const { return m_source; }
    // The web link for watchers: http://<address>:<port>/#s=<guests' secret>.
    QUrl link() const;
    QList<usdcc::peers::Peer> watchers() const;
    bool isLive() const;
    // Closes the room; the page says why. stopped() follows.
    void stop(const QString& reason = {});

    struct Stats {
        int sent = 0;           // Pictures sent, snapshots aside
        int unchanged = 0;      // Grabbed, but equal to the last one sent
        qint64 bytes = 0;       // JPEG bytes of the pictures sent
        double encodeMs = 0.0;  // The last picture's
        QSize size;             // The last picture's
    };
    Stats stats() const { return m_stats; }

Q_SIGNALS:
    void watchersChanged();
    void watcherJoined(const usdcc::peers::Peer& watcher);
    // The room closed: stop(), the source gone, or the room itself.
    void stopped(const QString& reason);

private:
    explicit Broadcast(QObject* parent);
    void frameChanged();
    void sendNext();
    void encode(const QImage& image, int quality);
    void encoded(const QByteArray& jpeg, QSize size, size_t hash, int quality, bool unchanged, double ms);
    QJsonObject frameBody() const;

    QPointer<usdcc::peers::Room> m_room;
    QPointer<usdcc::peers::Channel> m_view;
    QPointer<FrameSource> m_source;
    Options m_options;
    QTimer m_still;
    QImage m_lastImage;   // The last one grabbed, for the sharp one after it
    bool m_dirty = false; // A new picture waits to be grabbed
    bool m_encoding = false;
    bool m_sharpPending = false;
    size_t m_lastHash = 0;
    int m_lastQuality = 0;
    QByteArray m_latest;  // The last JPEG sent, for a newcomer
    QSize m_latestSize;
    Stats m_stats;
    bool m_warnedEncoder = false;
    bool m_stopped = false;
};

}  // namespace usdcc::broadcast
