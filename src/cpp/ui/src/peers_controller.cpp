#include "usdcc/ui/peers_controller.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/core/version.h"
#include "usdcc/peers/channel.h"
#include "usdcc/peers/state.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/toast_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolButton>

namespace usdcc::ui {

using namespace usdcc::peers;
using core::SettingType;

namespace {

constexpr int kHistoryLines = 1000;   // Kept per room, in memory (PEERS.md §10.1)
constexpr int kSnapshotLines = 200;   // Given to a joiner
constexpr int kToastTextLength = 300;
const QString kChat = QStringLiteral("chat");
const QString kPresence = QStringLiteral("presence");
const QString kMutedRoomsKey = QStringLiteral("Peers/mutedRooms");
const QString kMutedPeersKey = QStringLiteral("Peers/mutedPeers");
const QString kTrustedKey = QStringLiteral("Peers/trusted");

QString defaultName() {
    QString name = qEnvironmentVariable("USERNAME");
    if (name.isEmpty()) {
        name = qEnvironmentVariable("USER");
    }
    return name.isEmpty() ? QStringLiteral("usdcc") : name;
}

QString roomKey(Room* room) {
    return room->hostPeer().fingerprint + u'/' + room->name();
}

QString settingText(const char* key) {
    return QString::fromStdString(core::Settings::instance().get<std::string>(key));
}

}  // namespace

void PeersController::defineSettings() {
    auto& settings = core::Settings::instance();
    settings.define({.key = nameKey,
                     .category = "Peers",
                     .group = "You",
                     .label = "Name",
                     .description = "How you show in rooms you host or join from now on.",
                     .type = SettingType::String,
                     .defaultValue = defaultName().toStdString()});
    settings.define({.key = colorKey,
                     .category = "Peers",
                     .group = "You",
                     .label = "Color",
                     .description = "Your color in the people list and the chat.",
                     .type = SettingType::Color,
                     .defaultValue = std::string("#5ba3e0")});
    settings.define({.key = lookOnNetworkKey,
                     .category = "Peers",
                     .group = "Rooms",
                     .label = "Look for rooms on this network",
                     .description = "While the Peers panel shows, usdcc asks the network (mDNS) for rooms others host. "
                                    "Off, it lists only rooms on this machine.",
                     .type = SettingType::Bool,
                     .defaultValue = true});
    settings.define({.key = portKey,
                     .category = "Peers",
                     .group = "Rooms",
                     .label = "Port",
                     .description = "The port a room you host listens on; another free one if it is taken. The first "
                                    "time, Windows asks whether to let it through its firewall.",
                     .type = SettingType::Int,
                     .defaultValue = 47800,
                     .minimum = 1024,
                     .maximum = 65535});
    settings.define({.key = askBeforeLinkJoinKey,
                     .category = "Peers",
                     .group = "Rooms",
                     .label = "Ask before someone joins by link",
                     .description = "Otherwise the invite link is the invitation.",
                     .type = SettingType::Bool,
                     .defaultValue = false});
    settings.define({.key = chatToastsKey,
                     .category = "Peers",
                     .group = "Chat",
                     .label = "Show chat messages as toasts",
                     .description = "While the Peers panel is closed.",
                     .type = SettingType::Bool,
                     .defaultValue = true});
}

Identity peersIdentity() {
    static Identity identity;
    if (!identity.isValid()) {
        const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/peers";
        QString error;
        identity = Identity::loadOrCreate(QCoreApplication::applicationName(), folder, &error);
        if (!identity.isValid()) {
            // A key for this run only: rooms work, but nobody can "always allow" it.
            USDCC_LOG_WARNING("usdcc.peers", "{}; using a key for this run only", error.toStdString());
            identity = Identity::generate();
        }
    }
    return identity;
}

Profile peersProfile() {
    Profile profile;
    profile.name = settingText(PeersController::nameKey);
    profile.color = settingText(PeersController::colorKey);
    profile.app = QStringLiteral("usdcc ") + QString::fromLatin1(core::version());
    return profile;
}

QString peersRoomsFolder() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/peers/rooms";
}

PeersController::PeersController(std::unique_ptr<Peers> peers, MainWindow* window, Hooks hooks, QObject* parent)
    : QObject(parent), m_peers(std::move(peers)), m_window(window), m_hooks(std::move(hooks)) {
    connect(m_peers.get(), &Peers::roomAdded, this, [this](Room* room) { adopt(room); });
    connect(m_peers.get(), &Peers::roomRemoved, this, [this](Room* room) {
        m_chat.remove(room);
        updateStatus();
        Q_EMIT roomsChanged();
    });
    connect(m_peers.get(), &Peers::foundRoomsChanged, this, &PeersController::roomsChanged);
    onSettingsChanged(this, [this](const std::string& key) {
        if (key == nameKey || key == colorKey) {
            m_peers->setProfile(settingText(nameKey), settingText(colorKey));
            Q_EMIT roomsChanged();  // The panel shows the name
        } else if (key == lookOnNetworkKey) {
            setPanelShown(m_panelShown);
        }
    });
    if (m_window) {
        m_status = new QToolButton(m_window);
        m_status->setAutoRaise(true);
        m_status->setToolTip(tr("The rooms you are in: open the Peers panel"));
        m_status->hide();
        connect(m_status, &QToolButton::clicked, this, [this] {
            if (showPanel) {
                showPanel();
            }
        });
        m_window->statusBar()->addPermanentWidget(m_status);
    }
}

PeersController::~PeersController() = default;

Room* PeersController::host(const HostRequest& request, QString* error) {
    HostOptions options;
    options.name = request.name.trimmed().isEmpty() ? tr("%1's room").arg(m_peers->profile().name) : request.name.trimmed();
    options.address = QHostAddress::Any;
    options.port = quint16(core::Settings::instance().get<int>(portKey, 47800));
    options.admission = request.admission;
    options.password = request.password;
    options.askBeforeLinkJoin = core::Settings::instance().get<bool>(askBeforeLinkJoinKey, false);
    options.trustedFingerprints = storedList(kTrustedKey);
    return m_peers->host(options, error);
}

Room* PeersController::join(const FoundRoom& room, const QString& password) {
    return m_peers->join(room, password);
}

Room* PeersController::joinLinkOrAddress(const QString& text, const QString& password, QString* error) {
    const QString trimmed = text.trimmed();
    if (trimmed.startsWith(QStringLiteral("usdcc:"), Qt::CaseInsensitive)) {
        return m_peers->joinLink(trimmed, error);
    }
    // host:port: as if found, so the host is asked.
    const qsizetype colon = trimmed.lastIndexOf(u':');
    bool portOk = false;
    if (colon > 0) {
        trimmed.mid(colon + 1).toUShort(&portOk);
    }
    if (!portOk) {
        if (error) {
            *error = tr("Paste an invite link (usdcc://join/…), or an address and port (192.168.1.20:47800)");
        }
        return nullptr;
    }
    JoinOptions options;
    options.password = password;
    return m_peers->join(trimmed, options);
}

void PeersController::adopt(Room* room) {
    Channel* chat = room->channel(kChat);
    m_chat.insert(room, {});
    if (room->isHost()) {
        // A joiner gets the recent chat first (PEERS.md §10.1).
        chat->setSnapshotProvider([this, room] {
            QJsonArray lines;
            const QList<ChatLine> history = m_chat.value(room);
            for (qsizetype i = std::max<qsizetype>(0, history.size() - kSnapshotLines); i < history.size(); ++i) {
                const ChatLine& line = history[i];
                lines.append(QJsonObject{{"from", line.from.id},
                                         {"name", line.from.name},
                                         {"color", line.from.color},
                                         {"text", line.text},
                                         {"time", line.when.toString(Qt::ISODate)}});
            }
            return Channel::Snapshot{{{"lines", lines}}, {}};
        });
        connect(room, &Room::joinRequested, this, [this, room](JoinRequest* request) { askHost(request, room); });
    }
    connect(chat, &Channel::received, this, [this, room](const Message& message) {
        if (message.type == u"snapshot") {
            QList<ChatLine> lines;
            for (const QJsonValue& value : message.body.value("lines").toArray()) {
                const QJsonObject object = value.toObject();
                Peer from;
                from.id = object.value("from").toString();
                from.name = object.value("name").toString();
                from.color = object.value("color").toString();
                lines.append({from, object.value("text").toString(),
                              QDateTime::fromString(object.value("time").toString(), Qt::ISODate)});
            }
            m_chat[room] = lines + m_chat.value(room);
            Q_EMIT chatReset(room);
            return;
        }
        if (message.type != u"say") {
            return;
        }
        Peer from = room->peer(message.from);
        if (!from.isValid()) {
            from.id = message.from;
            from.name = tr("Someone");
        }
        const QString text = message.body.value("text").toString().left(4000);
        addLine(room, {from, text, QDateTime::currentDateTime()});
        const bool mine = from.id == room->me().id;
        if (!mine && m_window && !m_panelShown && core::Settings::instance().get<bool>(chatToastsKey, true) &&
            !isMuted(room) && !isMuted(from)) {
            core::ToastRequest toast;
            toast.title = tr("%1 in %2").arg(from.name, room->name()).toStdString();
            toast.body = (text.size() > kToastTextLength ? text.left(kToastTextLength) + u'…' : text).toStdString();
            m_window->toasts()->show(toast, {{tr("Open Peers"), [this] {
                                                  if (showPanel) {
                                                      showPanel();
                                                  }
                                              }}});
        }
    });
    connect(room, &Room::opened, this, [this, room] {
        updatePresence();
        addLine(room, {{}, tr("You joined %1").arg(room->name()), QDateTime::currentDateTime()});
        updateStatus();
        Q_EMIT peopleChanged(room);
        Q_EMIT roomsChanged();
    });
    connect(room, &Room::peerJoined, this, [this, room](const Peer& peer) {
        addLine(room, {{}, tr("%1 joined").arg(peer.name), QDateTime::currentDateTime()});
        updateStatus();
        Q_EMIT peopleChanged(room);
    });
    connect(room, &Room::peerLeft, this, [this, room](const Peer& peer, const QString& reason) {
        addLine(room, {{}, reason.isEmpty() ? tr("%1 left").arg(peer.name) : tr("%1 left: %2").arg(peer.name, reason),
                       QDateTime::currentDateTime()});
        updateStatus();
        Q_EMIT peopleChanged(room);
    });
    connect(room, &Room::stateChanged, this, [this, room](Room::State state) {
        if (state == Room::State::Reconnecting) {
            addLine(room, {{}, tr("Lost the host; trying again…"), QDateTime::currentDateTime()});
        }
        Q_EMIT roomsChanged();
    });
    connect(room, &Room::resumed, this, [this, room](bool caughtUp) {
        addLine(room, {{}, caughtUp ? tr("Back in the room") : tr("Back in the room; messages in between are missing"),
                       QDateTime::currentDateTime()});
        updatePresence();
    });
    connect(room, &Room::closed, this, [this, room](const QString& reason) {
        if (!m_window || reason == u"Left the room" || reason == u"The host closed the room" ||
            reason == u"usdcc is closing") {
            return;
        }
        core::ToastRequest toast;
        toast.kind = core::ToastKind::Warning;
        const bool joined = !room->me().id.isEmpty();  // Set by the host's welcome
        toast.title = (joined ? tr("Left %1") : tr("Couldn't join %1"))
                          .arg(room->name().isEmpty() ? tr("the room") : room->name())
                          .toStdString();
        toast.body = reason.toStdString();
        m_window->toasts()->show(toast);
    });
    PeerState* presence = room->peerState(kPresence);
    connect(presence, &PeerState::changed, this, [this, room] { Q_EMIT peopleChanged(room); });
    if (room->isHost()) {
        updatePresence();
        addLine(room, {{}, tr("You host %1").arg(room->name()), QDateTime::currentDateTime()});
    }
    updateStatus();
    Q_EMIT roomsChanged();
}

void PeersController::addLine(Room* room, const ChatLine& line) {
    QList<ChatLine>& lines = m_chat[room];
    lines.append(line);
    while (lines.size() > kHistoryLines) {
        lines.removeFirst();
    }
    Q_EMIT chatAdded(room, line);
}

void PeersController::say(Room* room, const QString& text) {
    const QString trimmed = text.trimmed();
    if (!trimmed.isEmpty() && room && room->state() == Room::State::Open) {
        // It comes back from the host in the room's order, and is shown then.
        room->channel(kChat)->send(QStringLiteral("say"), {{"text", trimmed}});
    }
}

QString PeersController::stageOf(Room* room, const Peer& peer) const {
    PeerState* presence = room->peerState(kPresence);
    return presence ? presence->value(peer.id).toObject().value("stage").toString() : QString();
}

void PeersController::askHost(JoinRequest* request, Room* room) {
    const Peer peer = request->peer();
    if (!m_window) {
        return;  // Unanswered, it is denied in time
    }
    core::ToastRequest toast = core::ToastRequest::sticky();
    toast.title = tr("%1 wants to join %2").arg(peer.name, room->name()).toStdString();
    toast.body = tr("Key %1 · %2").arg(shortFingerprint(peer.fingerprint), peer.app).toStdString();
    QPointer<JoinRequest> guard = request;
    const int id = m_window->toasts()->show(
        toast, {{tr("Allow"), [guard] {
                     if (guard) {
                         guard->allow();
                     }
                 }},
                {tr("Always Allow %1").arg(peer.name), [this, guard, peer] {
                     trust(peer);
                     if (guard) {
                         guard->allow();
                     }
                 }},
                {tr("Deny"), [guard] {
                     if (guard) {
                         guard->deny();
                     }
                 }}});
    QPointer<MainWindow> window = m_window;
    connect(request, &JoinRequest::cancelled, this, [window, id] {
        if (window) {
            window->toasts()->dismiss(id);
        }
    });
}

void PeersController::trust(const Peer& peer) {
    QStringList trusted = storedList(kTrustedKey);
    if (!trusted.contains(peer.fingerprint)) {
        trusted.append(peer.fingerprint);
        storeList(kTrustedKey, trusted);
    }
    for (Room* room : m_peers->rooms()) {
        if (room->isHost()) {
            room->trust(peer.fingerprint);
        }
    }
}

bool PeersController::isMuted(Room* room) const {
    return storedList(kMutedRoomsKey).contains(roomKey(room));
}

void PeersController::setMuted(Room* room, bool muted) {
    QStringList list = storedList(kMutedRoomsKey);
    list.removeAll(roomKey(room));
    if (muted) {
        list.append(roomKey(room));
    }
    storeList(kMutedRoomsKey, list);
}

bool PeersController::isMuted(const Peer& peer) const {
    return !peer.fingerprint.isEmpty() && storedList(kMutedPeersKey).contains(peer.fingerprint);
}

void PeersController::setMuted(const Peer& peer, bool muted) {
    QStringList list = storedList(kMutedPeersKey);
    list.removeAll(peer.fingerprint);
    if (muted && !peer.fingerprint.isEmpty()) {
        list.append(peer.fingerprint);
    }
    storeList(kMutedPeersKey, list);
}

void PeersController::setPanelShown(bool shown) {
    m_panelShown = shown;
    m_peers->setLooking(shown && core::Settings::instance().get<bool>(lookOnNetworkKey, true));
}

void PeersController::updatePresence() {
    const QString stage = m_hooks.currentStage ? m_hooks.currentStage() : QString();
    for (Room* room : m_peers->rooms()) {
        if (room->state() == Room::State::Open) {
            room->peerState(kPresence)->set(QJsonObject{{"stage", stage}});
        }
    }
}

void PeersController::updateStatus() {
    if (!m_status) {
        return;
    }
    const QList<Room*> rooms = m_peers->rooms();
    if (rooms.isEmpty()) {
        m_status->hide();
        return;
    }
    int people = 0;
    for (Room* room : rooms) {
        people += int(room->peers().size());
    }
    m_status->setText(rooms.size() == 1 ? tr("Peers: %1 · %2").arg(rooms.first()->name()).arg(people)
                                        : tr("Peers: %1 rooms · %2").arg(rooms.size()).arg(people));
    m_status->show();
}

QStringList PeersController::storedList(const QString& key) const {
    return appSettings().value(key).toStringList();
}

void PeersController::storeList(const QString& key, const QStringList& list) const {
    QSettings settings = appSettings();
    settings.setValue(key, list);
}

}  // namespace usdcc::ui
