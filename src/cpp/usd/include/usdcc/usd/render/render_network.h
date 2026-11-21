#pragma once

// Distributed final renders (docs/FINAL_RENDER.md §7): usdcc instances in the same peers
// rooms render each other's tasks.
//
// - Every instance says in each room what it can render: the PeerState
//   org.usdcc.render/node, {renderers, node, busy, schemes}.
// - Each peer that is a render node becomes a worker of this instance's queue (PeerWorker).
//   A job that may use peers (usdcc:render:distribute) has its tasks sent to them as well as,
//   or instead of, this instance.
// - The first task of a job a node gets is preceded by an offer. The node takes it without
//   asking when this instance is a peer it trusts; otherwise it asks its user, or declines.
// - The scene: shared (the node opens the stage from the same paths, after checking it has
//   them) or packaged (a bundle of the flattened stage and its assets, sent as a transfer,
//   kept in the node's inbox by its hash).
// - A finished task's images come back as a transfer; progress and failures as messages.
//
// Messages on the Direct channel org.usdcc.render/jobs, to one member each:
//   offer {job, renderer, title, shared?}   accept {job, delivery}   decline {job, reason}
//   bundle {job, hash}   task {...}   cancel {job, index?}   end {job}
//   progress {job, index, fraction, state}   result {job, index, hash}   failed {job, index, error}
// Transfers on org.usdcc.render/files.

#include "usdcc/core/export.h"
#include "usdcc/peers/room.h"
#include "usdcc/peers/types.h"
#include "usdcc/usd/render/render_scheduler.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::peers {
class Channel;
class PeerState;
class Room;
class Transfer;
class TransferInbox;
}  // namespace usdcc::peers

namespace usdcc::render {

inline constexpr const char* kNodeState = "org.usdcc.render/node";
inline constexpr const char* kJobsChannel = "org.usdcc.render/jobs";
inline constexpr const char* kFilesChannel = "org.usdcc.render/files";

// What a peer says it can do.
struct USDCC_USD_UI_API RenderNode {
    struct Renderer {
        std::string id;
        std::string name;
        bool tiles = false;
    };
    QPointer<usdcc::peers::Room> room;
    usdcc::peers::Peer peer;
    bool node = false;  // takes jobs
    bool busy = false;
    std::vector<Renderer> renderers;
    QStringList schemes;

    bool canRender(const std::string& rendererId, bool tiles) const;
    static RenderNode fromJson(usdcc::peers::Room* room, const usdcc::peers::Peer& peer, const QJsonObject& json);
};

struct USDCC_USD_UI_API RenderNetworkOptions {
    QString folder;  // the inbox, bundles and results go under it
    // Whether this instance renders for peers (Settings > Final Render > Render for peers).
    std::function<bool()> isNode;
    // Peers whose jobs it takes without asking.
    std::function<bool(const usdcc::peers::Peer&)> trusted;
    // Asks the user about a job from a peer it doesn't trust; `answer` once. Without it, such
    // jobs are declined.
    std::function<void(const usdcc::peers::Peer& from, const QString& what, std::function<void(bool)> answer)> ask;
    // The queue whose workers the nodes become; usually RenderScheduler::instance().
    RenderScheduler* scheduler = nullptr;
};

class PeerWorker;

class USDCC_USD_UI_API RenderNetwork : public QObject {
    Q_OBJECT

public:
    explicit RenderNetwork(RenderNetworkOptions options, QObject* parent = nullptr);
    ~RenderNetwork() override;

    // The app's, for the Render Settings panel; null without one.
    static RenderNetwork* instance();
    static void setInstance(RenderNetwork* network);

    // A room this instance is in, and when it goes.
    void addRoom(usdcc::peers::Room* room);
    void removeRoom(usdcc::peers::Room* room);
    // Says again what this instance can do: the renderers or the node setting changed.
    void advertise();

    // The other members that say what they can render, in every room.
    std::vector<RenderNode> nodes() const;
    // Those that take jobs and have `rendererId` (and render tiles, if `tiles`).
    std::vector<RenderNode> nodesFor(const std::string& rendererId, bool tiles) const;

    // Tasks this instance renders for peers: running and waiting.
    int remoteTasks() const;

Q_SIGNALS:
    void nodesChanged();
    // This instance started or finished a peer's task.
    void remoteTasksChanged();

private:
    friend class PeerWorker;
    struct RoomLink;
    struct Outgoing;
    struct RemoteTask;
    struct Incoming;
    struct NodeTask;
    class RemoteHandle;

    RoomLink* link(usdcc::peers::Room* room) const;
    void onMessage(RoomLink& link, const usdcc::peers::Message& message);
    void onStateChanged(RoomLink& link, const usdcc::peers::Peer& peer);
    void onPeerLeft(RoomLink& link, const usdcc::peers::Peer& peer);
    void onBundleArrived(RoomLink& link, usdcc::peers::Transfer* transfer);
    bool acceptsTransfer(RoomLink& link, const usdcc::peers::Peer& from) const;

    // The submitting side.
    bool canRun(usdcc::peers::Room* room, const QString& peer, const RenderJob& job) const;
    std::unique_ptr<RenderTaskHandle> startRemote(usdcc::peers::Room* room, const QString& peer, const RenderJob& job,
                                                  const JobTask& task, TaskCallbacks callbacks);
    void onAccepted(RoomLink& link, const QString& peer, const QJsonObject& body);
    void onDeclined(RoomLink& link, const QString& peer, const QJsonObject& body);
    void onResult(RoomLink& link, const QString& peer, const QJsonObject& body);
    void deliverResult(RoomLink& link, std::shared_ptr<RemoteTask> task);
    void sendWaiting(RoomLink& link, const QString& peer, const std::string& jobId);
    void failWaiting(const QString& peer, const std::string& jobId, const std::string& error, bool declined);
    void cancelRemote(const std::string& key);
    void jobEnded(const QString& jobId);
    void syncWorkers(RoomLink& link);

    // The node's side.
    void onOffer(RoomLink& link, const usdcc::peers::Peer& from, const QJsonObject& body);
    void answerOffer(RoomLink& link, const usdcc::peers::Peer& from, const QJsonObject& offer, bool take);
    void onTask(RoomLink& link, const usdcc::peers::Peer& from, const QJsonObject& body);
    void onCancel(RoomLink& link, const usdcc::peers::Peer& from, const QJsonObject& body);
    void loadBundle(RoomLink& link, Incoming& incoming);
    void pump();
    void finishNodeTask(const std::map<std::string, FloatImage>& images, const std::string& error);

    RenderNetworkOptions m_options;
    std::map<usdcc::peers::Room*, std::unique_ptr<RoomLink>> m_rooms;

    // Submitting: by peer, then job.
    std::map<QString, std::map<std::string, std::unique_ptr<Outgoing>>> m_outgoing;
    std::map<std::string, std::shared_ptr<RemoteTask>> m_remote;  // by "job#index"
    std::map<std::string, PXR_NS::UsdStageRefPtr> m_jobStages;    // what a job's bundle is made from
    std::map<std::string, QString> m_bundles;                     // job → bundle folder
    QSet<QString> m_arrivedResults;                               // result hashes in before their message

    // Rendering for peers: by peer id + job.
    std::map<QString, std::unique_ptr<Incoming>> m_incoming;
    std::deque<std::unique_ptr<NodeTask>> m_nodeQueue;
    std::unique_ptr<NodeTask> m_nodeRunning;
    unsigned m_nodeSerial = 0;
    bool m_advertising = false;
};

// A peer that is a render node, as a worker of this instance's queue.
class USDCC_USD_UI_API PeerWorker : public Worker {
public:
    PeerWorker(RenderNetwork* network, usdcc::peers::Room* room, usdcc::peers::Peer peer);
    std::string name() const override;
    bool canRun(const RenderJob& job) const override;
    std::unique_ptr<RenderTaskHandle> start(const RenderJob& job, const JobTask& task, TaskCallbacks callbacks) override;
    const usdcc::peers::Peer& peer() const { return m_peer; }
    usdcc::peers::Room* room() const { return m_room; }

private:
    QPointer<RenderNetwork> m_network;
    QPointer<usdcc::peers::Room> m_room;
    usdcc::peers::Peer m_peer;
};

// A task as it travels, and back; images as they travel (qCompress of width, height and floats).
USDCC_USD_UI_API QJsonObject taskToJson(const RenderTask& task);
USDCC_USD_UI_API RenderTask taskFromJson(const QJsonObject& json, const PXR_NS::UsdStageRefPtr& stage);
USDCC_USD_UI_API QByteArray packImage(const FloatImage& image);
USDCC_USD_UI_API std::optional<FloatImage> unpackImage(const QByteArray& bytes);

}  // namespace usdcc::render
