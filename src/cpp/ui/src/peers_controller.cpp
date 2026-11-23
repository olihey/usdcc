#include "usdcc/ui/peers_controller.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/core/version.h"
#include "usdcc/peers/channel.h"
#include "usdcc/peers/state.h"
#include "usdcc/peers/transfer.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/toast_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QRegularExpression>
#include <QUuid>
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
// Shared images' files (PEERS.md §10.1): a bundle of one file each, fetched from the sender.
const QString kChatFiles = QStringLiteral("chat-files");
constexpr qint64 kMaxPreviewBytes = 32 * 1024 * 1024;
constexpr int kImagesPerMinute = 10;  // from one sender; more are left out
constexpr int kOfferHours = 24;       // an image stays offered this long

// Whether the file's first bytes are what its name says: the image types, nothing else.
bool contentMatchesName(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray head = file.read(8);
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == u"png") {
        return head.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8));
    }
    if (suffix == u"jpg" || suffix == u"jpeg") {
        return head.startsWith(QByteArray("\xff\xd8\xff", 3));
    }
    if (suffix == u"exr") {
        return head.startsWith(QByteArray("\x76\x2f\x31\x01", 4));
    }
    if (suffix == u"tif" || suffix == u"tiff") {
        return head.startsWith(QByteArray("II*\0", 4)) || head.startsWith(QByteArray("MM\0*", 4));
    }
    return false;
}

qint64 folderBytes(const QString& folder) {
    qint64 total = 0;
    QDirIterator it(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        total += it.nextFileInfo().size();
    }
    return total;
}

// A shared image's file name: one plain name, of an image.
bool isImageName(const QString& name) {
    static const QStringList suffixes = {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
                                         QStringLiteral("exr"), QStringLiteral("tif"),  QStringLiteral("tiff")};
    return !name.contains(u'/') && isSafeTransferPath(name) && suffixes.contains(QFileInfo(name).suffix().toLower());
}

bool isBundleHash(const QString& hash) {
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    return hex.match(hash).hasMatch();
}

QJsonObject imageToJson(const PeersController::ChatImage& image) {
    QJsonObject json{{"preview", image.preview}, {"previewName", image.previewName}, {"previewBytes", image.previewBytes}};
    if (!image.original.isEmpty()) {
        json["original"] = image.original;
        json["originalName"] = image.originalName;
        json["originalBytes"] = image.originalBytes;
    }
    if (!image.sharedPath.isEmpty()) {
        json["sharedPath"] = image.sharedPath;
    }
    return json;
}

// Nothing for a message without a valid preview; an original that isn't valid is left out.
std::optional<PeersController::ChatImage> imageFromJson(const QJsonObject& json) {
    PeersController::ChatImage image;
    image.preview = json.value("preview").toString();
    image.previewName = json.value("previewName").toString();
    image.previewBytes = json.value("previewBytes").toInteger();
    if (!isBundleHash(image.preview) || !isImageName(image.previewName) || image.previewBytes <= 0 ||
        image.previewBytes > kMaxPreviewBytes) {
        return std::nullopt;
    }
    image.original = json.value("original").toString();
    image.originalName = json.value("originalName").toString();
    image.originalBytes = json.value("originalBytes").toInteger();
    if (!isBundleHash(image.original) || !isImageName(image.originalName) || image.originalBytes <= 0) {
        image.original.clear();
        image.originalName.clear();
        image.originalBytes = 0;
    }
    image.sharedPath = json.value("sharedPath").toString().left(1024);
    return image;
}

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
    settings.define({.key = acceptImagesKey,
                     .category = "Peers",
                     .group = "Shared images",
                     .label = "Images shared with you",
                     .description = "ask: each one waits for Accept, Preview Only or Decline, and nothing is "
                                    "downloaded before. never: they're listed, and nothing can be downloaded.",
                     .type = SettingType::Choice,
                     .defaultValue = std::string("ask"),
                     .choices = {"ask", "never"}});
    settings.define({.key = receivedImagesMaxKey,
                     .category = "Peers",
                     .group = "Shared images",
                     .label = "Received images up to (GB)",
                     .description = "Over this, an image you accept isn't downloaded until you clear some.",
                     .type = SettingType::Int,
                     .defaultValue = 2,
                     .minimum = 1,
                     .maximum = 10000});
    settings.define({.key = keepReceivedImagesKey,
                     .category = "Peers",
                     .group = "Shared images",
                     .label = "Keep received images (days)",
                     .description = "Older ones are deleted when usdcc starts.",
                     .type = SettingType::Int,
                     .defaultValue = 30,
                     .minimum = 1,
                     .maximum = 3650});
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
        m_imageInboxes.remove(room);
        updateStatus();
        Q_EMIT roomsChanged();
    });
    connect(m_peers.get(), &Peers::foundRoomsChanged, this, &PeersController::roomsChanged);
    // Received images older than the setting says go when usdcc starts.
    {
        const QDateTime cutoff =
            QDateTime::currentDateTime().addDays(-core::Settings::instance().get<int>(keepReceivedImagesKey, 30));
        const QDir folder(receivedImagesFolder());
        for (const QFileInfo& bundle : folder.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (bundle.lastModified() < cutoff) {
                QDir(bundle.absoluteFilePath()).removeRecursively();
            }
        }
    }
    core::Settings::instance().define({.key = "Peers/clearReceivedImages",
                                       .category = "Peers",
                                       .group = "Shared images",
                                       .label = "Received images",
                                       .description = "Deletes every image shared with you that was downloaded.",
                                       .type = SettingType::Action,
                                       .defaultValue = std::string("Clear Received Images"),
                                       .action = [this] { clearReceivedImages(); }});
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
    openImageInbox(room);
    if (room->isHost()) {
        // A joiner gets the recent chat first (PEERS.md §10.1).
        chat->setSnapshotProvider([this, room] {
            QJsonArray lines;
            const QList<ChatLine> history = m_chat.value(room);
            // What people said: the host's own notices ("You host …") are its alone.
            for (qsizetype i = std::max<qsizetype>(0, history.size() - kSnapshotLines); i < history.size(); ++i) {
                const ChatLine& line = history[i];
                if (!line.from.isValid() || !line.to.isEmpty()) {
                    continue;  // The host's own notices, and one member's lines, aren't the room's
                }
                QJsonObject json = lineJson(line);
                if (line.image) {
                    json["image"] = imageToJson(*line.image);
                }
                lines.append(json);
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
            ChatLine line{from, object.value("text").toString(),
                          QDateTime::fromString(object.value("time").toString(), Qt::ISODate)};
            line.records = recordsFrom(object.value("records"));
            if (object.contains("image")) {
                line.image = imageFromJson(object.value("image").toObject());
                if (!line.image) {
                    continue;
                }
            }
            lines.append(line);
        }
        m_chat[room] = lines + m_chat.value(room);
        for (const ChatLine& line : lines) {
            if (line.image) {
                onImageLine(room, line);
            }
        }
        Q_EMIT chatReset(room);
        return;
    }
    if (message.type == u"fetch") {
        onFetch(room, message);
        return;
    }
    if (message.type == u"gone") {
        const QString hash = message.body.value("hash").toString();
        const auto expected = m_expected.constFind(hash);
        if (expected != m_expected.constEnd() && expected->from == message.from &&
            m_images.value(hash).state == ImageFile::State::Fetching) {
            ImageFile& file = m_images[hash];
            file.state = ImageFile::State::Failed;
            file.error = message.body.value("reason").toString().left(300);
            Q_EMIT imageChanged(hash);
        }
        return;
    }
    std::optional<ChatImage> image;
    if (message.type == u"image") {
        image = imageFromJson(message.body);
        if (!image) {
            return;
        }
    } else if (message.type != u"say" && message.type != u"log") {
        return;
    }
    // To this member alone: a private conversation. Words and log messages count only
    // sealed (§6.2); an image may also come plain from an older sender, since its files
    // are offered to this member alone either way.
    const bool privately = !message.to.isEmpty();
    if (privately && !message.sealed && !image) {
        return;
    }
    Peer from = room->peer(message.from);
    if (!from.isValid()) {
        from.id = message.from;
        from.name = tr("Someone");
    }
    const QString text = message.body.value("text").toString().left(kSharedTextLength);
    if (image && from.id != room->me().id) {
        // At most kImagesPerMinute a minute from one sender; the rest are left out.
        const QDateTime now = QDateTime::currentDateTime();
        QList<QDateTime>& times = m_imageTimes[from.id];
        times.removeIf([&now](const QDateTime& when) { return when.secsTo(now) >= 60; });
        if (times.size() >= kImagesPerMinute) {
            const QDateTime noted = m_floodNoted.value(from.id);
            if (!noted.isValid() || noted.secsTo(now) >= 60) {
                m_floodNoted.insert(from.id, now);
                addLine(room, {{}, tr("%1 is sharing many images; the rest of this minute's are left out").arg(from.name),
                               now});
            }
            return;
        }
        times.append(now);
    }
    ChatLine line{from, text, QDateTime::currentDateTime(), image, message.to};
    if (message.type == u"log") {
        line.records = recordsFrom(message.body.value("records"));
        if (line.records.isEmpty()) {
            return;
        }
    }
    const bool mine = from.id == room->me().id;
    if (privately) {
        addPrivateLine(room, from, line);
    } else {
        addLine(room, line);
    }
    if (image) {
        onImageLine(room, line);
    }
    if (!mine) {
        const QString shown = !line.records.isEmpty()
                                  ? (line.records.size() == 1 ? tr("Shared a log message")
                                                              : tr("Shared %1 log messages").arg(line.records.size()))
                              : image && text.isEmpty() ? tr("Shared an image")
                                                        : text;
        toastChat(room, from, shown, privately, image);
    }
}

void PeersController::toastChat(Room* room, const Peer& from, const QString& text, bool privately,
                                const std::optional<ChatImage>& image) {
    if (!m_window || m_panelShown || isDoNotDisturb() || !core::Settings::instance().get<bool>(chatToastsKey, true) ||
        (!privately && isMuted(room)) || isMuted(from)) {
        return;
    }
    core::ToastRequest toast;
    toast.title = (privately ? tr("%1 (private) in %2") : tr("%1 in %2")).arg(from.name, room->name()).toStdString();
    toast.body = (text.size() > kToastTextLength ? text.left(kToastTextLength) + u'…' : text).toStdString();
    QPointer<Room> guard = room;
    QList<ToastAction> actions;
    if (image && acceptsImages()) {
        // Asked here too: nothing comes before the answer.
        const QString preview = image->preview;
        actions.append({tr("Accept"), [this, guard, preview] {
                            if (guard) {
                                acceptImage(guard, preview, true);
                            }
                        }});
        actions.append({tr("Decline"), [this, preview] { declineImage(preview); }});
    }
    const QString with = privately ? from.id : QString();
    actions.append({tr("Open Peers"), [this, guard, with] {
                        if (guard && showConversation) {
                            showConversation(guard, with);
                        } else if (showPanel) {
                            showPanel();
                        }
                    }});
    m_window->toasts()->show(toast, actions);
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
    addPrivateLine(room, peer, {room->me(), trimmed, QDateTime::currentDateTime(), std::nullopt, with});
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
    ChatLine line{room->me(), {}, QDateTime::currentDateTime(), std::nullopt, with};
    line.records = records;
    addPrivateLine(room, peer, line);
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

// --- Shared images (PEERS.md §10.1)

void PeersController::openImageInbox(Room* room) {
    TransferInbox::Options options;
    options.directory = receivedImagesFolder();
    // Only what a chat line announced, from whoever announced it, at most its size.
    options.accept = [this, room](const Peer& from, const TransferManifest& manifest) {
        const auto expected = m_expected.constFind(manifest.hash());
        const qint64 cap =
            qint64(core::Settings::instance().get<int>(receivedImagesMaxKey, 2)) * 1024 * 1024 * 1024;
        return acceptsImages() && expected != m_expected.constEnd() && expected->room == room &&
               expected->from == from.id && m_images.value(manifest.hash()).state == ImageFile::State::Fetching &&
               manifest.files.size() == 1 && manifest.files.front().path == expected->name &&
               manifest.totalBytes() <= expected->bytes &&
               folderBytes(receivedImagesFolder()) + manifest.totalBytes() <= cap;
    };
    auto* inbox = new TransferInbox(room, kChatFiles, options, room);
    m_imageInboxes.insert(room, inbox);
    connect(inbox, &TransferInbox::started, this, [this](Transfer* transfer) {
        watchTransfer(transfer, transfer->bundleHash());
    });
    connect(inbox, &TransferInbox::received, this, [this, inbox](Transfer* transfer) {
        const QString hash = transfer->bundleHash();
        ImageFile& file = m_images[hash];
        const QString path = QDir(inbox->bundleDirectory(hash)).filePath(m_expected.value(hash).name);
        if (!contentMatchesName(path)) {
            // Not the image it says it is: it goes at once, and is never opened.
            QDir(inbox->bundleDirectory(hash)).removeRecursively();
            file = ImageFile{};
            file.state = ImageFile::State::Failed;
            file.error = tr("Not an image; removed");
            USDCC_LOG_WARNING("usdcc.peers", "A shared file that isn't the image its name says was removed: {}",
                              m_expected.value(hash).name.toStdString());
            Q_EMIT imageChanged(hash);
            return;
        }
        file.state = ImageFile::State::Done;
        file.path = path;
        file.done = file.total = transfer->bytesTotal();
        file.error.clear();
        Q_EMIT imageChanged(hash);
    });
}

void PeersController::watchTransfer(Transfer* transfer, const QString& hash) {
    ImageFile& file = m_images[hash];
    file.state = ImageFile::State::Fetching;
    file.total = transfer->bytesTotal();
    Q_EMIT imageChanged(hash);
    connect(transfer, &Transfer::progress, this, [this, hash](qint64 done, qint64 total) {
        ImageFile& file = m_images[hash];
        if (file.state != ImageFile::State::Fetching) {
            return;
        }
        // A step of 1% or more, so a large file doesn't redraw the chat for every piece.
        const qint64 step = std::max<qint64>(1, total / 100);
        if (done / step == file.done / step && done != total) {
            return;
        }
        file.done = done;
        file.total = total;
        Q_EMIT imageChanged(hash);
    });
    connect(transfer, &Transfer::failed, this, [this, hash](const QString& reason) {
        ImageFile& file = m_images[hash];
        if (file.state == ImageFile::State::Done) {
            return;
        }
        file.state = ImageFile::State::Failed;
        file.error = reason;
        Q_EMIT imageChanged(hash);
    });
}

void PeersController::onImageLine(Room* room, const ChatLine& line) {
    const ChatImage& image = *line.image;
    const bool mine = line.from.id == room->me().id;
    TransferInbox* inbox = m_imageInboxes.value(room);
    const auto note = [&](const QString& hash, const QString& name, qint64 bytes) {
        if (hash.isEmpty() || m_images.value(hash).state == ImageFile::State::Done) {
            return;
        }
        if (const auto offer = m_offers.constFind(hash); offer != m_offers.constEnd()) {
            // This instance's own: the file it shared.
            ImageFile& file = m_images[hash];
            file.state = ImageFile::State::Done;
            file.path = offer->path;
            file.done = file.total = bytes;
            Q_EMIT imageChanged(hash);
            return;
        }
        m_expected.insert(hash, {room, line.from.id, name, bytes});
        if (inbox && inbox->hasBundle(hash) && contentMatchesName(QDir(inbox->bundleDirectory(hash)).filePath(name)) &&
            m_accepted.contains(image.preview)) {
            ImageFile& file = m_images[hash];
            file.state = ImageFile::State::Done;
            file.path = QDir(inbox->bundleDirectory(hash)).filePath(name);
            file.done = file.total = bytes;
            Q_EMIT imageChanged(hash);
        }
    };
    note(image.preview, image.previewName, image.previewBytes);
    note(image.original, image.originalName, image.originalBytes);
    if (!image.original.isEmpty()) {
        m_originals.insert(image.preview, image.original);
    }
    // Nothing is fetched here: the receiver answers first (acceptImage()).
}

bool PeersController::acceptsImages() {
    return core::Settings::instance().get<std::string>(acceptImagesKey, "ask") != "never";
}

void PeersController::acceptImage(Room* room, const QString& preview, bool original) {
    if (!acceptsImages() || !m_expected.contains(preview)) {
        return;
    }
    m_declined.remove(preview);
    m_accepted.insert(preview);
    fetchImage(room, preview);
    if (original && m_originals.contains(preview)) {
        fetchImage(room, m_originals.value(preview));
    }
    Q_EMIT imageChanged(preview);
}

void PeersController::declineImage(const QString& preview) {
    if (m_accepted.contains(preview)) {
        return;
    }
    m_declined.insert(preview);
    Q_EMIT imageChanged(preview);
}

void PeersController::stopSharing(const QString& preview) {
    const auto offer = m_offers.constFind(preview);
    if (offer == m_offers.constEnd()) {
        return;
    }
    const QString original = offer->original;
    // The preview's copy was this instance's own; the original is the user's file and stays.
    QDir(QFileInfo(offer->path).absolutePath()).removeRecursively();
    m_offers.remove(preview);
    m_offers.remove(original);
    Q_EMIT imageChanged(preview);
}

QString PeersController::receivedImagesFolder() {
    return usdcc::ui::userDataFolder() + QStringLiteral("/peers/chat-images");
}

void PeersController::clearReceivedImages() {
    QDir(receivedImagesFolder()).removeRecursively();
    QList<QString> cleared;
    for (auto it = m_images.begin(); it != m_images.end(); ++it) {
        if (it->state == ImageFile::State::Done && !m_offers.contains(it.key())) {
            *it = ImageFile{};
            cleared.append(it.key());
        }
    }
    m_accepted.clear();
    for (const QString& hash : cleared) {
        Q_EMIT imageChanged(hash);
    }
}

PeersController::ImageFile PeersController::imageFile(const QString& hash) const {
    return m_images.value(hash);
}

void PeersController::fetchImage(Room* room, const QString& hash) {
    const auto expected = m_expected.constFind(hash);
    if (!room || expected == m_expected.constEnd() || expected->room != room) {
        return;
    }
    if (!acceptsImages()) {
        return;
    }
    ImageFile& file = m_images[hash];
    if (file.state == ImageFile::State::Done || file.state == ImageFile::State::Fetching) {
        return;
    }
    file.total = expected->bytes;
    file.done = 0;
    const qint64 cap = qint64(core::Settings::instance().get<int>(receivedImagesMaxKey, 2)) * 1024 * 1024 * 1024;
    if (folderBytes(receivedImagesFolder()) + expected->bytes > cap) {
        file.state = ImageFile::State::Failed;
        file.error = tr("Your received images are at their limit (Settings > Peers); clear some first");
    } else if (room->state() != Room::State::Open || !room->peer(expected->from).isValid()) {
        file.state = ImageFile::State::Failed;
        file.error = tr("The sender isn't in the room any more");
    } else {
        file.state = ImageFile::State::Fetching;
        file.error.clear();
        room->channel(kChat)->send(QStringLiteral("fetch"), {{"hash", hash}}, {}, expected->from);
    }
    Q_EMIT imageChanged(hash);
}

void PeersController::onFetch(Room* room, const Message& message) {
    const QString hash = message.body.value("hash").toString();
    const auto refuse = [&](const QString& reason) {
        room->channel(kChat)->send(QStringLiteral("gone"), {{"hash", hash}, {"reason", reason}}, {}, message.from);
    };
    const auto offer = m_offers.constFind(hash);
    if (offer == m_offers.constEnd() || offer->room != room || (!offer->to.isEmpty() && offer->to != message.from)) {
        return refuse(tr("It isn't shared with you"));
    }
    if (offer->expires.isValid() && QDateTime::currentDateTime() > offer->expires) {
        return refuse(tr("It isn't shared any more"));
    }
    if (!QFileInfo::exists(offer->path)) {
        return refuse(tr("The file isn't there any more"));
    }
    QString error;
    Transfer* transfer = Transfer::send(room, kChatFiles, message.from, *offer->manifest, {}, this, &error);
    if (!transfer) {
        return refuse(error);
    }
    connect(transfer, &Transfer::finished, transfer, &QObject::deleteLater);
    connect(transfer, &Transfer::failed, transfer, &QObject::deleteLater);
}

bool PeersController::shareImage(const ShareRequest& request, QString* error) {
    const auto fail = [error](const QString& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    Room* room = request.room;
    if (!room || room->state() != Room::State::Open || !m_chat.contains(room)) {
        return fail(tr("Not in that room"));
    }
    if (!request.to.isEmpty() && !room->peer(request.to).isValid()) {
        return fail(tr("They aren't in the room any more"));
    }
    if (request.preview.isNull()) {
        return fail(tr("No image"));
    }
    // The preview, kept while this instance runs.
    QString previewName = QFileInfo(request.previewName).completeBaseName();
    previewName.replace(QRegularExpression(QStringLiteral(R"([^A-Za-z0-9._ -])")), QStringLiteral("_"));
    previewName = (previewName.trimmed().isEmpty() ? QStringLiteral("image") : previewName.trimmed().left(100)) +
                  QStringLiteral(".jpg");
    const QString folder = usdcc::ui::userDataFolder() + QStringLiteral("/peers/shared/") +
                           QUuid::createUuid().toString(QUuid::Id128);
    if (!QDir().mkpath(folder) || !request.preview.save(QDir(folder).filePath(previewName), "JPG", 90)) {
        return fail(tr("Couldn't write the preview in %1").arg(QDir::toNativeSeparators(folder)));
    }
    QString why;
    const std::optional<TransferManifest> preview = TransferManifest::fromFiles(folder, {previewName}, &why);
    if (!preview) {
        return fail(why);
    }
    ChatImage image;
    image.preview = preview->hash();
    image.previewName = previewName;
    image.previewBytes = preview->totalBytes();
    image.sharedPath = request.sharedPath;
    std::optional<TransferManifest> original;
    const QFileInfo originalFile(request.original);
    if (!request.original.isEmpty()) {
        if (!isImageName(originalFile.fileName())) {
            return fail(tr("%1 can't be shared: only images (EXR, PNG, JPEG, TIFF) with a plain name")
                            .arg(originalFile.fileName()));
        }
        original = TransferManifest::fromFiles(originalFile.absolutePath(), {originalFile.fileName()}, &why);
        if (!original) {
            return fail(why);
        }
        image.original = original->hash();
        image.originalName = originalFile.fileName();
        image.originalBytes = original->totalBytes();
    }
    const QDateTime expires = QDateTime::currentDateTime().addSecs(qint64(kOfferHours) * 3600);
    m_offers.insert(image.preview, {room, request.to, std::make_shared<TransferManifest>(*preview),
                                    QDir(folder).filePath(previewName), expires, image.original});
    if (original) {
        m_offers.insert(image.original, {room, request.to, std::make_shared<TransferManifest>(*original),
                                         originalFile.absoluteFilePath(), expires, QString()});
    }
    QJsonObject body = imageToJson(image);
    body["text"] = request.caption.trimmed().left(4000);
    const Peer to = request.to.isEmpty() ? Peer() : room->peer(request.to);
    if (to.isValid() && canMessage(room, to)) {
        room->channel(kChat)->sendPrivate(QStringLiteral("image"), body, request.to);  // Sealed (§6.2)
    } else {
        room->channel(kChat)->send(QStringLiteral("image"), body, {}, request.to);
    }
    if (!request.to.isEmpty()) {
        // To one member: it doesn't come back from the host, so it's shown here now, in
        // the private conversation with them.
        const ChatLine line{room->me(), request.caption.trimmed().left(4000), QDateTime::currentDateTime(), image,
                            request.to};
        addPrivateLine(room, to.isValid() ? to : Peer{request.to, tr("someone")}, line);
        onImageLine(room, line);
    }
    return true;
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
    // One whose time is up isn't typing, though the timer that removes it may not have
    // ticked yet (it ticks every second).
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const Typist& t : m_typists) {
        if (t.room == room && t.with == with && t.until > now) {
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
