#pragma once

// Shared stages in the app (docs/SHARED_STAGE.md): the stages shared in each chat room,
// and the ones this instance takes part in.
//
// - The room's host shares an open stage. It gets a live layer, the strongest sublayer
//   of its session layer, which is the edit target while shared (§2). A StageSync keeps
//   it equal on everyone's stage over the room's channels:
//     stage/<id>        ordered: the batches, the check, snapshots for joiners
//     stage/<id>/drag   latest only: a drag's values while it moves
//     stage/<id>/files  transfers: a large snapshot; the stage's files (phase C)
// - The room's map `stages` says what is shared: a SharedStageInfo per id, with the
//   layers below the live layer and their digests, so a joiner can tell whether its
//   files are the host's (§3).
// - A member opens a shared stage from that: the same file, a file it locates, or the
//   root layer's text for a stage that is only in memory. It gets its own live layer.
// - Ending (§9): the host merges the live layer into a layer of the stage, keeps it
//   beside the stage as <name>.live.usda, or discards it. A member who leaves, or whose
//   host stopped, keeps its copy as an ordinary session sublayer, or discards it.
// - Presence (§8): each member's selection, camera and what it drags, in the per-peer
//   state stage/<id>/presence (phase B).

#include "usdcc/core/export.h"
#include "usdcc/core/signal.h"
#include "usdcc/peers/types.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/layer.h>

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace usdcc::peers {
class Room;
}
namespace usdcc::ui {
class PeersController;
}

namespace usdcc::usd {
class StageManager;
class UndoManager;
}  // namespace usdcc::usd

namespace usdcc::usd::share {

class StageSync;

inline constexpr char kStagesMap[] = "stages";

// What the room's `stages` map says about one shared stage.
struct USDCC_USD_UI_API SharedStageInfo {
    struct Layer {
        QString identifier;
        QString digest;  // sha256 of the file, or of the layer's text when it isn't one
    };
    QString id;
    QString name;              // the root layer's display name
    QString root;              // the root layer's identifier on the host
    usdcc::peers::PeerId host;
    QString hostName;
    QString assetEnvironment;  // the host's (docs/ASSET_SOURCES.md §8); empty for none
    bool inMemory = false;     // no file: `rootText` is the root layer
    QString rootText;
    QList<Layer> layers;       // below the live layer, the root first
    QStringList watchers;      // members who may only watch

    QJsonObject toJson() const;
    static SharedStageInfo fromJson(const QString& id, const QJsonObject& json);
};

// The digest SharedStageInfo keeps for a layer.
USDCC_USD_UI_API QString layerFileDigest(const PXR_NS::SdfLayerHandle& layer);

class USDCC_USD_UI_API SharedStages : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        // A question with buttons; the index of the one chosen, -1 if dismissed.
        std::function<int(const QString& title, const QString& text, const QStringList& buttons)> ask;
        // Where this machine has a file of the shared stage: the host's path is `hostPath`.
        // Empty if not given.
        std::function<QString(const QString& title, const QString& hostPath)> locate;
        // Something the user should know, with buttons that act on it.
        std::function<void(const QString& title, const QString& text,
                           const QList<std::pair<QString, std::function<void()>>>& actions)>
            notify;
    };

    SharedStages(usdcc::ui::PeersController* peers, StageManager& stages, UndoManager& undo, Hooks hooks,
                 QObject* parent = nullptr);
    ~SharedStages() override;

    // The app's; null without one.
    static SharedStages* instance();
    static void setInstance(SharedStages* sharedStages);

    usdcc::ui::PeersController* peers() const { return m_peers; }
    StageManager& stageManager() const { return m_stages; }

    // What `room` shares.
    QList<SharedStageInfo> shared(usdcc::peers::Room* room) const;
    std::optional<SharedStageInfo> info(usdcc::peers::Room* room, const QString& id) const;
    // Whether this instance takes part in it, and with which of its stages.
    bool isOpen(usdcc::peers::Room* room, const QString& id) const;
    StageRefPtr stageOf(usdcc::peers::Room* room, const QString& id) const;
    StageSync* syncOf(usdcc::peers::Room* room, const QString& id) const;
    // Whether `stage` is shared in any room this instance is in, and where.
    bool isShared(const StageRefPtr& stage) const;
    usdcc::peers::Room* roomOf(const StageRefPtr& stage) const;
    QString idOf(const StageRefPtr& stage) const;

    // The room's host shares `stage`. False, with why in `error`, when it can't or the
    // user cancelled (error empty then).
    bool share(usdcc::peers::Room* room, const StageRefPtr& stage, QString* error = nullptr);
    // A member opens a shared stage.
    bool open(usdcc::peers::Room* room, const QString& id, QString* error = nullptr);

    // Phase C (§3): a member without the stage's files asks the host for them. They come
    // as a transfer (a stage's layers and assets, with paths relative to its root), and
    // the stage opens from them. filesProgress() says how far; changed() when it opened.
    bool requestFiles(usdcc::peers::Room* room, const QString& id, QString* error = nullptr);
    bool isFetching(usdcc::peers::Room* room, const QString& id) const;

    enum class Ending {
        MergeIntoRoot,    // into the root layer
        MergeIntoTarget,  // into the layer that was the edit target before sharing
        Keep,             // as <name>.live.usda beside the root layer (or in memory)
        Discard,
    };
    // The host stops sharing. False, with why, if merging or keeping failed (it is still shared then).
    bool stop(usdcc::peers::Room* room, const QString& id, Ending ending, QString* error = nullptr);
    // A member stops taking part: its copy of the live layer stays in its session layer, or goes.
    void leave(usdcc::peers::Room* room, const QString& id, bool keepCopy);

    // The host's: who may edit (§6). Everyone may, until the host says otherwise.
    bool mayEdit(usdcc::peers::Room* room, const QString& id, const usdcc::peers::PeerId& peer) const;
    void setMayEdit(usdcc::peers::Room* room, const QString& id, const usdcc::peers::PeerId& peer, bool may);

Q_SIGNALS:
    // What a room shares, or this instance's part in it, changed.
    void changed(usdcc::peers::Room* room);
    // A session started or ended, on this stage.
    void sessionsChanged();
    // Who is on a shared stage, and where they look, changed.
    void presenceChanged(usdcc::peers::Room* room);
    // A stage's files on their way here (requestFiles()).
    void filesProgress(usdcc::peers::Room* room, const QString& id, qint64 done, qint64 total);

private:
    struct Session;
    struct RoomLink;
    struct Fetch;

    void addRoom(usdcc::peers::Room* room);
    void removeRoom(usdcc::peers::Room* room);
    Session* session(usdcc::peers::Room* room, const QString& id) const;
    Session* session(const StageRefPtr& stage) const;
    // Live layer, edit target, sync and channels for a stage now shared or opened.
    Session* start(usdcc::peers::Room* room, const SharedStageInfo& info, const StageRefPtr& stage, bool host,
                   const PXR_NS::SdfLayerHandle& keptContent = {});
    // Takes the live layer out of the stage (or leaves it, unsynced) and forgets the session.
    void end(Session* s, bool keepLiveLayer);
    void endSessionsOf(usdcc::peers::Room* room, const QString& why);
    // Ends a member's session, keeping its copy, and tells the user, who may discard it.
    void endKeepingCopy(Session* s, const QString& title, const QString& text);
    void onStagesChanged(usdcc::peers::Room* room, const QString& id, const QJsonValue& value);
    void updateSenders(Session* s);
    // Presence (§8): this instance's, sent when it changed; the others', to the hub.
    void publishPresence(Session* s);
    void updatePresence(Session* s);
    // The stage to open for `info`, after checking its files are the host's (§3).
    // `fetch` says the user would rather have the host send the files.
    StageRefPtr findOrOpenStage(const SharedStageInfo& info, QString* error, bool* fetch = nullptr);
    // The host's: packs the stage's files once, and sends them to `to`.
    void sendFiles(Session* s, const usdcc::peers::PeerId& to);
    void onFilesArrived(Fetch* fetch, const QString& directory);
    bool saveFirst(const StageRefPtr& stage);
    bool merge(Session* s, const PXR_NS::SdfLayerHandle& into, QString* error);

    usdcc::ui::PeersController* m_peers = nullptr;
    StageManager& m_stages;
    UndoManager& m_undo;
    Hooks m_hooks;
    std::map<usdcc::peers::Room*, std::unique_ptr<RoomLink>> m_rooms;
    std::vector<std::unique_ptr<Session>> m_sessions;
    std::vector<PXR_NS::SdfLayerRefPtr> m_keptLayers;  // copies left in session layers
    std::vector<std::unique_ptr<Fetch>> m_fetches;
    std::map<QString, QString> m_packed;  // the host's: share id -> its files, packed
    usdcc::core::Signal<StageRefPtr>::Connection m_stageClosed;
};

}  // namespace usdcc::usd::share
