#pragma once

// The render queue's web page (docs/RENDER_NODE.md §4.5): a room of its own on the --web
// port that only guests join, serving web/ (index.html, usdcc.css). On the channel
// org.usdcc.render/queue-web, open to guests:
//   to the page:   snapshot / state {queue, accent, accentHover, actions, jobs[{..., tasks}], nodes}
//                  preview {id, frame} + PNG bytes      result {id, action, ok, reason}
//   from the page: cancel / retry / remove {id, token}
// Looking needs only the guests' link; acting needs the token of --web-token-file.

#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace usdcc::peers {
class Channel;
class Room;
}  // namespace usdcc::peers

namespace usdcc::render {
class RenderNetwork;
class RenderQueueServer;
class RenderScheduler;
}  // namespace usdcc::render

namespace usdcc::node {

inline constexpr const char* kQueueWebChannel = "org.usdcc.render/queue-web";

struct QueueWebOptions {
    QString name;  // the queue's
    quint16 port = 0;
    QHostAddress address = QHostAddress(QHostAddress::Any);
    QString pageDirectory;  // index.html, usdcc.css
    QString token;          // empty: the page can't act
    QString accent = QStringLiteral("#3d8bfd");
    int previewIntervalMs = 1000;
    int previewMaxSide = 960;
};

class QueueWeb : public QObject {
    Q_OBJECT

public:
    QueueWeb(usdcc::render::RenderQueueServer* server, usdcc::render::RenderScheduler* scheduler,
             usdcc::render::RenderNetwork* network, QueueWebOptions options, QObject* parent = nullptr);
    ~QueueWeb() override;

    // Opens the room. False, with `error`, when the port is taken.
    bool start(QString* error);
    // http://<address>:<port>/#s=<guests' secret>
    QUrl link(const QString& address = {}) const;
    usdcc::peers::Room* room() const;

    // What the page gets: the queue, its jobs with their tasks, the nodes.
    QJsonObject state() const;

    // A token for --web-token-file: 32 random hex characters.
    static QString makeToken();
    // Whether `given` is `token`, in time that doesn't depend on where they differ.
    static bool tokenMatches(const QString& token, const QString& given);

private:
    void sendSoon();
    void send();
    void onPreview(const QString& jobId, int frameIndex, const QImage& linear);
    int guests() const;

    usdcc::render::RenderQueueServer* m_server;
    usdcc::render::RenderScheduler* m_scheduler;
    QPointer<usdcc::render::RenderNetwork> m_network;
    QueueWebOptions m_options;
    std::unique_ptr<usdcc::peers::Room> m_room;
    usdcc::peers::Channel* m_channel = nullptr;
    QTimer* m_sendTimer = nullptr;
    qint64 m_lastPreviewMs = 0;
};

}  // namespace usdcc::node
