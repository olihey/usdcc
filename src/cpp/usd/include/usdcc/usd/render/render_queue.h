#pragma once

// A render queue server (docs/RENDER_NODE.md §4.4): jobs that outlive the artist's instance.
// The queue takes a job and its scene from an artist, hands its tasks to the render nodes in
// its rooms through its own RenderNetwork, writes the frames (under an output root, or keeps
// them for the artist to fetch), saves what it has so that it goes on after a restart, and
// publishes its jobs for the Render Queue panel and its web page.
//
// The artist's side is RenderQueueClient. On the Direct channel org.usdcc.render/queue-jobs:
//   artist → queue: submit {job, title, stage, settings, renderer, rendererName, spec, frames,
//                    tasks, hash, folder}   cancel / retry / remove {id}   received {id}
//                   tasks {id}
//   queue → artist: queued {job, id, position}   refused {job, reason}
//                   ended {id, job, title, status, error, files}   frames {id, hash, folder}
//                   tasks {id, tasks}   denied {id, action, reason}
// Scenes go to the queue, and frames back, as transfers on org.usdcc.render/queue-files.
// The queue says it is one in the per-peer state org.usdcc.render/queuer {name, root}; its
// jobs are in the map org.usdcc.render/queue, a key "<queue peer id>|<job id>" each.

#include "usdcc/core/export.h"
#include "usdcc/peers/types.h"
#include "usdcc/usd/render/render_job.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::peers {
class Room;
}

namespace usdcc::render {

class RenderScheduler;

inline constexpr const char* kQueueChannel = "org.usdcc.render/queue-jobs";
inline constexpr const char* kQueueFilesChannel = "org.usdcc.render/queue-files";
inline constexpr const char* kQueueState = "org.usdcc.render/queuer";
inline constexpr const char* kQueueMap = "org.usdcc.render/queue";

// --- A job as it travels, and as a queue keeps it.

USDCC_RENDER_API QJsonObject specToJson(const ResolvedSpec& spec);
USDCC_RENDER_API ResolvedSpec specFromJson(const QJsonObject& json);
// With its tasks and, with `status`, where each stands (a queue's saved job).
USDCC_RENDER_API QJsonObject jobToJson(const RenderJob& job, bool status);
// Its tasks render `stage`. Null for a job without frames or tasks.
USDCC_RENDER_API RenderJobPtr jobFromJson(const QJsonObject& json, const PXR_NS::UsdStageRefPtr& stage);
USDCC_RENDER_API const char* jobStatusName(RenderJob::Status status);
USDCC_RENDER_API RenderJob::Status jobStatusFromName(const QString& name);
USDCC_RENDER_API const char* taskStatusName(JobTask::Status status);
USDCC_RENDER_API JobTask::Status taskStatusFromName(const QString& name);

// The output pattern a queue writes a job's frames with: under `root`, the job's pattern
// (its authored output, else its product's file name, else <stage>_<settings>) when it's
// relative, or the absolute one when it's inside `root`. Nothing, with `error`, for an
// absolute pattern outside it. With no root, the file name only, under `keep`.
USDCC_RENDER_API std::optional<std::string> queueOutputPattern(const RenderJob& job, const QString& root, const QString& keep,
                                                               std::string* error);

// --- The server.

struct USDCC_RENDER_API RenderQueueOptions {
    QString name = QStringLiteral("Render queue");
    // Jobs, scenes and finished tiles, kept across restarts; empty: a temporary folder, and
    // nothing survives the process.
    QString stateFolder;
    // Where the frames go (a shared path everyone opens them from); empty: kept in the state
    // folder until the artist's instance fetches them.
    QString outputRoot;
    // Whose jobs it takes; without it, nobody's.
    std::function<bool(const usdcc::peers::Peer&)> accepts;
    // Who may cancel, retry and remove anyone's jobs (artists may their own).
    std::function<bool(const usdcc::peers::Peer&)> isAdmin;
    // Its queue, whose workers the nodes are (and this machine, with a LocalWorker).
    RenderScheduler* scheduler = nullptr;
    // Who renders: "peers" (the nodes) or "both" (the nodes and this machine).
    std::string distribute = "peers";
};

// A job as the queue publishes it, and as the artist's instance and the web page show it.
struct USDCC_RENDER_API QueuedJobInfo {
    QString key;       // in the map
    QString queuePeer;  // the queue's peer id
    QString queueName;
    QString id;         // the queue's job id
    QString title;
    QString submitter;  // a name
    QString submitterFingerprint;
    QString renderer;
    QString status;     // jobStatusName(), or "receiving" while its scene arrives
    int done = 0;       // tasks
    int total = 0;
    int frames = 0;
    double progress = 0.0;
    QStringList nodes;  // rendering it now
    qint64 submittedMs = 0;
    qint64 finishedMs = 0;
    QString error;
    QString output;     // the folder its frames go to, on the queue's side

    QJsonObject toJson() const;
    static QueuedJobInfo fromJson(const QString& key, const QJsonObject& json);
};

class USDCC_RENDER_API RenderQueueServer : public QObject {
    Q_OBJECT

public:
    explicit RenderQueueServer(RenderQueueOptions options, QObject* parent = nullptr);
    ~RenderQueueServer() override;

    // A room where it takes jobs (any type; render nodes are the RenderNetwork's business).
    void addRoom(usdcc::peers::Room* room);
    void removeRoom(usdcc::peers::Room* room);

    // The jobs saved in the state folder, back in the queue (docs/RENDER_NODE.md §4.4): at
    // start, before any room. Returns how many.
    int restore();

    std::vector<QueuedJobInfo> jobs() const;
    // A job's tasks, as the web page and the tasks message have them.
    QJsonArray tasksJson(const QString& id) const;
    RenderJobPtr job(const QString& id) const;

    // What an artist, an admin or the web page asks for; false with `error` when it may not.
    // `by` null: the web page with its token, or this process.
    bool cancel(const QString& id, const usdcc::peers::Peer* by, QString* error = nullptr);
    bool retry(const QString& id, const usdcc::peers::Peer* by, QString* error = nullptr);
    bool remove(const QString& id, const usdcc::peers::Peer* by, QString* error = nullptr);

    const RenderQueueOptions& options() const { return m_options; }

Q_SIGNALS:
    // A job came, went, or changed (its status, a task's progress).
    void jobsChanged();
    void jobChanged(const QString& id);
    void refused(const QString& artist, const QString& reason);

private:
    struct RoomLink;
    struct Entry;
    struct Pending;

    RoomLink* link(usdcc::peers::Room* room) const;
    void onMessage(RoomLink& link, const usdcc::peers::Message& message);
    void onSubmit(RoomLink& link, const usdcc::peers::Peer& from, const QJsonObject& body);
    void onSceneArrived(RoomLink& link, const QString& hash);
    void startJob(Pending pending);
    bool allowed(const Entry& entry, const usdcc::peers::Peer* by, QString* error) const;
    Entry* entry(const QString& id) const;
    QueuedJobInfo info(const Entry& entry) const;
    void publishSoon(const QString& id);
    void publish();
    void saveSoon(const QString& id);
    void save(const Entry& entry) const;
    void saveTiles(const RenderJob& job, int taskIndex, const std::map<std::string, FloatImage>& images);
    void dropTiles(const QString& id, int frameIndex);
    void onJobEnded(const QString& id);
    void deliverFrames();
    void deliverFrames(Entry& entry);
    void sendTo(const QString& fingerprint, const QString& type, const QJsonObject& body);
    usdcc::peers::Room* roomWith(const QString& fingerprint, QString* peer) const;
    QString folder(const char* what) const;

    RenderQueueOptions m_options;
    std::map<usdcc::peers::Room*, std::unique_ptr<RoomLink>> m_rooms;
    std::vector<std::unique_ptr<Entry>> m_entries;
    std::vector<std::unique_ptr<Pending>> m_pending;  // waiting for their scenes
    QSet<QString> m_dirtyPublish;
    QSet<QString> m_dirtySave;
    QTimer* m_publishTimer = nullptr;
    QTimer* m_saveTimer = nullptr;
    QString m_temporaryState;
};

// --- The artist's side.

// A queue in a room this instance is in.
struct USDCC_RENDER_API RenderQueueInfo {
    QPointer<usdcc::peers::Room> room;
    usdcc::peers::Peer peer;
    QString name;
    bool outputRoot = false;  // writes to a shared path; else the frames come back here
};

// A job this instance sent to a queue, by the id it had here (RenderJob::id, what
// submitRender() and usdcc.render return), with what the queue has said of it since.
struct USDCC_RENDER_API SentJobInfo {
    QString artistJob;
    QString title;
    QString renderer;
    std::vector<double> frames;
    QString queuePeer;
    QString queueName;
    QString id;          // the queue's job id; empty until it answers queued
    QString status;      // "sending", "refused", or the queue's (QueuedJobInfo::status)
    double progress = 0.0;
    QString error;
    QStringList files;   // fetched here (none when the queue writes to a shared path)
};

class USDCC_RENDER_API RenderQueueClient : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString folder;  // the inbox for fetched frames
    };
    explicit RenderQueueClient(Options options, QObject* parent = nullptr);
    ~RenderQueueClient() override;

    // The app's, for the Render Settings and Render Queue panels; null without one.
    static RenderQueueClient* instance();
    static void setInstance(RenderQueueClient* client);

    void addRoom(usdcc::peers::Room* room);
    void removeRoom(usdcc::peers::Room* room);

    std::vector<RenderQueueInfo> queues() const;
    // The queue that `nameOrPeer` names (its name or peer id); empty: the first there is.
    std::optional<RenderQueueInfo> queue(const QString& nameOrPeer) const;
    // Every queue's jobs, in the order they were submitted there.
    std::vector<QueuedJobInfo> jobs() const;
    std::optional<QueuedJobInfo> job(const QString& key) const;

    // Sends `job` (made as for this instance) and its scene to the queue: the job is the
    // queue's from then on. False, with `error`, when there's no such queue or the scene
    // can't be packaged.
    bool submit(const RenderJobPtr& job, const QString& queue, QString* error = nullptr);
    // The jobs submit() sent, oldest first, with the queue's view of each merged in while
    // the queue is here, and the last one it gave once it's gone.
    std::vector<SentJobInfo> sentJobs() const;
    std::optional<SentJobInfo> sentJob(const QString& artistJob) const;
    // Cancels a sent job on its queue. False when it isn't one, or the queue hasn't
    // queued it yet (there's no id to name it by).
    bool cancelSent(const QString& artistJob);
    // A job of the map's; the queue says no (denied()) when it isn't this profile's to touch.
    void cancel(const QString& key);
    void retry(const QString& key);
    void remove(const QString& key);
    // Its tasks, from the queue: tasksReceived() once they come.
    void requestTasks(const QString& key);

Q_SIGNALS:
    void queuesChanged();
    void jobsChanged();
    void queued(const QString& artistJob, const QString& id, int position);
    void refused(const QString& artistJob, const QString& reason);
    void denied(const QString& id, const QString& reason);
    void ended(const QString& id, const QString& status);
    void tasksReceived(const QString& key, const QJsonArray& tasks);
    // A job's frames, fetched from a queue that keeps them, are in `folder`.
    void framesArrived(const QString& id, const QString& folder, const QStringList& files);

private:
    struct RoomLink;
    RoomLink* link(usdcc::peers::Room* room) const;
    RoomLink* linkOf(const QString& queuePeer) const;
    void onMessage(RoomLink& link, const usdcc::peers::Message& message);
    void onFrames(RoomLink& link, const QString& queuePeer, const QJsonObject& body);
    void sendAction(const QString& key, const QString& type);

    Options m_options;
    std::map<usdcc::peers::Room*, std::unique_ptr<RoomLink>> m_rooms;
    struct Fetch {
        QString id;
        QString folder;
        QString hash;
        QString queuePeer;
    };
    std::vector<Fetch> m_fetches;
    std::map<std::string, QString> m_bundles;  // artist job → bundle folder, until queued or refused
    std::vector<SentJobInfo> m_sent;           // what submit() sent, oldest first
    SentJobInfo* sent(const QString& queuePeer, const QString& artistJob, const QString& id);
};

}  // namespace usdcc::render
