#include "queue_web.h"

#include "usdcc/core/log.h"
#include "usdcc/peers/channel.h"
#include "usdcc/peers/room.h"
#include "usdcc/usd/color_management.h"
#include "usdcc/usd/render/render_network.h"
#include "usdcc/usd/render/render_queue.h"
#include "usdcc/usd/render/render_scheduler.h"

#include <QBuffer>
#include <QColor>
#include <QDateTime>
#include <QImage>
#include <QJsonArray>
#include <QRandomGenerator>

namespace usdcc::node {

namespace {
constexpr int kSendMs = 250;
using usdcc::peers::Message;
using usdcc::peers::Peer;
using usdcc::peers::Room;
}  // namespace

QueueWeb::QueueWeb(usdcc::render::RenderQueueServer* server, usdcc::render::RenderScheduler* scheduler,
                   usdcc::render::RenderNetwork* network, QueueWebOptions options, QObject* parent)
    : QObject(parent), m_server(server), m_scheduler(scheduler), m_network(network), m_options(std::move(options)) {
    m_sendTimer = new QTimer(this);
    m_sendTimer->setSingleShot(true);
    m_sendTimer->setInterval(kSendMs);
    connect(m_sendTimer, &QTimer::timeout, this, &QueueWeb::send);
    connect(m_server, &usdcc::render::RenderQueueServer::jobsChanged, this, &QueueWeb::sendSoon);
    connect(m_server, &usdcc::render::RenderQueueServer::jobChanged, this, &QueueWeb::sendSoon);
    if (m_network) connect(m_network, &usdcc::render::RenderNetwork::nodesChanged, this, &QueueWeb::sendSoon);
    connect(m_scheduler, &usdcc::render::RenderScheduler::frameRendered, this, &QueueWeb::onPreview);
}

QueueWeb::~QueueWeb() {
    if (m_room) m_room->close(QStringLiteral("The queue is stopping"));
}

QString QueueWeb::makeToken() {
    QString token;
    for (int i = 0; i < 4; ++i) token += QStringLiteral("%1").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
    return token;
}

bool QueueWeb::tokenMatches(const QString& token, const QString& given) {
    if (token.isEmpty()) return false;
    const QByteArray a = token.toUtf8();
    const QByteArray b = given.toUtf8();
    unsigned difference = static_cast<unsigned>(a.size() ^ b.size());
    for (qsizetype i = 0; i < a.size(); ++i) difference |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(i < b.size() ? b[i] : 0);
    return difference == 0;
}

bool QueueWeb::start(QString* error) {
    usdcc::peers::HostOptions host;
    host.name = m_options.name + QStringLiteral(" (web)");
    host.type = usdcc::peers::RoomType::Broadcast;
    host.address = m_options.address;
    host.port = m_options.port;
    host.guests = true;
    host.pageDirectory = m_options.pageDirectory;
    host.profile.name = m_options.name;
    host.profile.identity = usdcc::peers::Identity::generate();
    m_room.reset(Room::host(host, nullptr, error));
    if (!m_room) return false;
    m_channel = m_room->channel(QString::fromLatin1(kQueueWebChannel), usdcc::peers::Delivery::LatestOnly);
    m_channel->setOpenToGuests(true);
    m_channel->setSnapshotProvider([this]() { return usdcc::peers::Channel::Snapshot{state(), {}}; });
    connect(m_channel, &usdcc::peers::Channel::received, this, [this](const Message& message) {
        const Peer from = m_room->peer(message.from);
        const QString action = message.type;
        if (action != u"cancel" && action != u"retry" && action != u"remove") return;
        const QString id = message.body["id"].toString();
        QString reason;
        bool ok = false;
        if (!tokenMatches(m_options.token, message.body["token"].toString())) {
            reason = m_options.token.isEmpty() ? QStringLiteral("this queue's page can't change jobs (no --web-token-file)")
                                               : QStringLiteral("that isn't the queue's token");
        } else if (action == u"cancel") {
            ok = m_server->cancel(id, nullptr, &reason);
        } else if (action == u"retry") {
            ok = m_server->retry(id, nullptr, &reason);
        } else {
            ok = m_server->remove(id, nullptr, &reason);
        }
        USDCC_LOG_INFO("usdcc.queue", "Web page ({}): {} {}: {}", from.name.toStdString(), action.toStdString(), id.toStdString(),
                       ok ? std::string("done") : reason.toStdString());
        // To that page only: a guest's Direct message.
        m_channel->send(QStringLiteral("result"), QJsonObject{{"id", id}, {"action", action}, {"ok", ok}, {"reason", reason}}, {},
                        message.from);
        sendSoon();
    });
    connect(m_room.get(), &Room::peerJoined, this, [this]() { sendSoon(); });
    return true;
}

QUrl QueueWeb::link(const QString& address) const { return m_room ? m_room->guestLink(address) : QUrl(); }

Room* QueueWeb::room() const { return m_room.get(); }

int QueueWeb::guests() const {
    if (!m_room) return 0;
    int count = 0;
    for (const Peer& peer : m_room->peers()) count += peer.kind == Peer::Kind::Guest ? 1 : 0;
    return count;
}

QJsonObject QueueWeb::state() const {
    QJsonArray jobs;
    for (const usdcc::render::QueuedJobInfo& info : m_server->jobs()) {
        QJsonObject job = info.toJson();
        job["tasks"] = m_server->tasksJson(info.id);
        jobs.append(job);
    }
    QJsonArray nodes;
    if (m_network) {
        for (const usdcc::render::RenderNode& node : m_network->nodes()) {
            QJsonArray renderers;
            for (const auto& renderer : node.renderers) renderers.append(QString::fromStdString(renderer.name));
            QJsonObject n{{"name", node.peer.name}, {"node", node.node}, {"busy", node.busy}, {"renderers", renderers}};
            if (node.current) {
                n["task"] = QJsonObject{{"title", node.current->title}, {"frame", node.current->frame}, {"fraction", node.current->fraction}};
            }
            nodes.append(n);
        }
    }
    const QColor accent(m_options.accent);
    return QJsonObject{{"queue", m_options.name},
                       {"accent", accent.name()},
                       {"accentRgb", QStringLiteral("%1, %2, %3").arg(accent.red()).arg(accent.green()).arg(accent.blue())},
                       {"accentHover", accent.lighter(115).name()},
                       {"actions", !m_options.token.isEmpty()},
                       {"root", !m_server->options().outputRoot.isEmpty()},
                       {"jobs", jobs},
                       {"nodes", nodes},
                       {"timeMs", QDateTime::currentMSecsSinceEpoch()}};
}

void QueueWeb::sendSoon() {
    if (m_room && !m_sendTimer->isActive()) m_sendTimer->start();
}

void QueueWeb::send() {
    if (!m_channel || guests() == 0) return;
    m_channel->send(QStringLiteral("state"), state());
}

void QueueWeb::onPreview(const QString& jobId, int frameIndex, const QImage& linear) {
    // Only while someone looks, and at most once a second.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_channel || guests() == 0 || now - m_lastPreviewMs < m_options.previewIntervalMs) return;
    m_lastPreviewMs = now;
    QImage image = linear;
    if (image.width() > m_options.previewMaxSide || image.height() > m_options.previewMaxSide) {
        image = image.scaled(m_options.previewMaxSide, m_options.previewMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    image = image.convertToFormat(QImage::Format_RGBA32FPx4);
    // Through the view transform, as the Render Queue panel shows it.
    if (const auto transform = usdcc::usd::ColorManagement::instance().imageTransform()) {
        for (int y = 0; y < image.height(); ++y) transform->apply(reinterpret_cast<float*>(image.scanLine(y)), static_cast<size_t>(image.width()));
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.convertToFormat(QImage::Format_RGBA8888).save(&buffer, "PNG");
    double frame = frameIndex;
    if (const usdcc::render::RenderJobPtr job = m_server->job(jobId); job && frameIndex >= 0 && frameIndex < int(job->frames.size())) {
        frame = job->frames[static_cast<size_t>(frameIndex)];
    }
    m_channel->send(QStringLiteral("preview"), QJsonObject{{"id", jobId}, {"frame", frame}}, png);
}

}  // namespace usdcc::node
