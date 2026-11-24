#include "usdcc/usd/share/shared_stages.h"

#include "usdcc/core/log.h"
#include "usdcc/peers/channel.h"
#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/peers/state.h"
#include "usdcc/peers/transfer.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/usd/asset_servers.h"
#include "usdcc/usd/share/live_layers.h"
#include "usdcc/usd/share/presence.h"
#include "usdcc/usd/share/stage_sync.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdUtils/localizeAsset.h>
#include <pxr/usd/usdUtils/stitch.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>

#include <algorithm>

using namespace PXR_NS;
using usdcc::peers::Channel;
using usdcc::peers::Delivery;
using usdcc::peers::Message;
using usdcc::peers::Peer;
using usdcc::peers::PeerId;
using usdcc::peers::Room;
using usdcc::peers::RoomType;
using usdcc::peers::StateMap;
using usdcc::peers::Transfer;
using usdcc::peers::TransferInbox;
using usdcc::peers::TransferManifest;

namespace usdcc::usd::share {

namespace {

constexpr auto kLog = "usdcc.share";
SharedStages* s_instance = nullptr;

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString channelName(const QString& id, const char* suffix = "") {
    return QStringLiteral("stage/") + id + QLatin1String(suffix);
}

// The layers a joiner needs, below the live layer: the root first, then the rest of
// what the stage uses that is a file.
QList<SharedStageInfo::Layer> stageLayers(const UsdStageRefPtr& stage, const SdfLayerHandle& live) {
    QList<SharedStageInfo::Layer> layers;
    const SdfLayerHandle root = stage->GetRootLayer();
    if (!root->IsAnonymous()) layers.append({qs(root->GetIdentifier()), layerFileDigest(root)});
    for (const SdfLayerHandle& layer : stage->GetUsedLayers(false)) {
        if (!layer || layer == root || layer == live || layer->IsAnonymous() || layer == stage->GetSessionLayer()) {
            continue;
        }
        layers.append({qs(layer->GetIdentifier()), layerFileDigest(layer)});
    }
    return layers;
}

// The host's asset environment, when it's known here; empty (the default) otherwise
// (docs/ASSET_SOURCES.md §8).
std::string environmentFor(const SharedStageInfo& info) {
    const std::string environment = info.assetEnvironment.toStdString();
    return hasAssetEnvironment(environment) ? environment : std::string();
}

// Says so when the stage here isn't in the host's environment. Nothing to say to someone
// without any: their paths on named servers don't resolve, which the Log says.
void warnOtherEnvironment(const SharedStageInfo& info, const StageRefPtr& stage) {
    const std::string mine = stageAssetEnvironment(stage->usdStage());
    if (info.assetEnvironment.isEmpty() || info.assetEnvironment.toStdString() == mine || assetEnvironments().empty()) return;
    USDCC_LOG_TOAST(usdcc::core::LogLevel::Warning, kLog,
                    "{} has {} in the asset environment {}, and you have it in {}: files on servers may differ.",
                    info.hostName.toStdString(), info.name.toStdString(), info.assetEnvironment.toStdString(),
                    mine.empty() ? std::string("none") : mine);
}

int subLayerIndex(const SdfLayerHandle& parent, const SdfLayerHandle& layer) {
    const std::vector<std::string> paths = parent->GetSubLayerPaths();
    for (size_t i = 0; i < paths.size(); ++i) {
        if (paths[i] == layer->GetIdentifier()) return int(i);
    }
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------------
// SharedStageInfo

QJsonObject SharedStageInfo::toJson() const {
    QJsonArray layerArray;
    for (const Layer& layer : layers) {
        layerArray.append(QJsonObject{{QStringLiteral("id"), layer.identifier}, {QStringLiteral("digest"), layer.digest}});
    }
    QJsonObject json{{QStringLiteral("name"), name},
                     {QStringLiteral("root"), root},
                     {QStringLiteral("host"), host},
                     {QStringLiteral("hostName"), hostName},
                     {QStringLiteral("layers"), layerArray},
                     {QStringLiteral("watchers"), QJsonArray::fromStringList(watchers)}};
    if (!assetEnvironment.isEmpty()) json.insert(QStringLiteral("assetEnvironment"), assetEnvironment);
    if (inMemory) {
        json.insert(QStringLiteral("inMemory"), true);
        json.insert(QStringLiteral("rootText"), rootText);
    }
    return json;
}

SharedStageInfo SharedStageInfo::fromJson(const QString& id, const QJsonObject& json) {
    SharedStageInfo info;
    info.id = id;
    info.name = json.value(QLatin1String("name")).toString();
    info.root = json.value(QLatin1String("root")).toString();
    info.host = json.value(QLatin1String("host")).toString();
    info.hostName = json.value(QLatin1String("hostName")).toString();
    info.assetEnvironment = json.value(QLatin1String("assetEnvironment")).toString();
    info.inMemory = json.value(QLatin1String("inMemory")).toBool();
    info.rootText = json.value(QLatin1String("rootText")).toString();
    for (const QJsonValue& value : json.value(QLatin1String("layers")).toArray()) {
        const QJsonObject layer = value.toObject();
        info.layers.append({layer.value(QLatin1String("id")).toString(), layer.value(QLatin1String("digest")).toString()});
    }
    for (const QJsonValue& value : json.value(QLatin1String("watchers")).toArray()) info.watchers << value.toString();
    return info;
}

QString layerFileDigest(const SdfLayerHandle& layer) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const std::string realPath = layer->GetRealPath();
    QFile file(qs(realPath));
    if (!realPath.empty() && file.open(QIODevice::ReadOnly)) {
        hash.addData(&file);
    } else {
        std::string text;
        layer->ExportToString(&text);
        hash.addData(QByteArray::fromStdString(text));
    }
    return QString::fromLatin1(hash.result().toHex());
}

// ---------------------------------------------------------------------------------
// Sessions and rooms

struct SharedStages::Session {
    QPointer<Room> room;
    SharedStageInfo info;
    StageRefPtr stage;
    SdfLayerRefPtr live;
    bool host = false;
    SdfLayerHandle targetBefore;
    std::unique_ptr<StageSync> sync;
    QPointer<Channel> channel;  // stage/<id>: ordered
    QPointer<Channel> drag;     // stage/<id>/drag: latest only
    QPointer<Channel> ask;      // stage/<id>/ask: a member's questions to the host
    std::unique_ptr<TransferInbox> inbox;
    QPointer<usdcc::peers::PeerState> presence;  // stage/<id>/presence
    QTimer* publish = nullptr;
    QJsonObject lastPresence;
    std::vector<QMetaObject::Connection> connections;
    usdcc::core::Signal<>::Connection editTarget;
    bool fixingTarget = false;
};

struct SharedStages::Fetch {
    QPointer<Room> room;
    SharedStageInfo info;
    std::unique_ptr<TransferInbox> inbox;
    std::vector<QMetaObject::Connection> connections;
};

struct SharedStages::RoomLink {
    QPointer<Room> room;
    QPointer<StateMap> stages;
    std::vector<QMetaObject::Connection> connections;
};

SharedStages::SharedStages(usdcc::ui::PeersController* peers, StageManager& stages, UndoManager& undo, Hooks hooks,
                           QObject* parent)
    : QObject(parent), m_peers(peers), m_stages(stages), m_undo(undo), m_hooks(std::move(hooks)) {
    if (peers) {
        for (Room* room : peers->peers()->rooms(RoomType::Chat)) addRoom(room);
        connect(peers->peers(), &usdcc::peers::Peers::roomAdded, this, [this](Room* room) {
            if (room->type() == RoomType::Chat) addRoom(room);
        });
        connect(peers->peers(), &usdcc::peers::Peers::roomRemoved, this, &SharedStages::removeRoom);
    }
    // Following someone takes their frame at once, not at their next change.
    connect(&PresenceHub::instance(), &PresenceHub::followChanged, this, [this](const UsdStage* stage) {
        for (const auto& s : m_sessions) {
            if (get_pointer(s->stage->usdStage()) == stage) updatePresence(s.get());
        }
    });
    m_stageClosed = m_stages.stageClosed.connect([this](const StageRefPtr& stage) {
        Session* s = session(stage);
        if (!s) return;
        if (s->host && s->room) {
            // Closing the stage ends the share; what was done in it is kept if it can be.
            const bool canKeep = !s->stage->usdStage()->GetRootLayer()->IsAnonymous();
            stop(s->room, s->info.id, canKeep ? Ending::Keep : Ending::Discard);
        } else {
            end(s, false);
        }
    });
}

SharedStages::~SharedStages() {
    while (!m_sessions.empty()) end(m_sessions.back().get(), true);
    if (s_instance == this) s_instance = nullptr;
}

SharedStages* SharedStages::instance() { return s_instance; }
void SharedStages::setInstance(SharedStages* sharedStages) { s_instance = sharedStages; }

void SharedStages::addRoom(Room* room) {
    if (!room || m_rooms.count(room)) return;
    auto link = std::make_unique<RoomLink>();
    link->room = room;
    link->stages = room->state(QLatin1String(kStagesMap));
    if (link->stages) {
        // The host decides what is shared: only it writes the map.
        link->stages->setSenders(usdcc::peers::Senders::Host);
        link->connections.push_back(connect(link->stages, &StateMap::changed, this,
                                            [this, room](const QString& key, const QJsonValue& value, const Peer&) {
                                                onStagesChanged(room, key, value);
                                            }));
    }
    link->connections.push_back(connect(room, &Room::peerLeft, this, [this, room](const Peer& peer, const QString&) {
        std::vector<Session*> hostGone;
        for (const auto& s : m_sessions) {
            if (s->room != room) continue;
            if (s->sync) s->sync->peerLeft(peer.id);
            if (!s->host && peer.id == s->info.host) hostGone.push_back(s.get());
        }
        for (Session* s : hostGone) {
            endKeepingCopy(s, tr("%1 is no longer shared").arg(s->info.name),
                           tr("%1 left the room. Your copy of the shared edits stays in the stage's session layer.")
                               .arg(peer.name));
        }
    }));
    link->connections.push_back(connect(room, &Room::peerJoined, this, [this, room](const Peer&) {
        for (const auto& s : m_sessions) {
            if (s->room == room && s->host) updateSenders(s.get());
        }
    }));
    link->connections.push_back(connect(room, &Room::closed, this, [this, room](const QString& reason) {
        endSessionsOf(room, reason);
        removeRoom(room);
    }));
    m_rooms[room] = std::move(link);
    Q_EMIT changed(room);
}

void SharedStages::removeRoom(Room* room) {
    const auto it = m_rooms.find(room);
    if (it == m_rooms.end()) return;
    endSessionsOf(room, tr("The room closed"));
    for (auto f = m_fetches.begin(); f != m_fetches.end();) {
        if ((*f)->room == room || !(*f)->room) {
            for (const QMetaObject::Connection& c : (*f)->connections) disconnect(c);
            f = m_fetches.erase(f);
        } else {
            ++f;
        }
    }
    for (const QMetaObject::Connection& c : it->second->connections) disconnect(c);
    m_rooms.erase(it);
    Q_EMIT changed(room);
}

void SharedStages::endSessionsOf(Room* room, const QString& why) {
    std::vector<Session*> ending;
    for (const auto& s : m_sessions) {
        if (s->room == room || !s->room) ending.push_back(s.get());
    }
    for (Session* s : ending) {
        if (s->host) {
            end(s, false);
        } else {
            endKeepingCopy(s, tr("%1 is no longer shared").arg(s->info.name),
                           tr("%1. Your copy of the shared edits stays in the stage's session layer.").arg(why));
        }
    }
}

QList<SharedStageInfo> SharedStages::shared(Room* room) const {
    QList<SharedStageInfo> list;
    const auto it = m_rooms.find(room);
    if (it == m_rooms.end() || !it->second->stages) return list;
    const QJsonObject items = it->second->stages->items();
    for (auto item = items.begin(); item != items.end(); ++item) {
        list.append(SharedStageInfo::fromJson(item.key(), item.value().toObject()));
    }
    std::sort(list.begin(), list.end(), [](const SharedStageInfo& a, const SharedStageInfo& b) { return a.name < b.name; });
    return list;
}

std::optional<SharedStageInfo> SharedStages::info(Room* room, const QString& id) const {
    const auto it = m_rooms.find(room);
    if (it == m_rooms.end() || !it->second->stages || !it->second->stages->contains(id)) return std::nullopt;
    return SharedStageInfo::fromJson(id, it->second->stages->value(id).toObject());
}

SharedStages::Session* SharedStages::session(Room* room, const QString& id) const {
    for (const auto& s : m_sessions) {
        if (s->room == room && s->info.id == id) return s.get();
    }
    return nullptr;
}

SharedStages::Session* SharedStages::session(const StageRefPtr& stage) const {
    for (const auto& s : m_sessions) {
        if (s->stage == stage) return s.get();
    }
    return nullptr;
}

bool SharedStages::isOpen(Room* room, const QString& id) const { return session(room, id) != nullptr; }

StageRefPtr SharedStages::stageOf(Room* room, const QString& id) const {
    const Session* s = session(room, id);
    return s ? s->stage : StageRefPtr();
}

StageSync* SharedStages::syncOf(Room* room, const QString& id) const {
    const Session* s = session(room, id);
    return s ? s->sync.get() : nullptr;
}

bool SharedStages::isShared(const StageRefPtr& stage) const { return session(stage) != nullptr; }

Room* SharedStages::roomOf(const StageRefPtr& stage) const {
    const Session* s = session(stage);
    return s ? s->room.data() : nullptr;
}

QString SharedStages::idOf(const StageRefPtr& stage) const {
    const Session* s = session(stage);
    return s ? s->info.id : QString();
}

// ---------------------------------------------------------------------------------
// Sharing and opening

bool SharedStages::saveFirst(const StageRefPtr& stage) {
    std::vector<SdfLayerHandle> dirty;
    for (const SdfLayerHandle& layer : stage->usdStage()->GetUsedLayers(false)) {
        if (layer && !layer->IsAnonymous() && layer->IsDirty()) dirty.push_back(layer);
    }
    if (dirty.empty()) return true;
    QStringList names;
    for (const SdfLayerHandle& layer : dirty) names << qs(layer->GetDisplayName());
    const int answer =
        m_hooks.ask ? m_hooks.ask(tr("Share Stage"),
                                  tr("These layers have unsaved changes, which the others wouldn't have:\n\n%1\n\n"
                                     "Save them before sharing?")
                                      .arg(names.join(QLatin1Char('\n'))),
                                  {tr("Save and Share"), tr("Cancel")})
                    : 0;
    if (answer != 0) return false;
    for (const SdfLayerHandle& layer : dirty) {
        if (!layer->Save()) {
            USDCC_LOG_TOAST(usdcc::core::LogLevel::Error, kLog, "Couldn't save {}", layer->GetDisplayName());
            return false;
        }
    }
    return true;
}

bool SharedStages::share(Room* room, const StageRefPtr& stage, QString* error) {
    const auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (!room || !stage) return fail(tr("Nothing to share"));
    if (!room->isHost()) return fail(tr("Only the room's host can share a stage"));
    if (!m_rooms.count(room) || !m_rooms[room]->stages) return fail(tr("This room can't share stages"));
    if (isShared(stage)) return fail(tr("This stage is shared already"));
    if (!saveFirst(stage)) return fail({});

    const UsdStageRefPtr& usdStage = stage->usdStage();
    const SdfLayerHandle root = usdStage->GetRootLayer();
    SharedStageInfo info;
    info.id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    info.name = qs(root->GetDisplayName());
    info.root = qs(root->GetIdentifier());
    info.host = room->me().id;
    info.hostName = room->me().name;
    info.assetEnvironment = qs(stageAssetEnvironment(usdStage));
    info.inMemory = root->IsAnonymous();
    if (info.inMemory) {
        std::string text;
        root->ExportToString(&text);
        info.rootText = qs(text);
        info.name = tr("Untitled stage");
    }
    info.layers = stageLayers(usdStage, {});
    if (QJsonDocument(info.toJson()).toJson(QJsonDocument::Compact).size() > StateMap::kMaxValueBytes) {
        return fail(info.inMemory ? tr("The stage is too large to share from memory: save it to a file first")
                                  : tr("The stage uses too many layers to share"));
    }

    // Edits kept from an earlier share of this stage (§9).
    SdfLayerRefPtr kept;
    if (!info.inMemory) {
        const QFileInfo rootFile(qs(root->GetRealPath()));
        const QString keptPath = rootFile.dir().filePath(rootFile.completeBaseName() + QStringLiteral(".live.usda"));
        if (QFileInfo::exists(keptPath) && m_hooks.ask &&
            m_hooks.ask(tr("Share Stage"),
                        tr("The edits of an earlier share of %1 were kept in %2. Start from them?")
                            .arg(info.name, QFileInfo(keptPath).fileName()),
                        {tr("Start from Them"), tr("Start Afresh")}) == 0) {
            kept = SdfLayer::FindOrOpen(keptPath.toStdString());
        }
    }

    if (!start(room, info, stage, true, kept)) return fail(tr("Couldn't make the stage's live layer"));
    m_rooms[room]->stages->set(info.id, info.toJson());
    USDCC_LOG_INFO(kLog, "Sharing {} in {}", info.name.toStdString(), room->name().toStdString());
    Q_EMIT changed(room);
    return true;
}

StageRefPtr SharedStages::findOrOpenStage(const SharedStageInfo& info, QString* error, bool* fetch) {
    // The layers are looked up in it too.
    const std::string environment = environmentFor(info);
    const ArResolverContextBinder binder(stageResolverContext(info.root.toStdString(), environment));
    if (info.inMemory) {
        StageRefPtr stage = m_stages.adoptStage(
            UsdStage::CreateInMemory("tmp.usda", stageResolverContext(std::string(), environment)));
        if (stage && !stage->usdStage()->GetRootLayer()->ImportFromString(info.rootText.toStdString())) {
            m_stages.closeStage(stage);
            if (error) *error = tr("Couldn't read the shared stage's root layer");
            return {};
        }
        return stage;
    }
    const std::string rootId = info.root.toStdString();
    // Already open here.
    for (const StageRefPtr& stage : m_stages.stages()) {
        if (stage->usdStage()->GetRootLayer()->GetIdentifier() == rootId) return stage;
    }

    QString rootPath = info.root;
    SdfLayerRefPtr root = SdfLayer::FindOrOpen(rootId);
    if (!root) {
        // Not at the host's path: the user says where it is here (§3).
        const QString hostFile = QFileInfo(info.root).fileName();
        const int answer =
            m_hooks.ask ? m_hooks.ask(tr("Open Shared Stage"),
                                      tr("%1 shares %2, which is at\n%3\non the host's machine but not here.\n\n"
                                         "%1 can send you its files, or you can locate them on this machine.")
                                          .arg(info.hostName, info.name, info.root),
                                      {tr("Ask %1 for the Files").arg(info.hostName), tr("Locate…"), tr("Cancel")})
                        : -1;
        if (answer == 0 && fetch) {
            *fetch = true;
            if (error) error->clear();
            return {};
        }
        if (answer != 1 || !m_hooks.locate) {
            if (error) error->clear();
            return {};
        }
        rootPath = m_hooks.locate(tr("Locate %1").arg(hostFile), info.root);
        if (rootPath.isEmpty()) {
            if (error) error->clear();
            return {};
        }
        root = SdfLayer::FindOrOpen(rootPath.toStdString());
        if (!root) {
            if (error) *error = tr("Couldn't open %1").arg(rootPath);
            return {};
        }
    }

    // Are the files the host's? The root by its digest; the others by their path, or
    // their path relative to the root's folder when the root was elsewhere.
    QStringList differences;
    const QDir hostRootDir = QFileInfo(info.root).dir();
    const QDir rootDir = QFileInfo(qs(root->GetRealPath())).dir();
    for (int i = 0; i < info.layers.size(); ++i) {
        const SharedStageInfo::Layer& layer = info.layers[i];
        SdfLayerRefPtr here;
        if (i == 0) {
            here = root;
        } else {
            here = SdfLayer::FindOrOpen(layer.identifier.toStdString());
            if (!here && rootPath != info.root) {
                const QString relative = hostRootDir.relativeFilePath(layer.identifier);
                here = SdfLayer::FindOrOpen(rootDir.filePath(relative).toStdString());
            }
        }
        const QString name = QFileInfo(layer.identifier).fileName();
        if (!here) {
            differences << tr("%1 isn't here").arg(name);
        } else if (layerFileDigest(here) != layer.digest) {
            differences << tr("%1 is different").arg(name);
        }
        if (differences.size() > 12) {
            differences << tr("…");
            break;
        }
    }
    if (!differences.isEmpty()) {
        const int answer =
            m_hooks.ask ? m_hooks.ask(tr("Open Shared Stage"),
                                      tr("Your files of %1 aren't all the host's:\n\n%2\n\nWhat you see may differ from "
                                         "what the others see, though the shared edits stay the same.")
                                          .arg(info.name, differences.join(QLatin1Char('\n'))),
                                      {tr("Open Anyway"), tr("Cancel")})
                        : 0;
        if (answer != 0) {
            if (error) error->clear();
            return {};
        }
    }
    StageRefPtr stage =
        m_stages.adoptStage(StageManager::loadUsdStage(rootPath.toStdString(), nullptr, UsdStage::LoadAll, environment));
    if (!stage && error) *error = tr("Couldn't open %1").arg(rootPath);
    return stage;
}

bool SharedStages::open(Room* room, const QString& id, QString* error) {
    if (session(room, id)) return true;
    const std::optional<SharedStageInfo> found = info(room, id);
    if (!found) {
        if (error) *error = tr("That stage is no longer shared");
        return false;
    }
    if (found->host == room->me().id) {
        if (error) *error = tr("You share it");
        return false;
    }
    bool fetch = false;
    StageRefPtr stage = findOrOpenStage(*found, error, &fetch);
    if (fetch) return requestFiles(room, id, error);
    if (!stage) return false;
    if (isShared(stage)) {
        if (error) *error = tr("That stage takes part in another share already");
        return false;
    }
    if (!start(room, *found, stage, false)) {
        if (error) *error = tr("Couldn't make the stage's live layer");
        return false;
    }
    m_stages.setCurrentStage(stage);
    warnOtherEnvironment(*found, stage);
    Q_EMIT changed(room);
    return true;
}

SharedStages::Session* SharedStages::start(Room* room, const SharedStageInfo& info, const StageRefPtr& stage, bool host,
                                           const SdfLayerHandle& keptContent) {
    const UsdStageRefPtr& usdStage = stage->usdStage();
    auto s = std::make_unique<Session>();
    s->room = room;
    s->info = info;
    s->stage = stage;
    s->host = host;
    s->live = SdfLayer::CreateAnonymous("shared_" + info.id.toStdString() + ".usda");
    if (keptContent) s->live->TransferContent(keptContent);
    s->targetBefore = stage->editTargetLayer();

    // The strongest sublayer of the session layer, and the edit target (§2).
    const SdfLayerHandle session = usdStage->GetSessionLayer();
    session->InsertSubLayerPath(s->live->GetIdentifier(), 0);
    if (!stage->setEditTargetLayer(s->live)) {
        session->RemoveSubLayerPath(subLayerIndex(session, s->live));
        return nullptr;
    }
    registerLiveLayer(usdStage, s->live);
    Session* raw = s.get();
    s->editTarget = stage->editTargetChanged.connect([this, raw] {
        if (raw->fixingTarget) return;
        if (raw->stage->editTargetLayer() == raw->live) return;
        raw->fixingTarget = true;
        raw->stage->setEditTargetLayer(raw->live);
        raw->fixingTarget = false;
        USDCC_LOG_TOAST(usdcc::core::LogLevel::Info, kLog,
                        "While {} is shared, edits go to its live layer, the one everyone shares",
                        raw->info.name.toStdString());
    });

    s->channel = room->channel(channelName(info.id), Delivery::Ordered);
    s->drag = room->channel(channelName(info.id, "/drag"), Delivery::LatestOnly);
    s->ask = room->channel(channelName(info.id, "/ask"), Delivery::Direct);
    const QString filesChannel = channelName(info.id, "/files");

    StageSyncOptions options;
    options.live = s->live;
    options.me = room->me().id;
    options.host = host;
    QPointer<Channel> channel = s->channel, drag = s->drag, ask = s->ask;
    QPointer<Room> roomPtr = room;
    options.sendOrdered = [channel](const QString& type, const QJsonObject& body, const QByteArray& bytes) {
        if (channel) channel->send(type, body, bytes);
    };
    options.sendLatest = [drag](const QString& type, const QJsonObject& body, const QByteArray& bytes) {
        if (drag) drag->send(type, body, bytes);
    };
    options.sendDirect = [channel, ask, host](const PeerId& to, const QString& type, const QJsonObject& body,
                                              const QByteArray& bytes) {
        // The host answers on the stage's own channel; a member asks on its own, which
        // a member that may only watch can still send on.
        if (host) {
            if (channel) channel->send(type, body, bytes, to);
        } else if (ask) {
            ask->send(type, body, bytes);
        }
    };
    options.sendFile = [roomPtr, filesChannel, raw](const PeerId& to, const QString& path, const QString& id) {
        if (!roomPtr || !raw->sync) return;
        const QFileInfo file(path);
        QString error;
        std::optional<TransferManifest> manifest =
            TransferManifest::fromFiles(file.absolutePath(), {file.fileName()}, &error);
        if (manifest) {
            // Named by the snapshot's id, which is how the joiner knows it.
            manifest->files[0].path = id + QStringLiteral(".usdc");
            Transfer::send(roomPtr, filesChannel, to, *manifest, {}, raw->sync.get(), &error);
        }
        if (!error.isEmpty()) USDCC_LOG_WARNING(kLog, "Couldn't send the snapshot: {}", error.toStdString());
    };
    options.editing = [this, stage] { return m_undo.isEditing(stage); };
    s->sync = std::make_unique<StageSync>(options);
    StageSync* sync = s->sync.get();

    s->connections.push_back(connect(s->channel, &Channel::received, sync, &StageSync::receive));
    s->connections.push_back(connect(s->drag, &Channel::received, sync, &StageSync::receive));
    s->connections.push_back(connect(s->ask, &Channel::received, sync, [this, raw, sync, host](const Message& m) {
        if (!host) return;
        if (m.type == QLatin1String("files")) {
            sendFiles(raw, m.from);
        } else {
            sync->receive(m);
        }
    }));
    s->connections.push_back(connect(s->channel, &Channel::refused, sync, &StageSync::refused));
    s->connections.push_back(connect(s->drag, &Channel::refused, sync, &StageSync::refused));
    s->connections.push_back(connect(sync, &StageSync::editsRefused, this, [raw](const QString& reason) {
        USDCC_LOG_TOAST(usdcc::core::LogLevel::Warning, kLog, "Your edit to {} was taken back: {}",
                        raw->info.name.toStdString(), reason.toStdString());
    }));
    s->connections.push_back(connect(sync, &StageSync::failed, this, [raw](const QString& why) {
        USDCC_LOG_TOAST(usdcc::core::LogLevel::Warning, kLog, "{}: {}", raw->info.name.toStdString(), why.toStdString());
    }));
    if (host) {
        s->channel->setSnapshotProvider([sync] {
            Channel::Snapshot snapshot;
            snapshot.body = sync->snapshotBody(&snapshot.bytes);
            return snapshot;
        });
        updateSenders(raw);
    } else {
        // The inbox the stage's files came through, if they did: one per channel.
        const auto fetch = std::find_if(m_fetches.begin(), m_fetches.end(), [&](const auto& f) {
            return f->room == room && f->info.id == info.id;
        });
        if (fetch != m_fetches.end()) {
            for (const QMetaObject::Connection& c : (*fetch)->connections) disconnect(c);
            s->inbox = std::move((*fetch)->inbox);
            m_fetches.erase(fetch);
        } else {
            usdcc::peers::TransferInboxOptions inboxOptions;
            const PeerId hostId = info.host;
            inboxOptions.accept = [hostId](const Peer& from, const TransferManifest&) { return from.id == hostId; };
            s->inbox = std::make_unique<TransferInbox>(room, filesChannel, inboxOptions);
        }
        s->connections.push_back(connect(s->inbox.get(), &TransferInbox::received, sync, [sync](Transfer* transfer) {
            for (const usdcc::peers::TransferFile& file : transfer->manifest().files) {
                if (file.path.endsWith(QLatin1String(".usdc")) && file.path.startsWith(QLatin1String("snapshot-"))) {
                    sync->fileArrived(file.path.chopped(5), QDir(transfer->directory()).filePath(file.path));
                }
            }
        }));
    }
    // Presence: a few times a second at most, and only what changed.
    s->presence = room->peerState(channelName(info.id, "/presence"));
    if (s->presence) {
        s->connections.push_back(connect(s->presence, &usdcc::peers::PeerState::changed, this,
                                         [this, raw](const Peer&, const QJsonValue&) { updatePresence(raw); }));
    }
    s->publish = new QTimer(sync);
    s->publish->setInterval(150);
    connect(s->publish, &QTimer::timeout, this, [this, raw] { publishPresence(raw); });
    s->publish->start();
    m_sessions.push_back(std::move(s));
    updatePresence(raw);
    if (!host) sync->requestSnapshot();
    Q_EMIT sessionsChanged();
    return raw;
}

void SharedStages::updateSenders(Session* s) {
    if (!s->host || !s->room || !s->channel) return;
    if (s->info.watchers.isEmpty()) {
        s->channel->setSenders(usdcc::peers::Senders::Everyone);
        s->drag->setSenders(usdcc::peers::Senders::Everyone);
        return;
    }
    QList<PeerId> editors;
    for (const Peer& peer : s->room->peers()) {
        if (!s->info.watchers.contains(peer.id)) editors << peer.id;
    }
    s->channel->setNamedSenders(editors);
    s->drag->setNamedSenders(editors);
}

void SharedStages::end(Session* s, bool keepLiveLayer) {
    const auto it = std::find_if(m_sessions.begin(), m_sessions.end(), [s](const auto& p) { return p.get() == s; });
    if (it == m_sessions.end()) return;
    std::unique_ptr<Session> owned = std::move(*it);
    m_sessions.erase(it);
    Room* room = owned->room;
    for (const QMetaObject::Connection& c : owned->connections) disconnect(c);
    owned->editTarget.disconnect();
    if (owned->channel && owned->host) owned->channel->setSnapshotProvider({});
    if (owned->presence) owned->presence->set(QJsonValue());
    PresenceHub::instance().clear(get_pointer(owned->stage->usdStage()));
    owned->inbox.reset();
    owned->sync.reset();
    const UsdStageRefPtr& usdStage = owned->stage->usdStage();
    unregisterLiveLayer(usdStage);
    if (!keepLiveLayer) {
        const SdfLayerHandle session = usdStage->GetSessionLayer();
        const int index = subLayerIndex(session, owned->live);
        if (index >= 0) session->RemoveSubLayerPath(index);
    } else {
        // An ordinary session sublayer from now on, which the stage holds on to.
        m_keptLayers.push_back(owned->live);
    }
    const SdfLayerHandle back = owned->targetBefore ? owned->targetBefore : usdStage->GetRootLayer();
    if (!owned->stage->setEditTargetLayer(back)) owned->stage->setEditTargetLayer(usdStage->GetRootLayer());
    Q_EMIT sessionsChanged();
    if (room) Q_EMIT changed(room);
}

void SharedStages::endKeepingCopy(Session* s, const QString& title, const QString& text) {
    const StageRefPtr stage = s->stage;
    const SdfLayerRefPtr live = s->live;
    end(s, true);
    if (!m_hooks.notify) return;
    QPointer<SharedStages> self = this;
    m_hooks.notify(title, text,
                   {{tr("Discard Copy"), [self, stage, live] {
                         if (!self) return;
                         const SdfLayerHandle session = stage->usdStage()->GetSessionLayer();
                         const int index = subLayerIndex(session, live);
                         if (index >= 0) session->RemoveSubLayerPath(index);
                         auto& kept = self->m_keptLayers;
                         kept.erase(std::remove(kept.begin(), kept.end(), live), kept.end());
                     }}});
}

void SharedStages::onStagesChanged(Room* room, const QString& id, const QJsonValue& value) {
    Session* s = session(room, id);
    if (s && !s->host) {
        if (value.isUndefined()) {
            endKeepingCopy(s, tr("%1 is no longer shared").arg(s->info.name),
                           tr("The host stopped sharing it. Your copy of the shared edits stays in the stage's "
                              "session layer."));
        } else {
            s->info = SharedStageInfo::fromJson(id, value.toObject());
        }
    }
    Q_EMIT changed(room);
}

// ---------------------------------------------------------------------------------
// Ending

bool SharedStages::merge(Session* s, const SdfLayerHandle& into, QString* error) {
    if (!into) {
        if (error) *error = tr("There is no layer to merge into");
        return false;
    }
    const SdfLayerRefPtr live = s->live;
    return m_undo.apply(s->stage, into, tr("Merge Shared Edits"), [&] {
        // The live layer's opinions win over the target's own, as they did on the stage.
        SdfLayerRefPtr merged = SdfLayer::CreateAnonymous("merged.usda");
        merged->TransferContent(live);
        UsdUtilsStitchLayers(merged, into);
        into->TransferContent(merged);
        return true;
    });
}

bool SharedStages::stop(Room* room, const QString& id, Ending ending, QString* error) {
    Session* s = session(room, id);
    if (!s || !s->host) {
        if (error) *error = tr("You don't share that stage");
        return false;
    }
    s->sync->flush();
    const UsdStageRefPtr& usdStage = s->stage->usdStage();
    const SdfLayerHandle root = usdStage->GetRootLayer();
    switch (ending) {
    case Ending::MergeIntoRoot:
        if (!merge(s, root, error)) return false;
        break;
    case Ending::MergeIntoTarget:
        if (!merge(s, s->targetBefore ? s->targetBefore : root, error)) return false;
        break;
    case Ending::Keep: {
        if (root->IsAnonymous()) {
            if (error) *error = tr("The stage isn't saved, so there is nowhere to keep its shared edits beside it");
            return false;
        }
        const QFileInfo rootFile(qs(root->GetRealPath()));
        const QString path = rootFile.dir().filePath(rootFile.completeBaseName() + QStringLiteral(".live.usda"));
        if (!s->live->Export(path.toStdString())) {
            if (error) *error = tr("Couldn't write %1").arg(path);
            return false;
        }
        USDCC_LOG_TOAST(usdcc::core::LogLevel::Info, kLog, "The shared edits of {} are kept in {}",
                        s->info.name.toStdString(), path.toStdString());
        break;
    }
    case Ending::Discard: break;
    }
    if (m_rooms.count(room) && m_rooms[room]->stages) m_rooms[room]->stages->remove(id);
    end(s, false);
    if (const auto packed = m_packed.find(id); packed != m_packed.end()) {
        QDir(packed->second).removeRecursively();
        m_packed.erase(packed);
    }
    return true;
}

void SharedStages::leave(Room* room, const QString& id, bool keepCopy) {
    Session* s = session(room, id);
    if (!s || s->host) return;
    end(s, keepCopy);
}

void SharedStages::publishPresence(Session* s) {
    if (!s->presence || !s->room || s->room->state() != Room::State::Open) return;
    const UsdStage* usdStage = get_pointer(s->stage->usdStage());
    QJsonArray selection;
    const std::vector<SdfPath>& paths = s->stage->selectedPaths();
    for (size_t i = 0; i < paths.size() && i < 200; ++i) selection.append(qs(paths[i].GetString()));
    QJsonObject json{{QStringLiteral("selection"), selection}, {QStringLiteral("time"), s->stage->time()}};
    if (const std::optional<PeerView> view = PresenceHub::instance().localView(usdStage)) {
        json.insert(QStringLiteral("view"), view->toJson());
    }
    if (m_undo.isEditing(s->stage) && !paths.empty()) json.insert(QStringLiteral("dragging"), qs(paths.front().GetString()));
    if (json == s->lastPresence) return;
    s->lastPresence = json;
    s->presence->set(json);
}

void SharedStages::updatePresence(Session* s) {
    if (!s->presence || !s->room) return;
    const UsdStage* usdStage = get_pointer(s->stage->usdStage());
    std::vector<PeerPresence> peers;
    const QHash<PeerId, QJsonValue> values = s->presence->values();
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key() == s->room->me().id || !it.value().isObject()) continue;
        const Peer peer = s->room->peer(it.key());
        if (!peer.isValid()) continue;
        const QJsonObject json = it.value().toObject();
        PeerPresence presence;
        presence.id = peer.id;
        presence.name = peer.name;
        presence.color = QColor(peer.color);
        for (const QJsonValue& path : json.value(QLatin1String("selection")).toArray()) {
            const std::string text = path.toString().toStdString();
            if (SdfPath::IsValidPathString(text)) presence.selection.emplace_back(text);
        }
        presence.view = PeerView::fromJson(json.value(QLatin1String("view")).toObject());
        if (json.contains(QLatin1String("time"))) presence.time = json.value(QLatin1String("time")).toDouble();
        const std::string dragging = json.value(QLatin1String("dragging")).toString().toStdString();
        if (SdfPath::IsValidPathString(dragging)) presence.dragging = SdfPath(dragging);
        peers.push_back(std::move(presence));
    }
    std::sort(peers.begin(), peers.end(), [](const PeerPresence& a, const PeerPresence& b) { return a.name < b.name; });
    PresenceHub& hub = PresenceHub::instance();
    // Follow takes the peer's frame too.
    const PeerId followed = hub.following(usdStage);
    for (const PeerPresence& peer : peers) {
        if (peer.id == followed && peer.time && *peer.time != s->stage->time()) s->stage->setTime(*peer.time);
    }
    if (!followed.isEmpty() &&
        std::none_of(peers.begin(), peers.end(), [&](const PeerPresence& p) { return p.id == followed; })) {
        hub.stopFollowing(usdStage);  // gone
    }
    hub.setPeers(usdStage, std::move(peers));
    if (s->room) Q_EMIT presenceChanged(s->room);
}

bool SharedStages::isFetching(Room* room, const QString& id) const {
    return std::any_of(m_fetches.begin(), m_fetches.end(),
                       [&](const auto& f) { return f->room == room && f->info.id == id; });
}

bool SharedStages::requestFiles(Room* room, const QString& id, QString* error) {
    const std::optional<SharedStageInfo> found = info(room, id);
    if (!found || found->inMemory) {
        if (error) *error = tr("That stage is no longer shared");
        return false;
    }
    if (isFetching(room, id)) return true;
    auto fetch = std::make_unique<Fetch>();
    fetch->room = room;
    fetch->info = *found;
    usdcc::peers::TransferInboxOptions options;
    const PeerId hostId = found->host;
    options.accept = [hostId](const Peer& from, const TransferManifest&) { return from.id == hostId; };
    fetch->inbox = std::make_unique<TransferInbox>(room, channelName(id, "/files"), options);
    Fetch* raw = fetch.get();
    fetch->connections.push_back(connect(fetch->inbox.get(), &TransferInbox::started, this, [this, raw](Transfer* t) {
        connect(t, &Transfer::progress, this, [this, raw](qint64 done, qint64 total) {
            if (raw->room) Q_EMIT filesProgress(raw->room, raw->info.id, done, total);
        });
    }));
    fetch->connections.push_back(connect(fetch->inbox.get(), &TransferInbox::received, this, [this, raw](Transfer* t) {
        onFilesArrived(raw, t->directory());
    }));
    // The host says so if it couldn't pack them.
    Channel* channel = room->channel(channelName(id), Delivery::Ordered);
    fetch->connections.push_back(connect(channel, &Channel::received, this, [this, raw](const Message& m) {
        if (m.type != QLatin1String("files-failed")) return;
        const QString why = m.body.value(QLatin1String("error")).toString();
        const QString name = raw->info.name;
        const auto it = std::find_if(m_fetches.begin(), m_fetches.end(), [raw](const auto& f) { return f.get() == raw; });
        if (it != m_fetches.end()) {
            for (const QMetaObject::Connection& c : raw->connections) disconnect(c);
            m_fetches.erase(it);
        }
        if (m_hooks.notify) m_hooks.notify(tr("Couldn't get %1").arg(name), why, {});
    }));
    m_fetches.push_back(std::move(fetch));
    room->channel(channelName(id, "/ask"), Delivery::Direct)->send(QStringLiteral("files"));
    Q_EMIT changed(room);
    return true;
}

void SharedStages::sendFiles(Session* s, const PeerId& to) {
    const auto fail = [s, to](const QString& why) {
        USDCC_LOG_WARNING(kLog, "Couldn't send {}'s files: {}", s->info.name.toStdString(), why.toStdString());
        if (s->channel) s->channel->send(QStringLiteral("files-failed"), {{QStringLiteral("error"), why}}, {}, to);
    };
    if (s->info.inMemory) return fail(tr("The stage isn't saved to a file"));
    QString& folder = m_packed[s->info.id];
    if (folder.isEmpty() || !QDir(folder).exists()) {
        // Packed once per share: every layer and asset the root needs, with paths
        // relative to it (UsdUtilsLocalizeAsset), as the root's own name in the folder.
        folder = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                     .filePath(QStringLiteral("shared/") + s->info.id);
        QDir(folder).removeRecursively();
        QDir().mkpath(folder);
        if (!UsdUtilsLocalizeAsset(SdfAssetPath(s->info.root.toStdString()), QDir::toNativeSeparators(folder).toStdString())) {
            QDir(folder).removeRecursively();
            m_packed.erase(s->info.id);
            return fail(tr("USD couldn't gather the stage's files"));
        }
    }
    QString error;
    std::optional<TransferManifest> manifest = TransferManifest::fromDirectory(folder, &error);
    if (!manifest) return fail(error);
    if (!Transfer::send(s->room, channelName(s->info.id, "/files"), to, *manifest, {}, s->sync.get(), &error)) {
        return fail(error);
    }
    USDCC_LOG_INFO(kLog, "Sending {}'s files ({} MB) to {}", s->info.name.toStdString(),
                   double(manifest->totalBytes()) / (1024.0 * 1024.0), s->room->peer(to).name.toStdString());
}

void SharedStages::onFilesArrived(Fetch* fetch, const QString& directory) {
    // The root, under its own name; a snapshot's bundle is no stage.
    const QString rootName = QFileInfo(fetch->info.root).fileName();
    QString rootPath = QDir(directory).filePath(rootName);
    if (!QFileInfo::exists(rootPath)) return;
    Room* room = fetch->room;
    const SharedStageInfo info = fetch->info;
    if (!room) return;
    StageRefPtr stage = m_stages.adoptStage(
        StageManager::loadUsdStage(rootPath.toStdString(), nullptr, UsdStage::LoadAll, environmentFor(info)));
    if (!stage) {
        if (m_hooks.notify) m_hooks.notify(tr("Couldn't open %1").arg(info.name), rootPath, {});
        return;
    }
    if (!start(room, info, stage, false)) {
        if (m_hooks.notify) m_hooks.notify(tr("Couldn't open %1").arg(info.name), tr("Couldn't make its live layer"), {});
        return;
    }
    m_stages.setCurrentStage(stage);
    warnOtherEnvironment(info, stage);
    Q_EMIT changed(room);
}

bool SharedStages::mayEdit(Room* room, const QString& id, const PeerId& peer) const {
    const std::optional<SharedStageInfo> found = info(room, id);
    return !found || !found->watchers.contains(peer);
}

void SharedStages::setMayEdit(Room* room, const QString& id, const PeerId& peer, bool may) {
    Session* s = session(room, id);
    if (!s || !s->host || peer == s->info.host) return;
    s->info.watchers.removeAll(peer);
    if (!may) s->info.watchers << peer;
    updateSenders(s);
    if (m_rooms.count(room) && m_rooms[room]->stages) m_rooms[room]->stages->set(id, s->info.toJson());
    Q_EMIT changed(room);
}

}  // namespace usdcc::usd::share
