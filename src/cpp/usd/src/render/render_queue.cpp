#include "usdcc/usd/render/render_queue.h"

#include "usdcc/core/log.h"
#include "usdcc/peers/channel.h"
#include "usdcc/peers/room.h"
#include "usdcc/peers/state.h"
#include "usdcc/peers/transfer.h"
#include "usdcc/usd/render/render_bundle.h"
#include "usdcc/usd/render/render_network.h"
#include "usdcc/usd/render/render_scheduler.h"

#include <pxr/usd/usd/stage.h>

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <chrono>

PXR_NAMESPACE_USING_DIRECTIVE
using usdcc::peers::Channel;
using usdcc::peers::Delivery;
using usdcc::peers::Message;
using usdcc::peers::Peer;
using usdcc::peers::PeerState;
using usdcc::peers::Room;
using usdcc::peers::StateMap;
using usdcc::peers::Transfer;
using usdcc::peers::TransferInbox;
using usdcc::peers::TransferManifest;

namespace usdcc::render {

namespace {

RenderQueueClient* s_client = nullptr;
constexpr int kPublishMs = 300;
constexpr int kSaveMs = 500;

QString q(const std::string& text) { return QString::fromStdString(text); }
std::string s(const QString& text) { return text.toStdString(); }

QJsonArray rectToJson(const PixelRect& r) { return QJsonArray{r.x, r.y, r.width, r.height}; }
PixelRect rectFromJson(const QJsonValue& value) {
    const QJsonArray a = value.toArray();
    return {a.at(0).toInt(), a.at(1).toInt(), a.at(2).toInt(), a.at(3).toInt()};
}

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

bool active(const RenderJob& job) { return job.status == RenderJob::Status::Queued || job.status == RenderJob::Status::Running; }

// `from` into `to`, files and folders.
bool copyTree(const QString& from, const QString& to) {
    QDir source(from);
    if (!source.exists() || !QDir().mkpath(to)) return false;
    QDirIterator it(from, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString target = QDir(to).filePath(source.relativeFilePath(file));
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile::remove(target);
        if (!QFile::copy(file, target)) return false;
    }
    return true;
}

bool writeJson(const QString& path, const QJsonObject& json) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
    return file.commit();
}

QJsonObject readJson(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonObject peerToJson(const Peer& peer) {
    return QJsonObject{{"name", peer.name}, {"fingerprint", peer.fingerprint}};
}

}  // namespace

// --- Wire formats

const char* jobStatusName(RenderJob::Status status) {
    switch (status) {
    case RenderJob::Status::Queued: return "queued";
    case RenderJob::Status::Running: return "running";
    case RenderJob::Status::Done: return "done";
    case RenderJob::Status::Failed: return "failed";
    case RenderJob::Status::Cancelled: return "cancelled";
    }
    return "queued";
}

RenderJob::Status jobStatusFromName(const QString& name) {
    if (name == u"running") return RenderJob::Status::Running;
    if (name == u"done") return RenderJob::Status::Done;
    if (name == u"failed") return RenderJob::Status::Failed;
    if (name == u"cancelled") return RenderJob::Status::Cancelled;
    return RenderJob::Status::Queued;
}

const char* taskStatusName(JobTask::Status status) {
    switch (status) {
    case JobTask::Status::Pending: return "pending";
    case JobTask::Status::Running: return "running";
    case JobTask::Status::Done: return "done";
    case JobTask::Status::Failed: return "failed";
    case JobTask::Status::Cancelled: return "cancelled";
    }
    return "pending";
}

JobTask::Status taskStatusFromName(const QString& name) {
    if (name == u"running") return JobTask::Status::Running;
    if (name == u"done") return JobTask::Status::Done;
    if (name == u"failed") return JobTask::Status::Failed;
    if (name == u"cancelled") return JobTask::Status::Cancelled;
    return JobTask::Status::Pending;
}

QJsonObject specToJson(const ResolvedSpec& spec) {
    QJsonArray products;
    for (const ProductSpec& product : spec.products) {
        QJsonArray aovs;
        for (const RenderAov& aov : product.aovs) aovs.append(QJsonArray{q(aov.name), q(aov.sourceName), q(aov.dataType.GetString())});
        products.append(QJsonObject{{"path", q(product.path.GetString())},
                                    {"name", q(product.name)},
                                    {"productName", q(product.productName)},
                                    {"aovs", aovs}});
    }
    QJsonArray purposes;
    for (const TfToken& purpose : spec.includedPurposes) purposes.append(q(purpose.GetString()));
    const JobOptions& o = spec.options;
    QJsonObject options{{"renderer", q(o.renderer)}, {"frameStep", o.frameStep},   {"output", q(o.output)},
                        {"format", q(o.format)},     {"tiles", QJsonArray{o.tilesX, o.tilesY}},
                        {"timeLimit", o.timeLimit},  {"distribute", q(o.distribute)}, {"delivery", q(o.delivery)}};
    if (o.frameStart) options["frameStart"] = *o.frameStart;
    if (o.frameEnd) options["frameEnd"] = *o.frameEnd;
    return QJsonObject{{"settings", q(spec.settings.GetString())},
                       {"camera", q(spec.camera.GetString())},
                       {"resolution", QJsonArray{spec.resolution[0], spec.resolution[1]}},
                       {"pixelAspect", double(spec.pixelAspect)},
                       {"products", products},
                       {"purposes", purposes},
                       {"options", options}};
}

ResolvedSpec specFromJson(const QJsonObject& json) {
    ResolvedSpec spec;
    const auto path = [](const QJsonValue& value) {
        const std::string text = s(value.toString());
        return !text.empty() && SdfPath::IsValidPathString(text) ? SdfPath(text) : SdfPath();
    };
    spec.settings = path(json["settings"]);
    spec.camera = path(json["camera"]);
    const QJsonArray resolution = json["resolution"].toArray();
    spec.resolution = GfVec2i(std::max(1, resolution.at(0).toInt(1)), std::max(1, resolution.at(1).toInt(1)));
    spec.pixelAspect = float(json["pixelAspect"].toDouble(1.0));
    for (const QJsonValue& p : json["products"].toArray()) {
        const QJsonObject o = p.toObject();
        ProductSpec product;
        product.path = path(o["path"]);
        product.name = s(o["name"].toString());
        product.productName = s(o["productName"].toString());
        for (const QJsonValue& a : o["aovs"].toArray()) {
            const QJsonArray aov = a.toArray();
            product.aovs.push_back({s(aov.at(0).toString()), s(aov.at(1).toString()), TfToken(s(aov.at(2).toString()))});
        }
        spec.products.push_back(std::move(product));
    }
    for (const QJsonValue& purpose : json["purposes"].toArray()) spec.includedPurposes.emplace_back(s(purpose.toString()));
    const QJsonObject o = json["options"].toObject();
    JobOptions& options = spec.options;
    options.renderer = s(o["renderer"].toString());
    if (o.contains("frameStart")) options.frameStart = o["frameStart"].toDouble();
    if (o.contains("frameEnd")) options.frameEnd = o["frameEnd"].toDouble();
    options.frameStep = o["frameStep"].toDouble(1.0);
    options.output = s(o["output"].toString());
    options.format = o["format"].toString() == u"png" ? "png" : "exr";
    const QJsonArray tiles = o["tiles"].toArray();
    options.tilesX = std::clamp(tiles.at(0).toInt(1), 1, 64);
    options.tilesY = std::clamp(tiles.at(1).toInt(1), 1, 64);
    options.timeLimit = o["timeLimit"].toDouble();
    options.distribute = s(o["distribute"].toString(QStringLiteral("local")));
    options.delivery = s(o["delivery"].toString(QStringLiteral("packaged")));
    return spec;
}

QJsonObject jobToJson(const RenderJob& job, bool status) {
    QJsonArray frames;
    for (double frame : job.frames) frames.append(frame);
    QJsonArray tasks;
    for (const JobTask& task : job.tasks) {
        QJsonObject t{{"task", taskToJson(task.task)}, {"frameIndex", task.frameIndex}, {"core", rectToJson(task.core)}};
        if (status) {
            t["status"] = taskStatusName(task.status);
            t["attempts"] = task.attempts;
            t["worker"] = q(task.worker);
            t["error"] = q(task.error);
        }
        tasks.append(t);
    }
    QJsonObject json{{"id", q(job.id)},
                     {"title", q(job.title)},
                     {"stage", q(job.stageName)},
                     {"settings", q(job.settingsName)},
                     {"renderer", q(job.rendererId)},
                     {"rendererName", q(job.rendererName)},
                     {"jobFolder", q(job.folderName)},
                     {"spec", specToJson(job.spec)},
                     {"frames", frames},
                     {"tasks", tasks}};
    if (status) {
        json["status"] = jobStatusName(job.status);
        json["error"] = q(job.error);
        QJsonArray files;
        for (const std::string& file : job.files) files.append(q(file));
        json["files"] = files;
    }
    return json;
}

RenderJobPtr jobFromJson(const QJsonObject& json, const UsdStageRefPtr& stage) {
    auto job = std::make_shared<RenderJob>();
    job->id = s(json["id"].toString());
    job->title = s(json["title"].toString());
    job->stageName = s(json["stage"].toString());
    job->settingsName = s(json["settings"].toString());
    job->rendererId = s(json["renderer"].toString());
    job->rendererName = s(json["rendererName"].toString());
    // One plain name: it becomes part of a path on whoever reads the job.
    const QString folder = json["jobFolder"].toString();
    const bool plain = !folder.isEmpty() && folder.size() <= 200 && folder != u"." && folder != u".." &&
                       !folder.endsWith(QLatin1Char('.')) && !folder.endsWith(QLatin1Char(' ')) &&
                       std::none_of(folder.begin(), folder.end(), [](QChar c) {
                           return c.unicode() < 0x20 || QStringLiteral("/\\:*?\"<>|").contains(c);
                       });
    if (plain) job->folderName = s(folder);
    job->spec = specFromJson(json["spec"].toObject());
    for (const QJsonValue& frame : json["frames"].toArray()) job->frames.push_back(frame.toDouble());
    for (const QJsonValue& t : json["tasks"].toArray()) {
        const QJsonObject o = t.toObject();
        JobTask task;
        task.task = taskFromJson(o["task"].toObject(), stage);
        task.task.jobId = job->id;
        task.task.index = static_cast<int>(job->tasks.size());
        task.frameIndex = std::clamp(o["frameIndex"].toInt(), 0, std::max(0, static_cast<int>(job->frames.size()) - 1));
        task.core = rectFromJson(o["core"]);
        if (o.contains("status")) {
            task.status = taskStatusFromName(o["status"].toString());
            task.attempts = o["attempts"].toInt();
            task.worker = s(o["worker"].toString());
            task.error = s(o["error"].toString());
        }
        job->tasks.push_back(std::move(task));
    }
    if (job->frames.empty() || job->tasks.empty()) return nullptr;
    if (json.contains("status")) {
        job->status = jobStatusFromName(json["status"].toString());
        job->error = s(json["error"].toString());
        for (const QJsonValue& file : json["files"].toArray()) job->files.push_back(s(file.toString()));
    }
    return job;
}

std::optional<std::string> queueOutputPattern(const RenderJob& job, const QString& root, const QString& keep,
                                              std::string* error) {
    // The job's own pattern, as outputPath() would take it.
    std::string pattern = job.spec.options.output;
    if (pattern.empty()) {
        if (job.spec.products.size() == 1 && !job.spec.products.front().productName.empty()) {
            pattern = job.spec.products.front().productName;
        } else {
            pattern = job.spec.products.size() > 1 ? "<stage>_<settings>_<product>" : "<stage>_<settings>";
        }
    }
    const QString text = QDir::fromNativeSeparators(q(pattern));
    if (root.isEmpty()) {
        // Kept by the queue until fetched: the file name only.
        return QDir::cleanPath(QDir(keep).filePath(QFileInfo(text).fileName())).toStdString();
    }
    const QString cleanRoot = QDir::cleanPath(QDir::fromNativeSeparators(root));
    if (QDir::isRelativePath(text)) {
        // In the job's own folder, as outputPath() would put it under the output folder.
        const QString inFolder = job.folderName.empty() || text.contains(QStringLiteral("<job>"))
                                     ? text
                                     : QStringLiteral("<job>/") + text;
        const QString path = QDir::cleanPath(QDir(cleanRoot).filePath(inFolder));
        // The pattern itself stays under the root, whatever folder goes in front.
        const QString asGiven = QDir::cleanPath(QDir(cleanRoot).filePath(text));
        if (!path.startsWith(cleanRoot + QLatin1Char('/'), Qt::CaseInsensitive) ||
            !asGiven.startsWith(cleanRoot + QLatin1Char('/'), Qt::CaseInsensitive)) {
            if (error) *error = "its output " + pattern + " leaves the queue's output root";
            return std::nullopt;
        }
        return path.toStdString();
    }
    const QString path = QDir::cleanPath(text);
    if (!path.startsWith(cleanRoot + QLatin1Char('/'), Qt::CaseInsensitive)) {
        if (error) *error = "its output " + pattern + " isn't inside the queue's output root " + s(cleanRoot);
        return std::nullopt;
    }
    return path.toStdString();
}

QJsonObject QueuedJobInfo::toJson() const {
    return QJsonObject{{"queue", queuePeer},     {"queueName", queueName},  {"id", id},
                       {"title", title},         {"submitter", submitter},  {"submitterFingerprint", submitterFingerprint},
                       {"renderer", renderer},   {"status", status},        {"done", done},
                       {"total", total},         {"frames", frames},        {"progress", progress},
                       {"nodes", QJsonArray::fromStringList(nodes)},        {"submittedMs", submittedMs},
                       {"finishedMs", finishedMs}, {"error", error},        {"output", output}};
}

QueuedJobInfo QueuedJobInfo::fromJson(const QString& key, const QJsonObject& json) {
    QueuedJobInfo info;
    info.key = key;
    info.queuePeer = json["queue"].toString();
    info.queueName = json["queueName"].toString();
    info.id = json["id"].toString();
    info.title = json["title"].toString();
    info.submitter = json["submitter"].toString();
    info.submitterFingerprint = json["submitterFingerprint"].toString();
    info.renderer = json["renderer"].toString();
    info.status = json["status"].toString();
    info.done = json["done"].toInt();
    info.total = json["total"].toInt();
    info.frames = json["frames"].toInt();
    info.progress = json["progress"].toDouble();
    info.nodes = json["nodes"].toVariant().toStringList();
    info.submittedMs = json["submittedMs"].toInteger();
    info.finishedMs = json["finishedMs"].toInteger();
    info.error = json["error"].toString();
    info.output = json["output"].toString();
    return info;
}

// --- The server

struct RenderQueueServer::RoomLink {
    QPointer<Room> room;
    Channel* jobs = nullptr;
    TransferInbox* inbox = nullptr;
    StateMap* map = nullptr;
    PeerState* state = nullptr;
    QObject context;
};

struct RenderQueueServer::Entry {
    RenderJobPtr job;
    QString artistJob;
    Peer submitter;  // its name and fingerprint; its id changes with every run
    QString artistFolder;
    qint64 submittedMs = 0;
    qint64 finishedMs = 0;
    bool delivered = false;  // its frames fetched (no output root)
    QPointer<Transfer> sending;
};

struct RenderQueueServer::Pending {
    QPointer<Room> room;
    Peer from;
    QJsonObject body;
    QString hash;
};

RenderQueueServer::RenderQueueServer(RenderQueueOptions options, QObject* parent) : QObject(parent), m_options(std::move(options)) {
    if (!m_options.scheduler) m_options.scheduler = &RenderScheduler::instance();
    if (m_options.stateFolder.isEmpty()) {
        m_temporaryState = QDir::temp().filePath(QStringLiteral("usdcc-queue-") + QUuid::createUuid().toString(QUuid::Id128).left(8));
        m_options.stateFolder = m_temporaryState;
    }
    QDir().mkpath(m_options.stateFolder);
    m_publishTimer = new QTimer(this);
    m_publishTimer->setSingleShot(true);
    m_publishTimer->setInterval(kPublishMs);
    connect(m_publishTimer, &QTimer::timeout, this, &RenderQueueServer::publish);
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kSaveMs);
    connect(m_saveTimer, &QTimer::timeout, this, [this]() {
        const QSet<QString> ids = std::exchange(m_dirtySave, {});
        for (const QString& id : ids) {
            if (const Entry* e = entry(id)) save(*e);
        }
    });

    RenderScheduler* scheduler = m_options.scheduler;
    scheduler->onTaskImages = [this](const RenderJob& job, int taskIndex, const std::map<std::string, FloatImage>& images) {
        if (entry(q(job.id))) saveTiles(job, taskIndex, images);
    };
    connect(scheduler, &RenderScheduler::jobChanged, this, [this](const QString& id) {
        if (!entry(id)) return;
        publishSoon(id);
        saveSoon(id);
        Q_EMIT jobChanged(id);
    });
    connect(scheduler, &RenderScheduler::frameRendered, this, [this](const QString& id, int frameIndex, const QImage&) {
        if (entry(id)) dropTiles(id, frameIndex);
    });
    connect(scheduler, &RenderScheduler::jobEnded, this, &RenderQueueServer::onJobEnded);
}

RenderQueueServer::~RenderQueueServer() {
    m_options.scheduler->onTaskImages = nullptr;
    for (const auto& e : m_entries) save(*e);
    std::vector<Room*> rooms;
    for (const auto& [room, l] : m_rooms) rooms.push_back(room);
    for (Room* room : rooms) removeRoom(room);
    if (!m_temporaryState.isEmpty()) QDir(m_temporaryState).removeRecursively();
}

QString RenderQueueServer::folder(const char* what) const { return QDir(m_options.stateFolder).filePath(QLatin1String(what)); }

RenderQueueServer::RoomLink* RenderQueueServer::link(Room* room) const {
    const auto found = m_rooms.find(room);
    return found == m_rooms.end() ? nullptr : found->second.get();
}

RenderQueueServer::Entry* RenderQueueServer::entry(const QString& id) const {
    for (const auto& e : m_entries) {
        if (q(e->job->id) == id) return e.get();
    }
    return nullptr;
}

RenderJobPtr RenderQueueServer::job(const QString& id) const {
    const Entry* e = entry(id);
    return e ? e->job : nullptr;
}

void RenderQueueServer::addRoom(Room* room) {
    if (!room || m_rooms.count(room)) return;
    auto owned = std::make_unique<RoomLink>();
    RoomLink* l = owned.get();
    l->room = room;
    l->jobs = room->channel(QString::fromLatin1(kQueueChannel), Delivery::Direct);
    l->map = room->state(QString::fromLatin1(kQueueMap));
    l->state = room->peerState(QString::fromLatin1(kQueueState));
    TransferInbox::Options inbox;
    inbox.directory = folder("inbox");
    inbox.accept = [this](const Peer& from, const TransferManifest&) { return m_options.accepts && m_options.accepts(from); };
    l->inbox = new TransferInbox(room, QString::fromLatin1(kQueueFilesChannel), inbox, &l->context);
    m_rooms[room] = std::move(owned);

    const auto announce = [this, l]() {
        if (!l->room || l->room->state() != Room::State::Open) return;
        // Only the queue writes its jobs, where it hosts the room.
        if (l->room->isHost()) l->map->setNamedSenders({l->room->me().id});
        l->state->set(QJsonObject{{"name", m_options.name}, {"root", !m_options.outputRoot.isEmpty()}});
        publish();
        deliverFrames();
    };
    connect(l->jobs, &Channel::received, &l->context, [this, l](const Message& message) { onMessage(*l, message); });
    connect(l->inbox, &TransferInbox::received, &l->context,
            [this, l](Transfer* transfer) { onSceneArrived(*l, transfer->bundleHash()); });
    connect(room, &Room::opened, &l->context, announce);
    connect(room, &Room::peerJoined, &l->context, [this]() {
        QTimer::singleShot(200, this, [this]() { deliverFrames(); });
    });
    connect(room, &Room::closed, &l->context, [this, room](const QString&) {
        QMetaObject::invokeMethod(this, [this, room]() { removeRoom(room); }, Qt::QueuedConnection);
    });
    connect(room, &QObject::destroyed, &l->context, [this, room]() { removeRoom(room); });
    announce();
}

void RenderQueueServer::removeRoom(Room* room) {
    m_rooms.erase(room);
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [room](const std::unique_ptr<Pending>& p) { return p->room == room || !p->room; }),
                    m_pending.end());
}

void RenderQueueServer::onMessage(RoomLink& link, const Message& message) {
    if (!link.room) return;
    const Peer from = link.room->peer(message.from);
    if (!from.isValid() || from.kind == Peer::Kind::Guest) return;
    const QString id = message.body["id"].toString();
    const auto deny = [&](const QString& action, const QString& why) {
        link.jobs->send(QStringLiteral("denied"), QJsonObject{{"id", id}, {"action", action}, {"reason", why}}, {}, from.id);
    };
    if (message.type == u"submit") return onSubmit(link, from, message.body);
    QString error;
    if (message.type == u"cancel") {
        if (!cancel(id, &from, &error)) deny(message.type, error);
    } else if (message.type == u"retry") {
        if (!retry(id, &from, &error)) deny(message.type, error);
    } else if (message.type == u"remove") {
        if (!remove(id, &from, &error)) deny(message.type, error);
    } else if (message.type == u"tasks") {
        link.jobs->send(QStringLiteral("tasks"), QJsonObject{{"id", id}, {"tasks", tasksJson(id)}}, {}, from.id);
    } else if (message.type == u"received") {
        Entry* e = entry(id);
        if (!e || e->submitter.fingerprint != from.fingerprint) return;
        e->delivered = true;
        // Once the transfer has let go of the files.
        const QString frames = folder("frames") + QLatin1Char('/') + id;
        // The sending side closes its last file when its transfer ends, which may be just
        // after the artist has it all.
        const auto clean = [this, frames]() {
            QMetaObject::invokeMethod(this, [frames]() { QDir(frames).removeRecursively(); }, Qt::QueuedConnection);
        };
        Transfer* sending = e->sending;
        const bool over = !sending || sending->state() == Transfer::State::Done || sending->state() == Transfer::State::Failed ||
                          sending->state() == Transfer::State::Cancelled;
        if (over) {
            clean();
        } else {
            connect(sending, &Transfer::finished, this, clean);
            connect(sending, &Transfer::failed, this, clean);
        }
        save(*e);
        publishSoon(id);
        USDCC_LOG_INFO("usdcc.queue", "{}'s frames are with {}", s(id), s(from.name));
    }
}

void RenderQueueServer::onSubmit(RoomLink& link, const Peer& from, const QJsonObject& body) {
    const QString artistJob = body["id"].toString();
    const auto refuse = [&](const QString& why) {
        USDCC_LOG_INFO("usdcc.queue", "Refused {} from {}: {}", s(body["title"].toString()), s(from.name), s(why));
        link.jobs->send(QStringLiteral("refused"), QJsonObject{{"job", artistJob}, {"reason", why}}, {}, from.id);
        Q_EMIT refused(from.name, why);
    };
    if (!m_options.accepts || !m_options.accepts(from)) return refuse(QStringLiteral("this queue doesn't take jobs from you"));
    const RenderJobPtr probe = jobFromJson(body, nullptr);
    if (!probe) return refuse(QStringLiteral("the job has no frames"));
    std::string error;
    if (!queueOutputPattern(*probe, m_options.outputRoot, folder("frames"), &error)) return refuse(q(error));
    const QString hash = body["hash"].toString();
    if (hash.isEmpty()) return refuse(QStringLiteral("the job has no scene"));
    Pending pending{link.room, from, body, hash};
    if (link.inbox->hasBundle(hash)) return startJob(std::move(pending));
    m_pending.push_back(std::make_unique<Pending>(std::move(pending)));
    USDCC_LOG_INFO("usdcc.queue", "{} from {}: waiting for its scene", s(body["title"].toString()), s(from.name));
}

void RenderQueueServer::onSceneArrived(RoomLink& link, const QString& hash) {
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if ((*it)->hash == hash && (*it)->room == link.room) {
            Pending pending = std::move(**it);
            it = m_pending.erase(it);
            startJob(std::move(pending));
        } else {
            ++it;
        }
    }
}

void RenderQueueServer::startJob(Pending pending) {
    RoomLink* l = link(pending.room);
    if (!l) return;
    const QString artistJob = pending.body["id"].toString();
    const auto refuse = [&](const QString& why) {
        l->jobs->send(QStringLiteral("refused"), QJsonObject{{"job", artistJob}, {"reason", why}}, {}, pending.from.id);
        Q_EMIT refused(pending.from.name, why);
    };
    const QString id = QStringLiteral("q") + QUuid::createUuid().toString(QUuid::Id128).left(10);
    // The scene, kept with the job until it ends; the inbox may let go of its copy.
    const QString scene = folder("scenes") + QLatin1Char('/') + id;
    if (!copyTree(l->inbox->bundleDirectory(pending.hash), scene)) return refuse(QStringLiteral("couldn't keep its scene"));
    const UsdStageRefPtr stage = UsdStage::Open(QDir(scene).filePath(QString::fromLatin1(kBundleScene)).toStdString());
    if (!stage) {
        QDir(scene).removeRecursively();
        return refuse(QStringLiteral("its scene doesn't open"));
    }
    QJsonObject body = pending.body;
    body["id"] = id;
    const RenderJobPtr job = jobFromJson(body, stage);
    std::string error;
    const std::optional<std::string> pattern =
        job ? queueOutputPattern(*job, m_options.outputRoot, folder("frames") + QLatin1Char('/') + id, &error) : std::nullopt;
    if (!job || !pattern) {
        QDir(scene).removeRecursively();
        return refuse(job ? q(error) : QStringLiteral("the job has no frames"));
    }
    job->spec.options.output = *pattern;
    job->spec.options.distribute = m_options.distribute;
    job->spec.options.delivery = "packaged";
    job->spec.options.rooms.clear();

    auto e = std::make_unique<Entry>();
    e->job = job;
    e->artistJob = artistJob;
    e->submitter = pending.from;
    e->artistFolder = pending.body["folder"].toString();
    e->submittedMs = nowMs();
    Entry& added = *e;
    m_entries.push_back(std::move(e));
    save(added);
    int position = 0;
    for (const auto& other : m_entries) {
        if (other.get() != &added && active(*other->job)) ++position;
    }
    USDCC_LOG_INFO("usdcc.queue", "Queued {} as {} for {}: {} task(s), to {}", job->title, s(id), s(pending.from.name),
                   job->tasks.size(), *pattern);
    l->jobs->send(QStringLiteral("queued"), QJsonObject{{"job", artistJob}, {"id", id}, {"position", position}}, {},
                  pending.from.id);
    m_options.scheduler->submit(job);
    publishSoon(id);
    Q_EMIT jobsChanged();
}

bool RenderQueueServer::allowed(const Entry& e, const Peer* by, QString* error) const {
    if (!by) return true;
    if (!by->fingerprint.isEmpty() && by->fingerprint == e.submitter.fingerprint) return true;
    if (m_options.isAdmin && m_options.isAdmin(*by)) return true;
    if (error) *error = QStringLiteral("it's %1's job").arg(e.submitter.name);
    return false;
}

bool RenderQueueServer::cancel(const QString& id, const Peer* by, QString* error) {
    Entry* e = entry(id);
    if (!e) {
        if (error) *error = QStringLiteral("there's no job %1").arg(id);
        return false;
    }
    if (!allowed(*e, by, error)) return false;
    m_options.scheduler->cancel(s(id));
    USDCC_LOG_INFO("usdcc.queue", "{} cancelled by {}", e->job->title, by ? s(by->name) : std::string("the queue"));
    return true;
}

bool RenderQueueServer::retry(const QString& id, const Peer* by, QString* error) {
    Entry* e = entry(id);
    if (!e) {
        if (error) *error = QStringLiteral("there's no job %1").arg(id);
        return false;
    }
    if (!allowed(*e, by, error)) return false;
    e->finishedMs = 0;
    m_options.scheduler->retry(s(id));
    publishSoon(id);
    return true;
}

bool RenderQueueServer::remove(const QString& id, const Peer* by, QString* error) {
    Entry* e = entry(id);
    if (!e) {
        if (error) *error = QStringLiteral("there's no job %1").arg(id);
        return false;
    }
    if (!allowed(*e, by, error)) return false;
    m_options.scheduler->remove(s(id));
    for (const QString& what : {QStringLiteral("scenes"), QStringLiteral("tiles"), QStringLiteral("frames")}) {
        QDir(QDir(m_options.stateFolder).filePath(what + QLatin1Char('/') + id)).removeRecursively();
    }
    QFile::remove(folder("jobs") + QLatin1Char('/') + id + QStringLiteral(".json"));
    for (const auto& [room, l] : m_rooms) {
        if (l->room && l->room->state() == Room::State::Open) l->map->remove(l->room->me().id + QLatin1Char('|') + id);
    }
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [e](const std::unique_ptr<Entry>& x) { return x.get() == e; }),
                    m_entries.end());
    Q_EMIT jobsChanged();
    return true;
}

QueuedJobInfo RenderQueueServer::info(const Entry& e) const {
    const RenderJob& job = *e.job;
    QueuedJobInfo info;
    info.queueName = m_options.name;
    info.id = q(job.id);
    info.title = q(job.title);
    info.submitter = e.submitter.name;
    info.submitterFingerprint = e.submitter.fingerprint;
    info.renderer = q(job.rendererName);
    info.status = q(jobStatusName(job.status));
    info.done = job.doneTasks();
    info.total = static_cast<int>(job.tasks.size());
    info.frames = static_cast<int>(job.frames.size());
    info.progress = job.progress();
    for (const JobTask& task : job.tasks) {
        if (task.status == JobTask::Status::Running && !info.nodes.contains(q(task.worker))) info.nodes.append(q(task.worker));
    }
    info.submittedMs = e.submittedMs;
    info.finishedMs = e.finishedMs;
    info.error = q(job.error);
    info.output = q(RenderScheduler::outputFolder(job));
    return info;
}

std::vector<QueuedJobInfo> RenderQueueServer::jobs() const {
    std::vector<QueuedJobInfo> result;
    for (const auto& e : m_entries) result.push_back(info(*e));
    return result;
}

QJsonArray RenderQueueServer::tasksJson(const QString& id) const {
    QJsonArray tasks;
    const Entry* e = entry(id);
    if (!e) return tasks;
    const RenderJob& job = *e->job;
    for (const JobTask& task : job.tasks) {
        tasks.append(QJsonObject{{"index", task.task.index},
                                 {"frame", task.task.frame},
                                 {"tile", job.tilesPerFrame() > 1 ? task.task.index % job.tilesPerFrame() + 1 : 0},
                                 {"tiles", job.tilesPerFrame()},
                                 {"status", taskStatusName(task.status)},
                                 {"progress", task.progress},
                                 {"state", q(task.state)},
                                 {"worker", q(task.worker)},
                                 {"attempts", task.attempts},
                                 {"error", q(task.error)}});
    }
    return tasks;
}

void RenderQueueServer::publishSoon(const QString& id) {
    m_dirtyPublish.insert(id);
    if (!m_publishTimer->isActive()) m_publishTimer->start();
}

void RenderQueueServer::publish() {
    m_dirtyPublish.clear();
    for (const auto& [room, l] : m_rooms) {
        if (!l->room || l->room->state() != Room::State::Open) continue;
        const QString me = l->room->me().id;
        for (const auto& e : m_entries) {
            QueuedJobInfo i = info(*e);
            i.queuePeer = me;
            const QString key = me + QLatin1Char('|') + i.id;
            const QJsonObject value = i.toJson();
            if (l->map->value(key) != QJsonValue(value)) l->map->set(key, value);
        }
    }
}

void RenderQueueServer::saveSoon(const QString& id) {
    m_dirtySave.insert(id);
    if (!m_saveTimer->isActive()) m_saveTimer->start();
}

void RenderQueueServer::save(const Entry& e) const {
    const QJsonObject json{{"job", jobToJson(*e.job, true)},
                           {"artistJob", e.artistJob},
                           {"submitter", peerToJson(e.submitter)},
                           {"folder", e.artistFolder},
                           {"submittedMs", e.submittedMs},
                           {"finishedMs", e.finishedMs},
                           {"delivered", e.delivered}};
    if (!writeJson(folder("jobs") + QLatin1Char('/') + q(e.job->id) + QStringLiteral(".json"), json)) {
        USDCC_LOG_WARNING("usdcc.queue", "Couldn't save {} in {}", e.job->id, s(m_options.stateFolder));
    }
}

void RenderQueueServer::saveTiles(const RenderJob& job, int taskIndex, const std::map<std::string, FloatImage>& images) {
    const QString dir = folder("tiles") + QLatin1Char('/') + q(job.id) + QLatin1Char('/') + QString::number(taskIndex);
    QDir().mkpath(dir);
    for (const auto& [name, image] : images) {
        QSaveFile file(QDir(dir).filePath(q(name) + QStringLiteral(".rawz")));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(packImage(image));
            file.commit();
        }
    }
}

void RenderQueueServer::dropTiles(const QString& id, int frameIndex) {
    const Entry* e = entry(id);
    if (!e) return;
    for (const JobTask& task : e->job->tasks) {
        if (task.frameIndex != frameIndex) continue;
        QDir(folder("tiles") + QLatin1Char('/') + id + QLatin1Char('/') + QString::number(task.task.index)).removeRecursively();
    }
    saveSoon(id);
}

void RenderQueueServer::onJobEnded(const QString& id) {
    Entry* e = entry(id);
    if (!e) return;
    e->finishedMs = nowMs();
    const RenderJob& job = *e->job;
    QDir(folder("tiles") + QLatin1Char('/') + id).removeRecursively();
    save(*e);
    QJsonArray files;
    for (const std::string& file : job.files) files.append(q(file));
    sendTo(e->submitter.fingerprint, QStringLiteral("ended"),
           QJsonObject{{"id", id}, {"job", e->artistJob}, {"title", q(job.title)}, {"queue", m_options.name},
                       {"status", jobStatusName(job.status)}, {"error", q(job.error)}, {"files", files},
                       {"root", !m_options.outputRoot.isEmpty()}});
    publish();
    deliverFrames(*e);
    Q_EMIT jobChanged(id);
}

Room* RenderQueueServer::roomWith(const QString& fingerprint, QString* peer) const {
    if (fingerprint.isEmpty()) return nullptr;
    for (const auto& [room, l] : m_rooms) {
        if (!l->room || l->room->state() != Room::State::Open) continue;
        for (const Peer& p : l->room->peers()) {
            if (p.fingerprint == fingerprint && p.id != l->room->me().id) {
                if (peer) *peer = p.id;
                return l->room;
            }
        }
    }
    return nullptr;
}

void RenderQueueServer::sendTo(const QString& fingerprint, const QString& type, const QJsonObject& body) {
    QString peer;
    Room* room = roomWith(fingerprint, &peer);
    if (RoomLink* l = link(room)) l->jobs->send(type, body, {}, peer);
}

void RenderQueueServer::deliverFrames() {
    for (const auto& e : m_entries) deliverFrames(*e);
}

void RenderQueueServer::deliverFrames(Entry& e) {
    // Kept here only without an output root, and only once done.
    if (!m_options.outputRoot.isEmpty() || e.delivered || e.sending || e.job->status != RenderJob::Status::Done) return;
    const QString frames = folder("frames") + QLatin1Char('/') + q(e.job->id);
    if (!QDir(frames).exists()) return;
    QString peer;
    Room* room = roomWith(e.submitter.fingerprint, &peer);
    if (!room) return;
    QString error;
    const std::optional<TransferManifest> manifest = TransferManifest::fromDirectory(frames, &error);
    Transfer* transfer = manifest ? Transfer::send(room, QString::fromLatin1(kQueueFilesChannel), peer, *manifest, {}, this, &error)
                                  : nullptr;
    if (!transfer) {
        USDCC_LOG_WARNING("usdcc.queue", "Couldn't send {}'s frames: {}", e.job->id, s(error));
        return;
    }
    e.sending = transfer;
    QStringList files = QDir(frames).entryList(QDir::Files);
    link(room)->jobs->send(QStringLiteral("frames"),
                           QJsonObject{{"id", q(e.job->id)}, {"job", e.artistJob}, {"hash", manifest->hash()},
                                       {"folder", e.artistFolder}, {"files", QJsonArray::fromStringList(files)}},
                           {}, peer);
}

int RenderQueueServer::restore() {
    int count = 0;
    const QDir jobs(folder("jobs"));
    for (const QFileInfo& file : jobs.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Time | QDir::Reversed)) {
        const QJsonObject json = readJson(file.filePath());
        const QString id = json["job"].toObject()["id"].toString();
        if (id.isEmpty() || entry(id)) continue;
        const QString scene = folder("scenes") + QLatin1Char('/') + id;
        const UsdStageRefPtr stage = UsdStage::Open(QDir(scene).filePath(QString::fromLatin1(kBundleScene)).toStdString());
        const RenderJobPtr job = stage ? jobFromJson(json["job"].toObject(), stage) : nullptr;
        if (!job) {
            USDCC_LOG_WARNING("usdcc.queue", "Couldn't restore {}: {}", s(id), stage ? "it has no tasks" : "its scene doesn't open");
            continue;
        }
        auto e = std::make_unique<Entry>();
        e->job = job;
        e->artistJob = json["artistJob"].toString();
        e->submitter.name = json["submitter"].toObject()["name"].toString();
        e->submitter.fingerprint = json["submitter"].toObject()["fingerprint"].toString();
        e->artistFolder = json["folder"].toString();
        e->submittedMs = json["submittedMs"].toInteger();
        e->finishedMs = json["finishedMs"].toInteger();
        e->delivered = json["delivered"].toBool();

        // Tiles: a frame whose tiles are all done and none kept was written; a done tile kept
        // goes back into its frame; a done tile not kept, of a frame not written, renders again.
        std::vector<std::pair<int, std::map<std::string, FloatImage>>> tiles;
        if (active(*job)) {
            std::map<int, std::vector<int>> byFrame;
            for (const JobTask& task : job->tasks) byFrame[task.frameIndex].push_back(task.task.index);
            for (const auto& [frameIndex, indices] : byFrame) {
                bool allDone = true, anyKept = false;
                for (int index : indices) {
                    allDone = allDone && job->tasks[static_cast<size_t>(index)].status == JobTask::Status::Done;
                    anyKept = anyKept || QDir(folder("tiles") + QLatin1Char('/') + id + QLatin1Char('/') + QString::number(index)).exists();
                }
                if (allDone && !anyKept) continue;  // written before the restart
                for (int index : indices) {
                    JobTask& task = job->tasks[static_cast<size_t>(index)];
                    if (task.status != JobTask::Status::Done) continue;
                    const QDir dir(folder("tiles") + QLatin1Char('/') + id + QLatin1Char('/') + QString::number(index));
                    std::map<std::string, FloatImage> images;
                    for (const QFileInfo& image : dir.entryInfoList({QStringLiteral("*.rawz")}, QDir::Files)) {
                        QFile in(image.filePath());
                        if (!in.open(QIODevice::ReadOnly)) continue;
                        if (std::optional<FloatImage> unpacked = unpackImage(in.readAll())) {
                            images[s(image.completeBaseName())] = std::move(*unpacked);
                        }
                    }
                    if (images.empty()) {
                        task.status = JobTask::Status::Pending;
                        task.progress = 0.0;
                    } else {
                        tiles.emplace_back(index, std::move(images));  // stays done: not handed out again
                    }
                }
            }
        }
        m_entries.push_back(std::move(e));
        m_options.scheduler->adopt(job);
        for (auto& [index, images] : tiles) m_options.scheduler->restoreTask(job, index, std::move(images));
        ++count;
    }
    if (count) {
        USDCC_LOG_INFO("usdcc.queue", "Restored {} job(s) from {}", count, s(m_options.stateFolder));
        Q_EMIT jobsChanged();
        publish();
    }
    return count;
}

// --- The artist's side

struct RenderQueueClient::RoomLink {
    QPointer<Room> room;
    Channel* jobs = nullptr;
    TransferInbox* inbox = nullptr;
    StateMap* map = nullptr;
    PeerState* state = nullptr;
    QObject context;
};

RenderQueueClient::RenderQueueClient(Options options, QObject* parent) : QObject(parent), m_options(std::move(options)) {
    if (m_options.folder.isEmpty()) m_options.folder = QDir::temp().filePath(QStringLiteral("usdcc-queue-client"));
    QDir().mkpath(m_options.folder);
}

RenderQueueClient::~RenderQueueClient() {
    if (s_client == this) s_client = nullptr;
    std::vector<Room*> rooms;
    for (const auto& [room, l] : m_rooms) rooms.push_back(room);
    for (Room* room : rooms) removeRoom(room);
}

RenderQueueClient* RenderQueueClient::instance() { return s_client; }
void RenderQueueClient::setInstance(RenderQueueClient* client) { s_client = client; }

RenderQueueClient::RoomLink* RenderQueueClient::link(Room* room) const {
    const auto found = m_rooms.find(room);
    return found == m_rooms.end() ? nullptr : found->second.get();
}

void RenderQueueClient::addRoom(Room* room) {
    if (!room || m_rooms.count(room)) return;
    auto owned = std::make_unique<RoomLink>();
    RoomLink* l = owned.get();
    l->room = room;
    l->jobs = room->channel(QString::fromLatin1(kQueueChannel), Delivery::Direct);
    l->map = room->state(QString::fromLatin1(kQueueMap));
    l->state = room->peerState(QString::fromLatin1(kQueueState));
    TransferInbox::Options inbox;
    inbox.directory = QDir(m_options.folder).filePath(QStringLiteral("inbox"));
    // Frames, from the queues here only.
    inbox.accept = [l](const Peer& from, const TransferManifest&) {
        return l->state && l->state->value(from.id).isObject();
    };
    l->inbox = new TransferInbox(room, QString::fromLatin1(kQueueFilesChannel), inbox, &l->context);
    m_rooms[room] = std::move(owned);
    connect(l->jobs, &Channel::received, &l->context, [this, l](const Message& message) { onMessage(*l, message); });
    connect(l->state, &PeerState::changed, &l->context, [this]() { Q_EMIT queuesChanged(); });
    connect(l->map, &StateMap::changed, &l->context, [this]() { Q_EMIT jobsChanged(); });
    connect(l->inbox, &TransferInbox::received, &l->context, [this, l](Transfer* transfer) {
        for (const Fetch& found : m_fetches) {
            if (found.hash != transfer->bundleHash()) continue;
            const Fetch fetch = found;  // onFrames() takes it off the list
            onFrames(*l, fetch.queuePeer, QJsonObject{{"id", fetch.id}, {"hash", fetch.hash}, {"folder", fetch.folder}});
            return;
        }
    });
    connect(room, &Room::closed, &l->context, [this, room](const QString&) {
        QMetaObject::invokeMethod(this, [this, room]() { removeRoom(room); }, Qt::QueuedConnection);
    });
    connect(room, &QObject::destroyed, &l->context, [this, room]() { removeRoom(room); });
    Q_EMIT queuesChanged();
    Q_EMIT jobsChanged();
}

void RenderQueueClient::removeRoom(Room* room) {
    if (!m_rooms.erase(room)) return;
    Q_EMIT queuesChanged();
    Q_EMIT jobsChanged();
}

std::vector<RenderQueueInfo> RenderQueueClient::queues() const {
    std::vector<RenderQueueInfo> result;
    for (const auto& [room, l] : m_rooms) {
        if (!l->room) continue;
        const QHash<QString, QJsonValue> values = l->state->values();
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!it.value().isObject() || it.key() == l->room->me().id) continue;
            const Peer peer = l->room->peer(it.key());
            if (!peer.isValid()) continue;
            RenderQueueInfo info;
            info.room = l->room;
            info.peer = peer;
            info.name = it.value().toObject()["name"].toString(peer.name);
            info.outputRoot = it.value().toObject()["root"].toBool();
            result.push_back(info);
        }
    }
    return result;
}

std::optional<RenderQueueInfo> RenderQueueClient::queue(const QString& nameOrPeer) const {
    for (const RenderQueueInfo& info : queues()) {
        if (nameOrPeer.isEmpty() || info.name == nameOrPeer || info.peer.id == nameOrPeer || info.peer.fingerprint == nameOrPeer) {
            return info;
        }
    }
    return std::nullopt;
}

RenderQueueClient::RoomLink* RenderQueueClient::linkOf(const QString& queuePeer) const {
    for (const auto& [room, l] : m_rooms) {
        if (l->room && l->room->peer(queuePeer).isValid() && l->state->value(queuePeer).isObject()) return l.get();
    }
    return nullptr;
}

std::vector<QueuedJobInfo> RenderQueueClient::jobs() const {
    std::vector<QueuedJobInfo> result;
    QSet<QString> seen;
    for (const auto& [room, l] : m_rooms) {
        if (!l->room) continue;
        const QJsonObject items = l->map->items();
        for (auto it = items.begin(); it != items.end(); ++it) {
            if (!it.value().isObject() || seen.contains(it.key())) continue;
            QueuedJobInfo info = QueuedJobInfo::fromJson(it.key(), it.value().toObject());
            // Only queues that are here: a queue that left takes its jobs with it.
            if (!l->state->value(info.queuePeer).isObject()) continue;
            seen.insert(it.key());
            result.push_back(std::move(info));
        }
    }
    std::sort(result.begin(), result.end(), [](const QueuedJobInfo& a, const QueuedJobInfo& b) { return a.submittedMs < b.submittedMs; });
    return result;
}

std::optional<QueuedJobInfo> RenderQueueClient::job(const QString& key) const {
    for (QueuedJobInfo& info : jobs()) {
        if (info.key == key) return info;
    }
    return std::nullopt;
}

bool RenderQueueClient::submit(const RenderJobPtr& job, const QString& queueName, QString* error) {
    const auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (!job || job->tasks.empty()) return fail(QStringLiteral("there's no job"));
    const std::optional<RenderQueueInfo> queue = this->queue(queueName);
    RoomLink* l = queue ? link(queue->room) : nullptr;
    if (!l || !l->room) return fail(queueName.isEmpty() ? QStringLiteral("there's no render queue in your rooms")
                                                        : QStringLiteral("the queue %1 isn't in your rooms").arg(queueName));
    // The scene: the job's snapshot, packaged.
    const QString bundle = QDir(m_options.folder).filePath(QStringLiteral("bundles/") + q(job->id) + QLatin1Char('-') +
                                                           QUuid::createUuid().toString(QUuid::Id128).left(8));
    QString why;
    if (!writeSceneBundle(job->tasks.front().task.stage, bundle, &why)) {
        QDir(bundle).removeRecursively();
        return fail(QStringLiteral("couldn't package the scene: %1").arg(why));
    }
    const std::optional<TransferManifest> manifest = TransferManifest::fromDirectory(bundle, &why);
    Transfer* transfer = manifest ? Transfer::send(l->room, QString::fromLatin1(kQueueFilesChannel), queue->peer.id, *manifest, {}, this, &why)
                                  : nullptr;
    if (!transfer) {
        QDir(bundle).removeRecursively();
        return fail(QStringLiteral("couldn't send the scene: %1").arg(why));
    }
    const auto clean = [bundle]() { QDir(bundle).removeRecursively(); };
    connect(transfer, &Transfer::finished, this, clean);
    connect(transfer, &Transfer::failed, this, clean);
    QJsonObject body = jobToJson(*job, false);
    body["hash"] = manifest->hash();
    body["folder"] = q(RenderScheduler::outputFolder(*job));
    l->jobs->send(QStringLiteral("submit"), body, {}, queue->peer.id);
    USDCC_LOG_INFO("usdcc.render", "Sent {} to the queue {}", job->title, s(queue->name));
    return true;
}

void RenderQueueClient::sendAction(const QString& key, const QString& type) {
    const std::optional<QueuedJobInfo> info = job(key);
    if (!info) return;
    if (RoomLink* l = linkOf(info->queuePeer)) l->jobs->send(type, QJsonObject{{"id", info->id}}, {}, info->queuePeer);
}

void RenderQueueClient::cancel(const QString& key) { sendAction(key, QStringLiteral("cancel")); }
void RenderQueueClient::retry(const QString& key) { sendAction(key, QStringLiteral("retry")); }
void RenderQueueClient::remove(const QString& key) { sendAction(key, QStringLiteral("remove")); }
void RenderQueueClient::requestTasks(const QString& key) { sendAction(key, QStringLiteral("tasks")); }

void RenderQueueClient::onMessage(RoomLink& link, const Message& message) {
    if (!link.room || !link.state->value(message.from).isObject()) return;  // queues only
    const QJsonObject& body = message.body;
    const QString queueName = link.state->value(message.from).toObject()["name"].toString();
    if (message.type == u"queued") {
        Q_EMIT queued(body["job"].toString(), body["id"].toString(), body["position"].toInt());
    } else if (message.type == u"refused") {
        using usdcc::core::ToastKind;
        using usdcc::core::ToastRequest;
        ToastRequest toast = ToastRequest::sticky();
        toast.kind = ToastKind::Error;
        toast.title = "The queue refused the job";
        USDCC_LOG_TOAST_AS(usdcc::core::LogLevel::Warning, "usdcc.render", toast, "{}: {}", s(queueName), s(body["reason"].toString()));
        Q_EMIT refused(body["job"].toString(), body["reason"].toString());
    } else if (message.type == u"denied") {
        // A toast, not a question: the panel's button was pressed, and this is the answer.
        using usdcc::core::ToastKind;
        using usdcc::core::ToastRequest;
        ToastRequest toast;
        toast.kind = ToastKind::Warning;
        toast.title = "The queue said no";
        USDCC_LOG_TOAST_AS(usdcc::core::LogLevel::Warning, "usdcc.render", toast, "{} {} on {}: {}", s(body["action"].toString()),
                           s(body["id"].toString()), s(queueName), s(body["reason"].toString()));
        Q_EMIT denied(body["id"].toString(), body["reason"].toString());
    } else if (message.type == u"ended") {
        // The notification, in whichever instance the artist has open (PEERS.md §10.2).
        using usdcc::core::LogLevel;
        using usdcc::core::ToastKind;
        using usdcc::core::ToastRequest;
        const QString status = body["status"].toString();
        ToastRequest toast;
        if (status == u"done") {
            toast.kind = ToastKind::Success;
            toast.title = "Render done on " + s(queueName);
            USDCC_LOG_TOAST_AS(LogLevel::Info, "usdcc.render", toast, "{}: {} file(s)", s(body["title"].toString()),
                               body["files"].toArray().size());
        } else if (status == u"failed") {
            toast = ToastRequest::sticky();
            toast.kind = ToastKind::Error;
            toast.title = "Render failed on " + s(queueName);
            USDCC_LOG_TOAST_AS(LogLevel::Error, "usdcc.render", toast, "{}: {}", s(body["title"].toString()), s(body["error"].toString()));
        }
        Q_EMIT ended(body["id"].toString(), status);
    } else if (message.type == u"tasks") {
        Q_EMIT tasksReceived(message.from + QLatin1Char('|') + body["id"].toString(), body["tasks"].toArray());
    } else if (message.type == u"frames") {
        const QString hash = body["hash"].toString();
        if (link.inbox->hasBundle(hash)) {
            onFrames(link, message.from, body);
        } else {
            m_fetches.push_back({body["id"].toString(), body["folder"].toString(), hash, message.from});
        }
    }
}

void RenderQueueClient::onFrames(RoomLink& link, const QString& queuePeer, const QJsonObject& body) {
    const QString id = body["id"].toString();
    const QString hash = body["hash"].toString();
    m_fetches.erase(std::remove_if(m_fetches.begin(), m_fetches.end(), [&hash](const Fetch& f) { return f.hash == hash; }),
                    m_fetches.end());
    // Where the artist asked for them; the default output folder when that's gone.
    QString target = body["folder"].toString();
    if (target.isEmpty()) target = q(defaultOutputFolder());
    const QString from = link.inbox->bundleDirectory(hash);
    QStringList files;
    QDir().mkpath(target);
    for (const QFileInfo& file : QDir(from).entryInfoList(QDir::Files)) {
        const QString to = QDir(target).filePath(file.fileName());
        QFile::remove(to);
        if (QFile::copy(file.filePath(), to)) files.append(to);
    }
    QDir(from).removeRecursively();
    link.jobs->send(QStringLiteral("received"), QJsonObject{{"id", id}}, {}, queuePeer);
    USDCC_LOG_INFO("usdcc.render", "{} frame file(s) from the queue in {}", files.size(), s(target));
    Q_EMIT framesArrived(id, target, files);
}

}  // namespace usdcc::render
