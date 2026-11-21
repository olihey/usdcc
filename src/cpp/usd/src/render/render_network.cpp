#include "usdcc/usd/render/render_network.h"

#include "usdcc/core/log.h"
#include "usdcc/peers/channel.h"
#include "usdcc/peers/room.h"
#include "usdcc/peers/state.h"
#include "usdcc/peers/transfer.h"
#include "usdcc/usd/render/render_bundle.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/pxr.h>
#include <pxr/usd/ar/resolver.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QMetaObject>
#include <QUuid>
#include <QtEndian>

#include <cstring>

PXR_NAMESPACE_USING_DIRECTIVE
using usdcc::peers::Channel;
using usdcc::peers::Delivery;
using usdcc::peers::Message;
using usdcc::peers::Peer;
using usdcc::peers::PeerId;
using usdcc::peers::PeerState;
using usdcc::peers::Room;
using usdcc::peers::Transfer;
using usdcc::peers::TransferInbox;
using usdcc::peers::TransferManifest;

namespace usdcc::render {

namespace {

RenderNetwork* s_instance = nullptr;
constexpr int kProgressIntervalMs = 250;

QString q(const std::string& s) { return QString::fromStdString(s); }
std::string s(const QString& q) { return q.toStdString(); }
std::string taskKey(const std::string& job, int index) { return job + "#" + std::to_string(index); }
QString incomingKey(const PeerId& from, const QString& job) { return from + QLatin1Char('|') + job; }

QJsonValue valueToJson(const VtValue& value) {
    QJsonObject out;
    if (value.IsHolding<bool>()) {
        out["t"] = "bool";
        out["v"] = value.UncheckedGet<bool>();
    } else if (value.IsHolding<int>()) {
        out["t"] = "int";
        out["v"] = value.UncheckedGet<int>();
    } else if (value.IsHolding<unsigned int>()) {
        out["t"] = "uint";
        out["v"] = double(value.UncheckedGet<unsigned int>());
    } else if (value.IsHolding<float>()) {
        out["t"] = "float";
        out["v"] = double(value.UncheckedGet<float>());
    } else if (value.IsHolding<double>()) {
        out["t"] = "double";
        out["v"] = value.UncheckedGet<double>();
    } else if (value.IsHolding<std::string>()) {
        out["t"] = "string";
        out["v"] = q(value.UncheckedGet<std::string>());
    } else if (value.IsHolding<TfToken>()) {
        out["t"] = "token";
        out["v"] = q(value.UncheckedGet<TfToken>().GetString());
    } else if (value.IsHolding<GfVec3f>()) {
        const GfVec3f v = value.UncheckedGet<GfVec3f>();
        out["t"] = "color";
        out["v"] = QJsonArray{v[0], v[1], v[2]};
    } else {
        return QJsonValue();
    }
    return out;
}

VtValue valueFromJson(const QJsonValue& json) {
    const QJsonObject o = json.toObject();
    const QString type = o["t"].toString();
    const QJsonValue v = o["v"];
    if (type == "bool") return VtValue(v.toBool());
    if (type == "int") return VtValue(v.toInt());
    if (type == "uint") return VtValue(static_cast<unsigned int>(v.toDouble()));
    if (type == "float") return VtValue(static_cast<float>(v.toDouble()));
    if (type == "double") return VtValue(v.toDouble());
    if (type == "string") return VtValue(s(v.toString()));
    if (type == "token") return VtValue(TfToken(s(v.toString())));
    if (type == "color") {
        const QJsonArray a = v.toArray();
        return VtValue(GfVec3f(float(a.at(0).toDouble()), float(a.at(1).toDouble()), float(a.at(2).toDouble())));
    }
    return {};
}

}  // namespace

// --- Wire formats

QJsonObject taskToJson(const RenderTask& task) {
    QJsonObject json;
    json["job"] = q(task.jobId);
    json["index"] = task.index;
    json["frame"] = task.frame;
    json["resolution"] = QJsonArray{task.resolution[0], task.resolution[1]};
    json["pixelAspect"] = double(task.pixelAspect);
    json["camera"] = q(task.camera.GetString());
    json["settings"] = q(task.renderSettings.GetString());
    json["region"] = QJsonArray{task.region.x, task.region.y, task.region.width, task.region.height};
    QJsonArray aovs;
    for (const RenderAov& aov : task.aovs) aovs.append(QJsonArray{q(aov.name), q(aov.sourceName), q(aov.dataType.GetString())});
    json["aovs"] = aovs;
    QJsonObject values;
    for (const auto& [key, value] : task.settings) {
        const QJsonValue v = valueToJson(value);
        if (!v.isNull()) values[q(key.GetString())] = v;
    }
    json["values"] = values;
    QJsonArray purposes;
    for (const TfToken& p : task.includedPurposes) purposes.append(q(p.GetString()));
    json["purposes"] = purposes;
    json["timeLimit"] = task.timeLimit;
    return json;
}

RenderTask taskFromJson(const QJsonObject& json, const UsdStageRefPtr& stage) {
    RenderTask task;
    task.jobId = s(json["job"].toString());
    task.index = json["index"].toInt();
    task.frame = json["frame"].toDouble();
    const QJsonArray res = json["resolution"].toArray();
    task.resolution = GfVec2i(res.at(0).toInt(1), res.at(1).toInt(1));
    task.pixelAspect = float(json["pixelAspect"].toDouble(1.0));
    task.stage = stage;
    const std::string camera = s(json["camera"].toString());
    if (SdfPath::IsValidPathString(camera)) task.camera = SdfPath(camera);
    const std::string settings = s(json["settings"].toString());
    if (!settings.empty() && SdfPath::IsValidPathString(settings)) task.renderSettings = SdfPath(settings);
    const QJsonArray region = json["region"].toArray();
    task.region = {region.at(0).toInt(), region.at(1).toInt(), region.at(2).toInt(), region.at(3).toInt()};
    for (const QJsonValue& a : json["aovs"].toArray()) {
        const QJsonArray aov = a.toArray();
        task.aovs.push_back({s(aov.at(0).toString()), s(aov.at(1).toString()), TfToken(s(aov.at(2).toString()))});
    }
    const QJsonObject values = json["values"].toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        const VtValue v = valueFromJson(it.value());
        if (!v.IsEmpty()) task.settings[TfToken(s(it.key()))] = v;
    }
    for (const QJsonValue& p : json["purposes"].toArray()) task.includedPurposes.emplace_back(s(p.toString()));
    task.timeLimit = json["timeLimit"].toDouble();
    return task;
}

QByteArray packImage(const FloatImage& image) {
    QByteArray raw(8 + int(image.rgba.size() * sizeof(float)), Qt::Uninitialized);
    qToLittleEndian<qint32>(image.width, raw.data());
    qToLittleEndian<qint32>(image.height, raw.data() + 4);
    std::memcpy(raw.data() + 8, image.rgba.data(), image.rgba.size() * sizeof(float));
    return qCompress(raw, 6);
}

std::optional<FloatImage> unpackImage(const QByteArray& bytes) {
    const QByteArray raw = qUncompress(bytes);
    if (raw.size() < 8) return std::nullopt;
    const int width = qFromLittleEndian<qint32>(raw.constData());
    const int height = qFromLittleEndian<qint32>(raw.constData() + 4);
    if (width <= 0 || height <= 0 || qint64(raw.size()) != 8 + qint64(width) * height * 4 * qint64(sizeof(float))) return std::nullopt;
    FloatImage image(width, height);
    std::memcpy(image.rgba.data(), raw.constData() + 8, image.rgba.size() * sizeof(float));
    return image;
}

bool RenderNode::canRender(const std::string& rendererId, bool tiles) const {
    if (!node) return false;
    for (const Renderer& r : renderers) {
        if (r.id == rendererId) return !tiles || r.tiles;
    }
    return false;
}

RenderNode RenderNode::fromJson(Room* room, const Peer& peer, const QJsonObject& json) {
    RenderNode node;
    node.room = room;
    node.peer = peer;
    node.node = json["node"].toBool();
    node.busy = json["busy"].toBool();
    for (const QJsonValue& r : json["renderers"].toArray()) {
        const QJsonObject o = r.toObject();
        node.renderers.push_back({s(o["id"].toString()), s(o["name"].toString()), o["tiles"].toBool()});
    }
    for (const QJsonValue& scheme : json["schemes"].toArray()) node.schemes.append(scheme.toString());
    return node;
}

// --- State

struct RenderNetwork::RoomLink {
    QPointer<Room> room;
    Channel* jobs = nullptr;
    PeerState* state = nullptr;
    TransferInbox* inbox = nullptr;
    std::map<PeerId, std::shared_ptr<PeerWorker>> workers;
    QObject context;  // its connections go with it
};

struct RenderNetwork::Outgoing {
    enum class Status { Offered, Accepted, Sending, Ready, Declined, Failed } status = Status::Offered;
    std::string reason;
    QPointer<Room> room;
    QPointer<Transfer> transfer;
};

struct RenderNetwork::RemoteTask {
    std::string job;
    int index = 0;
    QPointer<Room> room;
    QString peer;
    QJsonObject message;  // the task, to send once the job is ready there
    TaskCallbacks callbacks;
    bool sent = false;
    bool ended = false;
    QString resultHash;
};

struct RenderNetwork::Incoming {
    QPointer<Room> room;
    Peer from;
    QString job;
    std::string renderer;
    enum class Status { Asking, Accepted, Ready, Declined } status = Status::Asking;
    QString bundleHash;
    UsdStageRefPtr stage;
};

struct RenderNetwork::NodeTask {
    QString incoming;  // m_incoming's key
    QJsonObject message;
    unsigned serial = 0;
    std::unique_ptr<RenderTaskHandle> handle;
    QElapsedTimer lastProgress;
};

class RenderNetwork::RemoteHandle : public RenderTaskHandle {
public:
    RemoteHandle(RenderNetwork* network, std::string key) : m_network(network), m_key(std::move(key)) {}
    void cancel() override {
        if (m_network) m_network->cancelRemote(m_key);
    }

private:
    QPointer<RenderNetwork> m_network;
    std::string m_key;
};

// --- Setup

RenderNetwork::RenderNetwork(RenderNetworkOptions options, QObject* parent) : QObject(parent), m_options(std::move(options)) {
    if (!m_options.scheduler) m_options.scheduler = &RenderScheduler::instance();
    if (m_options.folder.isEmpty()) m_options.folder = QDir::temp().filePath(QStringLiteral("usdcc-render-network"));
    QDir().mkpath(m_options.folder);
    connect(m_options.scheduler, &RenderScheduler::jobEnded, this, &RenderNetwork::jobEnded);
}

RenderNetwork::~RenderNetwork() {
    if (s_instance == this) s_instance = nullptr;
    if (m_nodeRunning && m_nodeRunning->handle) m_nodeRunning->handle->cancel();
    std::vector<Room*> rooms;
    for (const auto& [room, link] : m_rooms) rooms.push_back(room);
    for (Room* room : rooms) removeRoom(room);
}

RenderNetwork* RenderNetwork::instance() { return s_instance; }
void RenderNetwork::setInstance(RenderNetwork* network) { s_instance = network; }

RenderNetwork::RoomLink* RenderNetwork::link(Room* room) const {
    const auto found = m_rooms.find(room);
    return found == m_rooms.end() ? nullptr : found->second.get();
}

void RenderNetwork::addRoom(Room* room) {
    if (!room || m_rooms.count(room)) return;
    auto link = std::make_unique<RoomLink>();
    RoomLink* l = link.get();
    l->room = room;
    l->jobs = room->channel(QString::fromLatin1(kJobsChannel), Delivery::Direct);
    l->state = room->peerState(QString::fromLatin1(kNodeState));
    TransferInbox::Options inbox;
    inbox.directory = QDir(m_options.folder).filePath(QStringLiteral("inbox"));
    inbox.accept = [this, l](const Peer& from, const TransferManifest&) { return acceptsTransfer(*l, from); };
    l->inbox = new TransferInbox(room, QString::fromLatin1(kFilesChannel), inbox, &l->context);
    m_rooms[room] = std::move(link);

    connect(l->jobs, &Channel::received, &l->context, [this, l](const Message& message) { onMessage(*l, message); });
    connect(l->state, &PeerState::changed, &l->context, [this, l](const Peer& peer, const QJsonValue&) { onStateChanged(*l, peer); });
    connect(room, &Room::peerLeft, &l->context, [this, l](const Peer& peer, const QString&) { onPeerLeft(*l, peer); });
    connect(room, &Room::opened, &l->context, [this]() { advertise(); });
    connect(room, &Room::closed, &l->context, [this, room](const QString&) {
        QMetaObject::invokeMethod(this, [this, room]() { removeRoom(room); }, Qt::QueuedConnection);
    });
    connect(room, &QObject::destroyed, &l->context, [this, room]() { removeRoom(room); });
    connect(l->inbox, &TransferInbox::received, &l->context, [this, l](Transfer* transfer) { onBundleArrived(*l, transfer); });
    advertise();
    syncWorkers(*l);
}

void RenderNetwork::removeRoom(Room* room) {
    const auto found = m_rooms.find(room);
    if (found == m_rooms.end()) return;
    std::unique_ptr<RoomLink> link = std::move(found->second);
    m_rooms.erase(found);
    for (auto& [id, worker] : link->workers) m_options.scheduler->removeWorker(worker);
    // Tasks rendering here for that room's peers stop.
    for (auto it = m_nodeQueue.begin(); it != m_nodeQueue.end();) {
        const auto incoming = m_incoming.find((*it)->incoming);
        it = incoming == m_incoming.end() || incoming->second->room == room ? m_nodeQueue.erase(it) : it + 1;
    }
    if (m_nodeRunning) {
        const auto incoming = m_incoming.find(m_nodeRunning->incoming);
        if (incoming == m_incoming.end() || incoming->second->room == room || !incoming->second->room) {
            if (m_nodeRunning->handle) m_nodeRunning->handle->cancel();
            m_nodeRunning.reset();
        }
    }
    for (auto it = m_incoming.begin(); it != m_incoming.end();) {
        it = it->second->room == room || !it->second->room ? m_incoming.erase(it) : std::next(it);
    }
    Q_EMIT nodesChanged();
    Q_EMIT remoteTasksChanged();
}

void RenderNetwork::advertise() {
    if (m_advertising) return;
    m_advertising = true;
    QJsonArray renderers;
    for (const auto& renderer : FinalRenderers::instance().all()) {
        if (!renderer->setupProblem().empty()) continue;
        renderers.append(QJsonObject{{"id", q(renderer->id())}, {"name", q(renderer->name())}, {"tiles", renderer->capabilities().tiles}});
    }
    QJsonArray schemes;
    for (const std::string& scheme : ArGetRegisteredURISchemes()) schemes.append(q(scheme));
    const QJsonObject value{{"renderers", renderers},
                            {"node", m_options.isNode ? m_options.isNode() : false},
                            {"busy", m_nodeRunning != nullptr},
                            {"schemes", schemes}};
    for (const auto& [room, link] : m_rooms) {
        if (link->room && link->room->state() == Room::State::Open) link->state->set(value);
    }
    m_advertising = false;
}

std::vector<RenderNode> RenderNetwork::nodes() const {
    std::vector<RenderNode> result;
    for (const auto& [room, link] : m_rooms) {
        if (!link->room) continue;
        const QHash<PeerId, QJsonValue> values = link->state->values();
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (it.key() == link->room->me().id || !it.value().isObject()) continue;
            const Peer peer = link->room->peer(it.key());
            if (!peer.isValid()) continue;
            result.push_back(RenderNode::fromJson(link->room, peer, it.value().toObject()));
        }
    }
    return result;
}

std::vector<RenderNode> RenderNetwork::nodesFor(const std::string& rendererId, bool tiles) const {
    std::vector<RenderNode> result;
    for (RenderNode& node : nodes()) {
        if (node.canRender(rendererId, tiles)) result.push_back(std::move(node));
    }
    return result;
}

int RenderNetwork::remoteTasks() const { return int(m_nodeQueue.size()) + (m_nodeRunning ? 1 : 0); }

void RenderNetwork::syncWorkers(RoomLink& link) {
    if (!link.room) return;
    const QHash<PeerId, QJsonValue> values = link.state->values();
    // Peers that are render nodes are workers; others aren't.
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (it.key() == link.room->me().id) continue;
        const Peer peer = link.room->peer(it.key());
        const bool node = peer.isValid() && it.value().toObject()["node"].toBool();
        const bool has = link.workers.count(it.key()) > 0;
        if (node && !has) {
            auto worker = std::make_shared<PeerWorker>(this, link.room, peer);
            link.workers[it.key()] = worker;
            m_options.scheduler->addWorker(worker);
            USDCC_LOG_INFO("usdcc.render", "{} renders for this instance", s(peer.name));
        } else if (!node && has) {
            m_options.scheduler->removeWorker(link.workers[it.key()]);
            link.workers.erase(it.key());
        }
    }
    for (auto it = link.workers.begin(); it != link.workers.end();) {
        if (!values.contains(it->first)) {
            m_options.scheduler->removeWorker(it->second);
            it = link.workers.erase(it);
        } else {
            ++it;
        }
    }
}

void RenderNetwork::onStateChanged(RoomLink& link, const Peer&) {
    syncWorkers(link);
    Q_EMIT nodesChanged();
}

void RenderNetwork::onPeerLeft(RoomLink& link, const Peer& peer) {
    const auto worker = link.workers.find(peer.id);
    if (worker != link.workers.end()) {
        std::shared_ptr<PeerWorker> gone = worker->second;
        link.workers.erase(worker);
        m_options.scheduler->removeWorker(gone);  // its running task goes back to the queue
    }
    m_outgoing.erase(peer.id);
    for (auto it = m_remote.begin(); it != m_remote.end();) {
        it = it->second->peer == peer.id ? m_remote.erase(it) : std::next(it);
    }
    // What this instance rendered for it stops.
    for (auto it = m_nodeQueue.begin(); it != m_nodeQueue.end();) {
        it = (*it)->incoming.startsWith(peer.id + QLatin1Char('|')) ? m_nodeQueue.erase(it) : it + 1;
    }
    if (m_nodeRunning && m_nodeRunning->incoming.startsWith(peer.id + QLatin1Char('|'))) {
        if (m_nodeRunning->handle) m_nodeRunning->handle->cancel();
        m_nodeRunning.reset();
        advertise();
        QMetaObject::invokeMethod(this, [this]() { pump(); }, Qt::QueuedConnection);
    }
    for (auto it = m_incoming.begin(); it != m_incoming.end();) {
        it = it->second->from.id == peer.id ? m_incoming.erase(it) : std::next(it);
    }
    Q_EMIT nodesChanged();
    Q_EMIT remoteTasksChanged();
}

bool RenderNetwork::acceptsTransfer(RoomLink& link, const Peer& from) const {
    // A node takes the bundles of jobs it accepted; a submitter, results from its workers.
    for (const auto& [key, incoming] : m_incoming) {
        if (incoming->room == link.room && incoming->from.id == from.id && incoming->status != Incoming::Status::Declined &&
            incoming->status != Incoming::Status::Asking) {
            return true;
        }
    }
    return m_outgoing.count(from.id) > 0;
}

void RenderNetwork::onMessage(RoomLink& link, const Message& message) {
    if (!link.room) return;
    const Peer from = link.room->peer(message.from);
    if (!from.isValid() || from.id == link.room->me().id) return;
    const QString& type = message.type;
    // To a node.
    if (type == "offer") return onOffer(link, from, message.body);
    if (type == "task") return onTask(link, from, message.body);
    if (type == "cancel") return onCancel(link, from, message.body);
    if (type == "bundle") {
        const auto found = m_incoming.find(incomingKey(from.id, message.body["job"].toString()));
        if (found == m_incoming.end()) return;
        found->second->bundleHash = message.body["hash"].toString();
        if (link.inbox->hasBundle(found->second->bundleHash)) loadBundle(link, *found->second);
        return;
    }
    if (type == "end") {
        const QString key = incomingKey(from.id, message.body["job"].toString());
        m_incoming.erase(key);
        return;
    }
    // To a submitter.
    if (type == "accept") return onAccepted(link, from.id, message.body);
    if (type == "decline") return onDeclined(link, from.id, message.body);
    if (type == "result") return onResult(link, from.id, message.body);
    if (type == "progress" || type == "failed") {
        const auto found = m_remote.find(taskKey(s(message.body["job"].toString()), message.body["index"].toInt()));
        if (found == m_remote.end() || found->second->ended || found->second->peer != from.id) return;
        const std::shared_ptr<RemoteTask> task = found->second;
        if (type == "progress") {
            if (task->callbacks.progress) task->callbacks.progress(message.body["fraction"].toDouble(-1), s(message.body["state"].toString()));
        } else {
            task->ended = true;
            m_remote.erase(found);
            if (task->callbacks.failed) task->callbacks.failed(s(from.name) + ": " + s(message.body["error"].toString()));
        }
    }
}

// --- Submitting

bool RenderNetwork::canRun(Room* room, const QString& peer, const RenderJob& job) const {
    if (!job.spec.options.usesPeers()) return false;
    RoomLink* l = link(room);
    if (!l || !l->room) return false;
    const QJsonObject json = l->state->value(peer).toObject();
    const RenderNode node = RenderNode::fromJson(room, l->room->peer(peer), json);
    if (!node.canRender(job.rendererId, job.tilesPerFrame() > 1)) return false;
    const auto byPeer = m_outgoing.find(peer);
    if (byPeer != m_outgoing.end()) {
        const auto out = byPeer->second.find(job.id);
        if (out != byPeer->second.end() &&
            (out->second->status == Outgoing::Status::Declined || out->second->status == Outgoing::Status::Failed)) {
            return false;
        }
    }
    return true;
}

std::unique_ptr<RenderTaskHandle> RenderNetwork::startRemote(Room* room, const QString& peer, const RenderJob& job,
                                                             const JobTask& jobTask, TaskCallbacks callbacks) {
    RoomLink* l = link(room);
    if (!l || !l->room) {
        if (callbacks.declined) callbacks.declined("the room has closed");
        return nullptr;
    }
    auto& byJob = m_outgoing[peer];
    auto& out = byJob[job.id];
    if (!out) {
        out = std::make_unique<Outgoing>();
        out->room = room;
        m_jobStages[job.id] = jobTask.task.stage;
        QJsonObject offer{{"job", q(job.id)}, {"renderer", q(job.rendererId)}, {"title", q(job.title)},
                          {"frames", int(job.frames.size())}};
        if (job.spec.options.delivery == "shared" && job.shared.whyNot.empty()) offer["shared"] = job.shared.toJson();
        l->jobs->send(QStringLiteral("offer"), offer, {}, peer);
    }
    auto task = std::make_shared<RemoteTask>();
    task->job = job.id;
    task->index = jobTask.task.index;
    task->room = room;
    task->peer = peer;
    task->message = taskToJson(jobTask.task);
    task->callbacks = std::move(callbacks);
    const std::string key = taskKey(job.id, task->index);
    m_remote[key] = task;
    if (out->status == Outgoing::Status::Ready) {
        task->sent = true;
        l->jobs->send(QStringLiteral("task"), task->message, {}, peer);
    } else if (out->status == Outgoing::Status::Declined || out->status == Outgoing::Status::Failed) {
        m_remote.erase(key);
        if (task->callbacks.declined) task->callbacks.declined(out->reason);
        return nullptr;
    }
    if (task->callbacks.progress) task->callbacks.progress(-1.0, task->sent ? "Sent" : "Waiting for " + s(l->room->peer(peer).name));
    return std::make_unique<RemoteHandle>(this, key);
}

void RenderNetwork::cancelRemote(const std::string& key) {
    const auto found = m_remote.find(key);
    if (found == m_remote.end()) return;
    const std::shared_ptr<RemoteTask> task = found->second;
    m_remote.erase(found);
    task->ended = true;
    if (RoomLink* l = link(task->room); l && l->room && task->sent) {
        l->jobs->send(QStringLiteral("cancel"), QJsonObject{{"job", q(task->job)}, {"index", task->index}}, {}, task->peer);
    }
}

void RenderNetwork::sendWaiting(RoomLink& link, const QString& peer, const std::string& jobId) {
    for (auto& [key, task] : m_remote) {
        if (task->peer != peer || task->job != jobId || task->sent || task->ended) continue;
        task->sent = true;
        link.jobs->send(QStringLiteral("task"), task->message, {}, peer);
        if (task->callbacks.progress) task->callbacks.progress(-1.0, "Sent");
    }
}

void RenderNetwork::failWaiting(const QString& peer, const std::string& jobId, const std::string& error, bool declined) {
    std::vector<std::shared_ptr<RemoteTask>> tasks;
    for (auto it = m_remote.begin(); it != m_remote.end();) {
        if (it->second->peer == peer && it->second->job == jobId) {
            tasks.push_back(it->second);
            it = m_remote.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& task : tasks) {
        task->ended = true;
        if (declined && task->callbacks.declined) {
            task->callbacks.declined(error);
        } else if (task->callbacks.failed) {
            task->callbacks.failed(error);
        }
    }
}

void RenderNetwork::onAccepted(RoomLink& link, const QString& peer, const QJsonObject& body) {
    const std::string jobId = s(body["job"].toString());
    const auto byPeer = m_outgoing.find(peer);
    if (byPeer == m_outgoing.end() || !byPeer->second.count(jobId)) return;
    Outgoing& out = *byPeer->second[jobId];
    if (body["delivery"].toString() == "shared") {
        out.status = Outgoing::Status::Ready;
        sendWaiting(link, peer, jobId);
        return;
    }
    if (body.contains("problems")) {
        USDCC_LOG_INFO("usdcc.render", "{} can't use the shared stage ({}): sending it the scene", s(link.room->peer(peer).name),
                       s(body["problems"].toVariant().toStringList().join(QStringLiteral("; "))));
    }
    // Packaged: the bundle, made once for the job.
    out.status = Outgoing::Status::Sending;
    QString& folder = m_bundles[jobId];
    QString error;
    if (folder.isEmpty()) {
        folder = QDir(m_options.folder).filePath(QStringLiteral("bundles/") + q(jobId) + QLatin1Char('-') +
                                                 QUuid::createUuid().toString(QUuid::Id128).left(8));
        const auto stage = m_jobStages.find(jobId);
        if (stage == m_jobStages.end() || !writeSceneBundle(stage->second, folder, &error)) {
            if (error.isEmpty()) error = QStringLiteral("the job's stage is gone");
            QDir(folder).removeRecursively();
            m_bundles.erase(jobId);
            out.status = Outgoing::Status::Failed;
            out.reason = "couldn't package the scene: " + s(error);
            failWaiting(peer, jobId, out.reason, true);
            return;
        }
    }
    const std::optional<TransferManifest> manifest = TransferManifest::fromDirectory(folder, &error);
    if (!manifest) {
        out.status = Outgoing::Status::Failed;
        out.reason = "couldn't list the scene's files: " + s(error);
        failWaiting(peer, jobId, out.reason, true);
        return;
    }
    link.jobs->send(QStringLiteral("bundle"), QJsonObject{{"job", q(jobId)}, {"hash", manifest->hash()}}, {}, peer);
    Transfer* transfer = Transfer::send(link.room, QString::fromLatin1(kFilesChannel), peer, *manifest, {}, this, &error);
    if (!transfer) {
        out.status = Outgoing::Status::Failed;
        out.reason = "couldn't send the scene: " + s(error);
        failWaiting(peer, jobId, out.reason, true);
        return;
    }
    out.transfer = transfer;
    QPointer<Room> room = link.room;
    connect(transfer, &Transfer::progress, this, [this, peer, jobId](qint64 done, qint64 total) {
        for (auto& [key, task] : m_remote) {
            if (task->peer == peer && task->job == jobId && !task->sent && task->callbacks.progress) {
                task->callbacks.progress(-1.0, "Sending the scene: " + std::to_string(total ? done * 100 / total : 0) + "%");
            }
        }
    });
    connect(transfer, &Transfer::finished, this, [this, room, peer, jobId]() {
        const auto p = m_outgoing.find(peer);
        if (p == m_outgoing.end() || !p->second.count(jobId)) return;
        p->second[jobId]->status = Outgoing::Status::Ready;
        if (RoomLink* l = this->link(room)) sendWaiting(*l, peer, jobId);
    });
    connect(transfer, &Transfer::failed, this, [this, peer, jobId](const QString& reason) {
        const auto p = m_outgoing.find(peer);
        if (p == m_outgoing.end() || !p->second.count(jobId)) return;
        Outgoing& o = *p->second[jobId];
        o.status = Outgoing::Status::Failed;
        o.reason = "sending the scene failed: " + s(reason);
        failWaiting(peer, jobId, o.reason, true);
    });
}

void RenderNetwork::onDeclined(RoomLink& link, const QString& peer, const QJsonObject& body) {
    const std::string jobId = s(body["job"].toString());
    const auto byPeer = m_outgoing.find(peer);
    if (byPeer == m_outgoing.end() || !byPeer->second.count(jobId)) return;
    Outgoing& out = *byPeer->second[jobId];
    out.status = Outgoing::Status::Declined;
    out.reason = s(body["reason"].toString());
    USDCC_LOG_INFO("usdcc.render", "{} declined {}: {}", s(link.room->peer(peer).name), jobId, out.reason);
    failWaiting(peer, jobId, out.reason, true);
}

void RenderNetwork::onResult(RoomLink& link, const QString& peer, const QJsonObject& body) {
    const auto found = m_remote.find(taskKey(s(body["job"].toString()), body["index"].toInt()));
    if (found == m_remote.end() || found->second->peer != peer) return;
    found->second->resultHash = body["hash"].toString();
    if (found->second->callbacks.progress) found->second->callbacks.progress(1.0, "Receiving the images");
    if (link.inbox->hasBundle(found->second->resultHash) || m_arrivedResults.contains(found->second->resultHash)) {
        deliverResult(link, found->second);
    }
}

void RenderNetwork::onBundleArrived(RoomLink& link, Transfer* transfer) {
    const QString hash = transfer->bundleHash();
    // A node: a job's scene.
    for (auto& [key, incoming] : m_incoming) {
        if (incoming->room == link.room && incoming->bundleHash == hash && incoming->status == Incoming::Status::Accepted) {
            loadBundle(link, *incoming);
        }
    }
    // A submitter: a task's images.
    for (auto& [key, task] : m_remote) {
        if (task->resultHash == hash && !task->ended) {
            const std::shared_ptr<RemoteTask> found = task;  // deliverResult() takes it off the map
            deliverResult(link, found);
            return;
        }
    }
    m_arrivedResults.insert(hash);
}

void RenderNetwork::deliverResult(RoomLink& link, std::shared_ptr<RemoteTask> task) {
    const QString folder = link.inbox->bundleDirectory(task->resultHash);
    std::map<std::string, FloatImage> images;
    for (const QFileInfo& file : QDir(folder).entryInfoList({QStringLiteral("*.rawz")}, QDir::Files)) {
        QFile in(file.filePath());
        if (!in.open(QIODevice::ReadOnly)) continue;
        if (std::optional<FloatImage> image = unpackImage(in.readAll())) images[s(file.completeBaseName())] = std::move(*image);
    }
    m_arrivedResults.remove(task->resultHash);
    m_remote.erase(taskKey(task->job, task->index));
    task->ended = true;
    QDir(folder).removeRecursively();  // received; the inbox needn't keep it
    if (images.empty()) {
        if (task->callbacks.failed) task->callbacks.failed("the images from " + s(link.room->peer(task->peer).name) + " couldn't be read");
    } else if (task->callbacks.finished) {
        task->callbacks.finished(std::move(images));
    }
}

void RenderNetwork::jobEnded(const QString& jobId) {
    const std::string id = s(jobId);
    // Tell the nodes it went to, so they let go of its scene.
    for (auto& [peer, byJob] : m_outgoing) {
        const auto out = byJob.find(id);
        if (out == byJob.end()) continue;
        if (RoomLink* l = link(out->second->room); l && l->room && out->second->status != Outgoing::Status::Declined) {
            l->jobs->send(QStringLiteral("end"), QJsonObject{{"job", jobId}}, {}, peer);
        }
        if (out->second->transfer) out->second->transfer->cancel(QStringLiteral("the job ended"));
        byJob.erase(out);
    }
    m_jobStages.erase(id);
    const auto bundle = m_bundles.find(id);
    if (bundle != m_bundles.end()) {
        QDir(bundle->second).removeRecursively();
        m_bundles.erase(bundle);
    }
}

// --- Rendering for peers

void RenderNetwork::onOffer(RoomLink& link, const Peer& from, const QJsonObject& body) {
    const QString job = body["job"].toString();
    const std::string renderer = s(body["renderer"].toString());
    const auto decline = [&](const QString& reason) {
        link.jobs->send(QStringLiteral("decline"), QJsonObject{{"job", job}, {"reason", reason}}, {}, from.id);
    };
    if (!m_options.isNode || !m_options.isNode()) return decline(QStringLiteral("it isn't a render node"));
    const std::shared_ptr<FinalRenderer> found = FinalRenderers::instance().find(renderer);
    if (!found || !found->setupProblem().empty()) return decline(QStringLiteral("it hasn't got %1").arg(q(renderer)));
    auto incoming = std::make_unique<Incoming>();
    incoming->room = link.room;
    incoming->from = from;
    incoming->job = job;
    incoming->renderer = renderer;
    const QString key = incomingKey(from.id, job);
    m_incoming[key] = std::move(incoming);
    if (m_options.trusted && m_options.trusted(from)) return answerOffer(link, from, body, true);
    if (!m_options.ask) return decline(QStringLiteral("it renders for trusted peers only"));
    const QString what = QStringLiteral("%1 asks this instance to render %2 with %3 (%4 frame(s)).")
                             .arg(from.name, body["title"].toString(), q(found->name()))
                             .arg(body["frames"].toInt());
    QPointer<Room> room = link.room;
    m_options.ask(from, what, [this, room, from, body](bool take) {
        if (RoomLink* l = this->link(room)) answerOffer(*l, from, body, take);
    });
}

void RenderNetwork::answerOffer(RoomLink& link, const Peer& from, const QJsonObject& offer, bool take) {
    const QString job = offer["job"].toString();
    const auto found = m_incoming.find(incomingKey(from.id, job));
    if (found == m_incoming.end()) return;
    Incoming& incoming = *found->second;
    if (!take) {
        incoming.status = Incoming::Status::Declined;
        link.jobs->send(QStringLiteral("decline"), QJsonObject{{"job", job}, {"reason", QStringLiteral("its user declined")}}, {}, from.id);
        m_incoming.erase(found);
        return;
    }
    // Shared, if every asset is here as it is there; else the bundle.
    QJsonObject accept{{"job", job}, {"delivery", "packaged"}};
    if (offer.contains("shared")) {
        const SharedScene shared = SharedScene::fromJson(offer["shared"].toObject());
        QStringList problems = sharedSceneProblems(shared);
        if (problems.isEmpty()) {
            QString error;
            incoming.stage = openSharedScene(shared, &error);
            if (!incoming.stage) problems.append(error);
        }
        if (problems.isEmpty()) {
            accept["delivery"] = "shared";
            incoming.status = Incoming::Status::Ready;
        } else {
            accept["problems"] = QJsonArray::fromStringList(problems);
        }
    }
    if (incoming.status != Incoming::Status::Ready) incoming.status = Incoming::Status::Accepted;
    USDCC_LOG_INFO("usdcc.render", "Rendering {} for {} ({})", s(job), s(from.name), s(accept["delivery"].toString()));
    link.jobs->send(QStringLiteral("accept"), accept, {}, from.id);
    if (incoming.status == Incoming::Status::Ready) pump();
}

void RenderNetwork::loadBundle(RoomLink& link, Incoming& incoming) {
    const QString file = QDir(link.inbox->bundleDirectory(incoming.bundleHash)).filePath(QString::fromLatin1(kBundleScene));
    incoming.stage = UsdStage::Open(file.toStdString());
    if (!incoming.stage) {
        link.jobs->send(QStringLiteral("decline"),
                        QJsonObject{{"job", incoming.job}, {"reason", QStringLiteral("the scene it was sent doesn't open")}}, {},
                        incoming.from.id);
        incoming.status = Incoming::Status::Declined;
        return;
    }
    incoming.status = Incoming::Status::Ready;
    pump();
}

void RenderNetwork::onTask(RoomLink&, const Peer& from, const QJsonObject& body) {
    const QString key = incomingKey(from.id, body["job"].toString());
    if (!m_incoming.count(key)) return;
    auto task = std::make_unique<NodeTask>();
    task->incoming = key;
    task->message = body;
    m_nodeQueue.push_back(std::move(task));
    Q_EMIT remoteTasksChanged();
    pump();
}

void RenderNetwork::onCancel(RoomLink&, const Peer& from, const QJsonObject& body) {
    const QString key = incomingKey(from.id, body["job"].toString());
    const bool all = !body.contains("index");
    const int index = body["index"].toInt();
    const auto matches = [&](const NodeTask& t) { return t.incoming == key && (all || t.message["index"].toInt() == index); };
    for (auto it = m_nodeQueue.begin(); it != m_nodeQueue.end();) it = matches(**it) ? m_nodeQueue.erase(it) : it + 1;
    if (m_nodeRunning && matches(*m_nodeRunning)) {
        if (m_nodeRunning->handle) m_nodeRunning->handle->cancel();
        m_nodeRunning.reset();
        advertise();
        QMetaObject::invokeMethod(this, [this]() { pump(); }, Qt::QueuedConnection);
    }
    Q_EMIT remoteTasksChanged();
}

void RenderNetwork::pump() {
    if (m_nodeRunning) return;
    // The first task whose job's scene is here.
    auto ready = m_nodeQueue.end();
    for (auto it = m_nodeQueue.begin(); it != m_nodeQueue.end(); ++it) {
        const auto incoming = m_incoming.find((*it)->incoming);
        if (incoming != m_incoming.end() && incoming->second->status == Incoming::Status::Ready) {
            ready = it;
            break;
        }
    }
    if (ready == m_nodeQueue.end()) return;
    m_nodeRunning = std::move(*ready);
    m_nodeQueue.erase(ready);
    Incoming& incoming = *m_incoming[m_nodeRunning->incoming];
    const RenderTask task = taskFromJson(m_nodeRunning->message, incoming.stage);
    const std::shared_ptr<FinalRenderer> renderer = FinalRenderers::instance().find(incoming.renderer);
    const unsigned serial = ++m_nodeSerial;
    m_nodeRunning->serial = serial;
    m_nodeRunning->lastProgress.start();
    advertise();
    Q_EMIT remoteTasksChanged();
    QPointer<RenderNetwork> self(this);
    TaskCallbacks callbacks;
    callbacks.progress = [self, serial](double fraction, const std::string& state) {
        QMetaObject::invokeMethod(self, [self, serial, fraction, state]() {
            if (!self || !self->m_nodeRunning || self->m_nodeRunning->serial != serial) return;
            NodeTask& running = *self->m_nodeRunning;
            if (running.lastProgress.elapsed() < kProgressIntervalMs) return;
            running.lastProgress.restart();
            const auto incoming = self->m_incoming.find(running.incoming);
            if (incoming == self->m_incoming.end()) return;
            if (RoomLink* l = self->link(incoming->second->room)) {
                l->jobs->send(QStringLiteral("progress"),
                              QJsonObject{{"job", running.message["job"]}, {"index", running.message["index"]},
                                          {"fraction", fraction}, {"state", q(state)}},
                              {}, incoming->second->from.id);
            }
        }, Qt::QueuedConnection);
    };
    callbacks.finished = [self, serial](std::map<std::string, FloatImage> images) {
        auto shared = std::make_shared<std::map<std::string, FloatImage>>(std::move(images));
        QMetaObject::invokeMethod(self, [self, serial, shared]() {
            if (self && self->m_nodeRunning && self->m_nodeRunning->serial == serial) self->finishNodeTask(*shared, {});
        }, Qt::QueuedConnection);
    };
    callbacks.failed = [self, serial](const std::string& error) {
        QMetaObject::invokeMethod(self, [self, serial, error]() {
            if (self && self->m_nodeRunning && self->m_nodeRunning->serial == serial) self->finishNodeTask({}, error);
        }, Qt::QueuedConnection);
    };
    callbacks.declined = callbacks.failed;
    if (!renderer) {
        callbacks.failed("it hasn't got " + incoming.renderer + " any more");
        return;
    }
    m_nodeRunning->handle = renderer->start(task, std::move(callbacks));
}

void RenderNetwork::finishNodeTask(const std::map<std::string, FloatImage>& images, const std::string& error) {
    std::unique_ptr<NodeTask> done = std::move(m_nodeRunning);
    const auto incoming = m_incoming.find(done->incoming);
    RoomLink* l = incoming == m_incoming.end() ? nullptr : link(incoming->second->room);
    if (l && l->room) {
        const PeerId to = incoming->second->from.id;
        const QJsonValue job = done->message["job"];
        const QJsonValue index = done->message["index"];
        QString why = q(error);
        if (why.isEmpty()) {
            // The images, as a bundle of their own, back to the submitter.
            const QString folder = QDir(m_options.folder).filePath(QStringLiteral("results/") + QUuid::createUuid().toString(QUuid::Id128));
            QDir().mkpath(folder);
            for (const auto& [name, image] : images) {
                QFile out(QDir(folder).filePath(q(name) + QStringLiteral(".rawz")));
                if (!out.open(QIODevice::WriteOnly) || out.write(packImage(image)) < 0) why = QStringLiteral("couldn't write the images");
            }
            QString listError;
            const std::optional<TransferManifest> manifest = why.isEmpty() ? TransferManifest::fromDirectory(folder, &listError) : std::nullopt;
            Transfer* transfer = manifest ? Transfer::send(l->room, QString::fromLatin1(kFilesChannel), to, *manifest, {}, this, &listError) : nullptr;
            if (transfer) {
                l->jobs->send(QStringLiteral("result"), QJsonObject{{"job", job}, {"index", index}, {"hash", manifest->hash()}}, {}, to);
                const auto removeFolder = [folder]() { QDir(folder).removeRecursively(); };
                connect(transfer, &Transfer::finished, this, removeFolder);
                connect(transfer, &Transfer::failed, this, removeFolder);
            } else {
                QDir(folder).removeRecursively();
                if (why.isEmpty()) why = QStringLiteral("couldn't send the images: %1").arg(listError);
            }
        }
        if (!why.isEmpty()) {
            l->jobs->send(QStringLiteral("failed"), QJsonObject{{"job", job}, {"index", index}, {"error", why}}, {}, to);
        }
    }
    advertise();
    Q_EMIT remoteTasksChanged();
    QMetaObject::invokeMethod(this, [this]() { pump(); }, Qt::QueuedConnection);
}

// --- PeerWorker

PeerWorker::PeerWorker(RenderNetwork* network, Room* room, Peer peer) : m_network(network), m_room(room), m_peer(std::move(peer)) {}

std::string PeerWorker::name() const { return m_peer.name.isEmpty() ? s(m_peer.id) : s(m_peer.name); }

bool PeerWorker::canRun(const RenderJob& job) const {
    return m_network && m_room && m_network->canRun(m_room, m_peer.id, job);
}

std::unique_ptr<RenderTaskHandle> PeerWorker::start(const RenderJob& job, const JobTask& task, TaskCallbacks callbacks) {
    if (!m_network || !m_room) {
        if (callbacks.declined) callbacks.declined("the peer has gone");
        return nullptr;
    }
    return m_network->startRemote(m_room, m_peer.id, job, task, std::move(callbacks));
}

}  // namespace usdcc::render
