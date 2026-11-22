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
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QStatusBar>
#include <QTimer>
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
const QString kTyping = QStringLiteral("typing");
const QString kDoNotDisturbKey = QStringLiteral("Peers/doNotDisturb");
constexpr int kTypingEveryMs = 2000;     // "Typing" is said at most this often
constexpr int kTypingShownMs = 5000;     // ... and shown this long after the last
constexpr int kAwayAfterSeconds = 5 * 60;
constexpr int kSharedTextLength = 4000;  // A shared log message's text, and its details
constexpr int kSharedBytes = 64 * 1024;  // All of one share, as JSON
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

QJsonArray recordsJson(const QList<PeersController::SharedLogRecord>& records) {
    QJsonArray array;
    for (const PeersController::SharedLogRecord& record : records) {
        QJsonObject object{{"time", record.time.toString(Qt::ISODateWithMs)},
                           {"level", record.level},
                           {"source", record.source},
                           {"category", record.category},
                           {"text", record.text}};
        if (!record.details.isEmpty()) {
            object.insert("details", record.details);
        }
        array.append(object);
    }
    return array;
}

QList<PeersController::SharedLogRecord> recordsFrom(const QJsonValue& value) {
    QList<PeersController::SharedLogRecord> records;
    for (const QJsonValue& item : value.toArray()) {
        if (records.size() >= PeersController::kMaxSharedRecords) {
            break;
        }
        const QJsonObject object = item.toObject();
        records.append({QDateTime::fromString(object.value("time").toString(), Qt::ISODateWithMs),
                        object.value("level").toString().left(16), object.value("source").toString().left(64),
                        object.value("category").toString().left(128),
                        object.value("text").toString().left(kSharedTextLength),
                        object.value("details").toString().left(kSharedTextLength)});
    }
    return records;
}

QJsonObject lineJson(const PeersController::ChatLine& line) {
    QJsonObject object{{"from", line.from.id},
                       {"name", line.from.name},
                       {"color", line.from.color},
                       {"text", line.text},
                       {"time", line.when.toString(Qt::ISODate)}};
    if (!line.records.isEmpty()) {
        object.insert("records", recordsJson(line.records));
    }
    return object;
}

QString typingKey(Room* room, const QString& with) {
    return QString::number(quintptr(room), 16) + u'/' + with;
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
        const QString folder = usdcc::ui::userDataFolder() + "/peers";
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
    return usdcc::ui::userDataFolder() + "/peers/rooms";
}

PeersController::PeersController(std::unique_ptr<Peers> peers, MainWindow* window, Hooks hooks, QObject* parent)
    : QObject(parent), m_peers(std::move(peers)), m_window(window), m_hooks(std::move(hooks)) {
    connect(m_peers.get(), &Peers::roomAdded, this, [this](Room* room) { adopt(room); });
    connect(m_peers.get(), &Peers::roomRemoved, this, [this](Room* room) {
        m_chat.remove(room);
        m_private.remove(room);
        m_typists.removeIf([room](const Typist& typist) { return typist.room == room; });
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
        // Away: no input in the app for a while, or the window minimized.
        m_lastInput = QDateTime::currentDateTimeUtc();
        qApp->installEventFilter(this);
        m_idleTimer = new QTimer(this);
        m_idleTimer->setInterval(15000);
        connect(m_idleTimer, &QTimer::timeout, this, [this] {
            setAway((m_window && m_window->isMinimized()) ||
                    m_lastInput.secsTo(QDateTime::currentDateTimeUtc()) >= kAwayAfterSeconds);
        });
        m_idleTimer->start();
    }
    m_typingTimer = new QTimer(this);
    m_typingTimer->setInterval(1000);
    connect(m_typingTimer, &QTimer::timeout, this, &PeersController::expireTypists);
}

bool PeersController::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::Wheel:
        m_lastInput = QDateTime::currentDateTimeUtc();
        if (m_away) {
            setAway(false);
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

PeersController::~PeersController() = default;

Room* PeersController::host(const HostRequest& request, QString* error) {
    HostOptions options;
    options.name = request.name.trimmed().isEmpty()
                       ? (request.type == RoomType::RenderQueue ? tr("%1's render queue") : tr("%1's room"))
                             .arg(m_peers->profile().name)
                       : request.name.trimmed();
    options.type = request.type;
    options.address = request.onNetwork ? QHostAddress(QHostAddress::Any) : QHostAddress(QHostAddress::LocalHost);
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

Room* PeersController::joinLinkOrAddress(const QString& text, const QString& password, QString* error, RoomType type) {
    const QString trimmed = text.trimmed();
    if (trimmed.startsWith(QStringLiteral("usdcc:"), Qt::CaseInsensitive)) {
        return m_peers->joinLink(trimmed, error, type);
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
    options.type = type;
    return m_peers->join(trimmed, options);
}

QList<Room*> PeersController::chatRooms() const {
    return m_peers->rooms(RoomType::Chat);
}

void PeersController::adopt(Room* room) {
    // Every room this instance hosts asks about joiners, and a room that ends unasked says
    // why; the rest is the chat rooms' (a broadcast and a render queue have their own).
    if (room->isHost()) {
        connect(room, &Room::joinRequested, this, [this, room](JoinRequest* request) { askHost(request, room); });
    }
    connect(room, &Room::closed, this, [this, room](const QString& reason) {
        // A room this instance hosts closes only because something here closed it (the
        // panel, a broadcast's Stop), which says so itself if it wants to.
        if (!m_window || room->isHost() || reason == u"Left the room" || reason == u"The host closed the room" ||
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
    if (room->type() != RoomType::Chat) {
        return;
    }
    Channel* chat = room->channel(kChat);
    m_chat.insert(room, {});
    if (room->isHost()) {
        // A joiner gets the recent chat first (PEERS.md §10.1).
        chat->setSnapshotProvider([this, room] {
            QJsonArray lines;
            const QList<ChatLine> history = m_chat.value(room);
            // What people said: the host's own notices ("You host …") are its alone.
            for (qsizetype i = std::max<qsizetype>(0, history.size() - kSnapshotLines); i < history.size(); ++i) {
                if (history[i].from.isValid()) {
                    lines.append(lineJson(history[i]));
                }
            }
            return Channel::Snapshot{{{"lines", lines}}, {}};
        });
    }
    connect(chat, &Channel::received, this, [this, room](const Message& message) { onChat(room, message); });
    connect(room->channel(kTyping, Delivery::LatestOnly), &Channel::received, this,
            [this, room](const Message& message) { onTyping(room, message); });
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
        if (conversation(room, peer.id)) {
            addPrivateLine(room, peer, {{}, tr("%1 left the room").arg(peer.name), QDateTime::currentDateTime()});
        }
        if (m_typists.removeIf([&](const Typist& t) { return t.room == room && t.peer.id == peer.id; }) > 0) {
            Q_EMIT typingChanged(room, {});
            Q_EMIT typingChanged(room, peer.id);
        }
        updateStatus();
        Q_EMIT peopleChanged(room);
    });
    connect(room, &Room::renamed, this, [this, room](const QString& name) {
        addLine(room, {{}, tr("The room is now called %1").arg(name), QDateTime::currentDateTime()});
        Q_EMIT roomsChanged();
        Q_EMIT roomRenamed(room);
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

    Q_EMIT chatAdded(room, {}, line);
}

void PeersController::onChat(Room* room, const Message& message) {
    if (message.type == u"snapshot") {
        QList<ChatLine> lines;
        for (const QJsonValue& value : message.body.value("lines").toArray()) {
            const QJsonObject object = value.toObject();
            Peer from;
            from.id = object.value("from").toString();
            from.name = object.value("name").toString();
            from.color = object.value("color").toString();
            lines.append({from, object.value("text").toString(),
                          QDateTime::fromString(object.value("time").toString(), Qt::ISODate),
                          recordsFrom(object.value("records"))});
        }
        m_chat[room] = lines + m_chat.value(room);
        Q_EMIT chatReset(room);
        return;
    }
    if (message.type != u"say" && message.type != u"log") {
        return;
    }
    // Only a sealed message is private: a plain one to this member alone is ignored.
    if (!message.to.isEmpty() && !message.sealed) {
        return;
    }
    Peer from = room->peer(message.from);
    if (!from.isValid()) {
        from.id = message.from;
        from.name = tr("Someone");
    }
    ChatLine line{from, message.body.value("text").toString().left(kSharedTextLength), QDateTime::currentDateTime()};
    if (message.type == u"log") {
        line.records = recordsFrom(message.body.value("records"));
        if (line.records.isEmpty()) {
            return;
        }
    }
    const bool mine = from.id == room->me().id;
    if (message.sealed) {
        addPrivateLine(room, from, line);
    } else {
        addLine(room, line);
    }
    if (!mine) {
        const QString text = line.records.isEmpty()
                                 ? line.text
                                 : (line.records.size() == 1 ? tr("Shared a log message") : tr("Shared %1 log messages").arg(line.records.size()));
        toastChat(room, from, text, message.sealed);
    }
}

void PeersController::toastChat(Room* room, const Peer& from, const QString& text, bool privately) {
    if (!m_window || m_panelShown || isDoNotDisturb() || !core::Settings::instance().get<bool>(chatToastsKey, true) ||
        (!privately && isMuted(room)) || isMuted(from)) {
        return;
    }
    core::ToastRequest toast;
    toast.title = (privately ? tr("%1 (private) in %2") : tr("%1 in %2")).arg(from.name, room->name()).toStdString();
    toast.body = (text.size() > kToastTextLength ? text.left(kToastTextLength) + u'…' : text).toStdString();
    QPointer<Room> guard = room;
    const QString with = privately ? from.id : QString();
    m_window->toasts()->show(toast, {{tr("Open Peers"), [this, guard, with] {
                                          if (guard && showConversation) {
                                              showConversation(guard, with);
                                          } else if (showPanel) {
                                              showPanel();
                                          }
                                      }}});
}

PeersController::Conversation* PeersController::conversation(Room* room, const PeerId& with) {
    auto found = m_private.find(room);
    if (found == m_private.end()) {
        return nullptr;
    }
    for (Conversation& c : *found) {
        if (c.with.id == with) {
            return &c;
        }
    }
    return nullptr;
}

void PeersController::addPrivateLine(Room* room, const Peer& with, const ChatLine& line) {
    openConversation(room, with);
    Conversation* c = conversation(room, with.id);
    if (!c) {
        return;
    }
    c->lines.append(line);
    while (c->lines.size() > kHistoryLines) {
        c->lines.removeFirst();
    }
    if (line.from.isValid() && line.from.id != room->me().id) {
        ++c->unread;
        Q_EMIT conversationsChanged(room);
    }
    Q_EMIT chatAdded(room, with.id, line);
}

QList<PeersController::ChatLine> PeersController::chat(Room* room, const PeerId& with) const {
    if (with.isEmpty()) {
        return m_chat.value(room);
    }
    for (const Conversation& c : m_private.value(room)) {
        if (c.with.id == with) {
            return c.lines;
        }
    }
    return {};
}

void PeersController::say(Room* room, const QString& text) {
    const QString trimmed = text.trimmed();
    if (!trimmed.isEmpty() && room && room->state() == Room::State::Open) {
        stoppedTyping(room, {});
        // It comes back from the host in the room's order, and is shown then.
        room->channel(kChat)->send(QStringLiteral("say"), {{"text", trimmed}});
    }
}

bool PeersController::sayTo(Room* room, const PeerId& with, const QString& text) {
    const QString trimmed = text.trimmed().left(kSharedTextLength);
    if (trimmed.isEmpty() || !room || room->state() != Room::State::Open) {
        return false;
    }
    const Peer peer = room->peer(with);
    if (!canMessage(room, peer) || !room->channel(kChat)->sendPrivate(QStringLiteral("say"), {{"text", trimmed}}, with)) {
        return false;
    }
    stoppedTyping(room, with);
    // Nothing comes back from a private message: shown here at once.
    addPrivateLine(room, peer, {room->me(), trimmed, QDateTime::currentDateTime()});
    return true;
}

PeersController::SharedLogRecord PeersController::sharedRecord(const core::LogRecord& record) {
    return {QDateTime::fromMSecsSinceEpoch(
                std::chrono::duration_cast<std::chrono::milliseconds>(record.time.time_since_epoch()).count()),
            QString::fromLatin1(core::logLevelName(record.level).data(), qsizetype(core::logLevelName(record.level).size())),
            QString::fromStdString(record.source),
            QString::fromStdString(record.category),
            QString::fromStdString(record.message).left(kSharedTextLength),
            QString::fromStdString(record.details).left(kSharedTextLength)};
}

bool PeersController::shareLog(Room* room, const PeerId& with, QList<SharedLogRecord> records) {
    if (!room || room->state() != Room::State::Open || records.isEmpty()) {
        return false;
    }
    records = records.mid(0, kMaxSharedRecords);
    for (SharedLogRecord& record : records) {
        record.text = record.text.left(kSharedTextLength);
        record.details = record.details.left(kSharedTextLength);
    }
    // Within the size one message may have: the last ones go first.
    while (records.size() > 1 && QJsonDocument(recordsJson(records)).toJson(QJsonDocument::Compact).size() > kSharedBytes) {
        records.removeLast();
    }
    const QJsonObject body{{"records", recordsJson(records)}};
    if (with.isEmpty()) {
        room->channel(kChat)->send(QStringLiteral("log"), body);
        return true;
    }
    const Peer peer = room->peer(with);
    if (!canMessage(room, peer) || !room->channel(kChat)->sendPrivate(QStringLiteral("log"), body, with)) {
        return false;
    }
    addPrivateLine(room, peer, {room->me(), {}, QDateTime::currentDateTime(), records});
    return true;
}

void PeersController::fillShareMenu(QMenu* menu, const QList<SharedLogRecord>& records) {
    QList<Room*> rooms;
    for (Room* room : chatRooms()) {
        if (room->state() == Room::State::Open) {
            rooms.append(room);
        }
    }
    if (rooms.isEmpty()) {
        menu->addAction(tr("Host or join a room in Peers to share"))->setEnabled(false);
        return;
    }
    if (records.size() > kMaxSharedRecords) {
        menu->addAction(tr("Shares the first %1 of %2").arg(kMaxSharedRecords).arg(records.size()))->setEnabled(false);
        menu->addSeparator();
    }
    for (Room* room : rooms) {
        QMenu* sub = menu->addMenu(room->name());
        sub->setObjectName(QStringLiteral("peersShareRoom"));
        QPointer<Room> guard = room;
        sub->addAction(tr("Everyone"), this, [this, guard, records] {
            if (guard) {
                shareLog(guard, {}, records);
            }
        });
        bool separated = false;
        for (const Peer& peer : room->peers()) {
            if (!canMessage(room, peer)) {
                continue;
            }
            if (!separated) {
                sub->addSeparator();
                separated = true;
            }
            const PeerId id = peer.id;
            sub->addAction(tr("%1 (private)").arg(peer.name), this, [this, guard, id, records] {
                if (guard) {
                    shareLog(guard, id, records);
                }
            });
        }
    }
}

QList<Peer> PeersController::conversations(Room* room) const {
    QList<Peer> result;
    for (const Conversation& c : m_private.value(room)) {
        result.append(c.with);
    }
    return result;
}

bool PeersController::canMessage(Room* room, const Peer& peer) const {
    return room && peer.isValid() && peer.id != room->me().id && peer.kind != Peer::Kind::Guest &&
           !peer.publicKey.isEmpty() && room->peer(peer.id).isValid();
}

void PeersController::openConversation(Room* room, const Peer& peer) {
    if (!room || !peer.isValid() || conversation(room, peer.id)) {
        return;
    }
    m_private[room].append({peer, {}, 0});
    Q_EMIT conversationsChanged(room);
}

void PeersController::closeConversation(Room* room, const PeerId& with) {
    auto found = m_private.find(room);
    if (found != m_private.end() && found->removeIf([&](const Conversation& c) { return c.with.id == with; }) > 0) {
        Q_EMIT conversationsChanged(room);
    }
}

int PeersController::unread(Room* room, const PeerId& with) const {
    for (const Conversation& c : m_private.value(room)) {
        if (c.with.id == with) {
            return c.unread;
        }
    }
    return 0;
}

void PeersController::markRead(Room* room, const PeerId& with) {
    Conversation* c = conversation(room, with);
    if (c && c->unread != 0) {
        c->unread = 0;
        Q_EMIT conversationsChanged(room);
    }
}

QString PeersController::stageOf(Room* room, const Peer& peer) const {
    PeerState* presence = room->peerState(kPresence);
    return presence ? presence->value(peer.id).toObject().value("stage").toString() : QString();
}

namespace {

QString statusName(PeersController::Status status) {
    switch (status) {
    case PeersController::Status::Away:
        return QStringLiteral("away");
    case PeersController::Status::DoNotDisturb:
        return QStringLiteral("dnd");
    case PeersController::Status::Active:
        break;
    }
    return QStringLiteral("active");
}

}  // namespace

PeersController::Status PeersController::statusOf(Room* room, const Peer& peer) const {
    if (room && peer.id == room->me().id) {
        return status();
    }
    PeerState* presence = room ? room->peerState(kPresence) : nullptr;
    const QString name = presence ? presence->value(peer.id).toObject().value("status").toString() : QString();
    return name == u"dnd" ? Status::DoNotDisturb : name == u"away" ? Status::Away : Status::Active;
}

PeersController::Status PeersController::status() const {
    return isDoNotDisturb() ? Status::DoNotDisturb : m_away ? Status::Away : Status::Active;
}

bool PeersController::isDoNotDisturb() const {
    return appSettings().value(kDoNotDisturbKey, false).toBool();
}

void PeersController::setDoNotDisturb(bool on) {
    if (on == isDoNotDisturb()) {
        return;
    }
    QSettings settings = appSettings();
    settings.setValue(kDoNotDisturbKey, on);
    updatePresence();
    Q_EMIT statusChanged();
}

void PeersController::setAway(bool away) {
    if (away == m_away) {
        return;
    }
    m_away = away;
    updatePresence();
    Q_EMIT statusChanged();
}

void PeersController::typing(Room* room, const PeerId& with) {
    if (!room || room->state() != Room::State::Open) {
        return;
    }
    const QString key = typingKey(room, with);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime last = m_typingSent.value(key);
    if (last.isValid() && last.msecsTo(now) < kTypingEveryMs) {
        return;
    }
    m_typingSent.insert(key, now);
    // To the room, or to that person alone: no one else learns who writes to whom.
    room->channel(kTyping, Delivery::LatestOnly)->send(QStringLiteral("typing"), {}, {}, with);
}

void PeersController::stoppedTyping(Room* room, const PeerId& with) {
    if (!room || !m_typingSent.remove(typingKey(room, with)) || room->state() != Room::State::Open) {
        return;
    }
    room->channel(kTyping, Delivery::LatestOnly)->send(QStringLiteral("stopped"), {}, {}, with);
}

void PeersController::onTyping(Room* room, const Message& message) {
    const Peer from = room->peer(message.from);
    if (!from.isValid() || from.id == room->me().id) {
        return;
    }
    // To this member alone: the private conversation with whoever writes.
    const QString with = message.to.isEmpty() ? QString() : from.id;
    const auto same = [&](const Typist& t) { return t.room == room && t.with == with && t.peer.id == from.id; };
    if (message.type == u"typing") {
        const QDateTime until = QDateTime::currentDateTimeUtc().addMSecs(kTypingShownMs);
        auto found = std::find_if(m_typists.begin(), m_typists.end(), same);
        if (found != m_typists.end()) {
            found->until = until;
            return;
        }
        m_typists.append({room, with, from, until});
        m_typingTimer->start();
    } else if (message.type == u"stopped") {
        if (m_typists.removeIf(same) == 0) {
            return;
        }
    } else {
        return;
    }
    Q_EMIT typingChanged(room, with);
}

void PeersController::expireTypists() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QList<QPair<Room*, QString>> changed;
    for (qsizetype i = m_typists.size() - 1; i >= 0; --i) {
        if (m_typists[i].until <= now) {
            changed.append({m_typists[i].room, m_typists[i].with});
            m_typists.removeAt(i);
        }
    }
    if (m_typists.isEmpty()) {
        m_typingTimer->stop();
    }
    for (const auto& [room, with] : changed) {
        Q_EMIT typingChanged(room, with);
    }
}

QList<Peer> PeersController::typingIn(Room* room, const PeerId& with) const {
    QList<Peer> result;
    for (const Typist& t : m_typists) {
        if (t.room == room && t.with == with) {
            result.append(t.peer);
        }
    }
    return result;
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
                // The name in full is in the title; the button stays within a toast's width.
                {tr("Always Allow %1").arg(peer.name.size() > 20 ? peer.name.left(19) + u'…' : peer.name),
                 [this, guard, peer] {
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
    m_peers->setLooking((m_panelShown || m_swarmShown) && core::Settings::instance().get<bool>(lookOnNetworkKey, true));
}

void PeersController::setSwarmShown(bool shown) {
    m_swarmShown = shown;
    setPanelShown(m_panelShown);
}

void PeersController::updatePresence() {
    const QString stage = m_hooks.currentStage ? m_hooks.currentStage() : QString();
    for (Room* room : chatRooms()) {
        if (room->state() == Room::State::Open) {
            room->peerState(kPresence)->set(QJsonObject{{"stage", stage}, {"status", statusName(status())}});
        }
    }
}

void PeersController::updateStatus() {
    if (!m_status) {
        return;
    }
    const QList<Room*> rooms = chatRooms();
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
