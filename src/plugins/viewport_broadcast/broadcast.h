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
struct Message;
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

    // A move from the watcher who navigates (VIEWPORT_BROADCAST.md §6): "orbit" and "pan"
    // by `x`, `y` as fractions of the picture's width and height, "zoom" by `x` wheel
    // notches (positive: closer), "fit". False when it isn't done now: the host's own hand
    // is on the camera, or the source can't move.
    // Whether watchers may ask to navigate it at all: a viewport may; a Render View, which
    // follows a viewport's camera or one of the stage's, doesn't. Fixed for a broadcast.
    virtual bool canNavigate() const { return false; }
    virtual bool navigate(const QString& move, double x, double y) {
        Q_UNUSED(move);
        Q_UNUSED(x);
        Q_UNUSED(y);
        return false;
    }

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
    // Navigating (§6): direct, open to guests. A watcher sends "ask" and "cancel", and
    // while it navigates "orbit" {x, y}, "pan" {x, y}, "zoom" {steps} and "fit"; anything
    // else, or a move from someone who doesn't navigate, is refused. The host sends each
    // watcher "navigator" {id, name} (empty when nobody), and the one who asked "denied".
    static constexpr char kNavChannel[] = "org.usdcc.viewport-broadcast/nav";

    struct Options {
        QString name;  // The room's
        QHostAddress address = QHostAddress(QHostAddress::Any);
        quint16 port = 47810;  // Taken: any free one
        QString pageDirectory;  // The page: its index.html and the files beside it
        QStringList allowedOrigins;  // A tunnel's address, for its pages to connect
        // Where watchers outside reach it (phase 4a): the address of the user's own tunnel or
        // reverse proxy pointed at the port ("https://watch.example.com/", a path allowed).
        // The link uses it, and its origin may connect. Empty: the link is this machine's.
        QUrl publicAddress;
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
    // The web link for watchers: the public address when there is one, else
    // http://<address>:<port>/; and #s=<guests' secret>.
    QUrl link() const;
    // The link on this machine's own address, whether or not there is a public one.
    QUrl localLink() const;
    QList<usdcc::peers::Peer> watchers() const;
    bool isLive() const;
    // Closes the room; the page says why. stopped() follows.
    void stop(const QString& reason = {});

    // Navigating (§2.1, §6): one watcher at a time, allowed by the host.
    QString navigator() const { return m_navigator; }
    QStringList asking() const { return m_asking; }
    // Lets `watcher` navigate, taking it from whoever did. False for someone who isn't watching.
    bool allowNavigating(const QString& watcher);
    // Says no to a watcher who asked.
    void denyNavigating(const QString& watcher);
    // Ends the watcher's navigating.
    void takeBack();

    struct Stats {
        int sent = 0;           // Pictures sent, snapshots aside
        int unchanged = 0;      // Grabbed, but equal to the last one sent
        qint64 bytes = 0;       // JPEG bytes of the pictures sent
        double encodeMs = 0.0;  // The last picture's
        QSize size;             // The last picture's
        int moves = 0;          // Navigating moves done
        int movesRefused = 0;   // From someone who didn't navigate, or while the host's hand was on it
    };
    Stats stats() const { return m_stats; }

Q_SIGNALS:
    void watchersChanged();
    void watcherJoined(const usdcc::peers::Peer& watcher);
    // A watcher asks to navigate; the host answers with allowNavigating() or denyNavigating().
    void navigateAsked(const usdcc::peers::Peer& watcher);
    // Someone else navigates now, or nobody; and who's asking changed.
    void navigatorChanged();
    // The room closed: stop(), the source gone, or the room itself.
    void stopped(const QString& reason);

private:
    explicit Broadcast(QObject* parent);
    void frameChanged();
    void sendNext();
    void encode(const QImage& image, int quality);
    void encoded(const QByteArray& jpeg, QSize size, size_t hash, int quality, bool unchanged, double ms);
    QJsonObject frameBody() const;
    void onNav(const usdcc::peers::Message& message);
    void setNavigator(const QString& id);
    void tellNavigator(const QString& to = {});

    QPointer<usdcc::peers::Room> m_room;
    QPointer<usdcc::peers::Channel> m_view;
    QPointer<usdcc::peers::Channel> m_nav;
    QString m_navigator;
    QStringList m_asking;
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
