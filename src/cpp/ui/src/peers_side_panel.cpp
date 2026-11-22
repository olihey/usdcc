#include "usdcc/ui/peers_side_panel.h"

#include "usdcc/peers/room.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QWidgetAction>
#include <QVBoxLayout>

namespace usdcc::ui {

using namespace usdcc::peers;

namespace {

enum Role { RoomRole = Qt::UserRole + 1, FoundRole, PeerRole };

constexpr int kThumbnailWidth = 240;
constexpr int kThumbnailHeight = 180;
constexpr int kMaxPreviewSide = 8192;  // a shared preview larger than this isn't decoded

QString sizeText(qint64 bytes) {
    if (bytes >= qint64(1024) * 1024 * 1024) {
        return QObject::tr("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024 * 1024) {
        return QObject::tr("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    }
    return QObject::tr("%1 KB").arg(std::max<qint64>(1, bytes / 1024));
}

// Explorer (Finder, the file manager) with the file picked, where it can.
void revealInFolder(const QString& path) {
#ifdef _WIN32
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + QDir::toNativeSeparators(path)});
#elif defined(__APPLE__)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

// A shared image under its chat line: the preview, and the original with what can be done
// with it. Every link names a bundle hash; nothing a member sent goes in unescaped.
QString imageHtml(const PeersController* controller, const PeersController::ChatImage& image, bool mine) {
    using State = PeersController::ImageFile::State;
    QString html;
    const QString what = image.original.isEmpty()
                             ? QObject::tr("a preview, %1").arg(sizeText(image.previewBytes))
                             : QObject::tr("a preview, %1, and %2, %3")
                                   .arg(sizeText(image.previewBytes), image.originalName.toHtmlEscaped(),
                                        sizeText(image.originalBytes));
    if (!mine) {
        // Nothing comes before the receiver says so.
        if (!PeersController::acceptsImages()) {
            return QStringLiteral("<br><i style=\"color:#8a8d93\">%1</i>")
                .arg(QObject::tr("An image (%1); images shared with you are off in Settings > Peers").arg(what));
        }
        if (controller->isDeclined(image.preview)) {
            return QStringLiteral("<br><i style=\"color:#8a8d93\">%1</i>").arg(QObject::tr("Declined: %1").arg(what));
        }
        if (!controller->isAccepted(image.preview)) {
            html += QStringLiteral("<br><span style=\"color:#8a8d93\">%1</span><br><a href=\"accept:%2\">%3</a>")
                        .arg(QObject::tr("Wants to share %1").arg(what), image.preview, QObject::tr("Accept"));
            if (!image.original.isEmpty()) {
                html += QStringLiteral(" · <a href=\"previewonly:%1\">%2</a>").arg(image.preview, QObject::tr("Preview Only"));
            }
            html += QStringLiteral(" · <a href=\"decline:%1\">%2</a>").arg(image.preview, QObject::tr("Decline"));
            return html;
        }
    }
    const PeersController::ImageFile preview = controller->imageFile(image.preview);
    switch (preview.state) {
    case State::Done: {
        // Its size from the header first: one that's unreasonably large isn't decoded.
        QSize size = QImageReader(preview.path).size();
        if (!size.isValid() || size.width() > kMaxPreviewSide || size.height() > kMaxPreviewSide) {
            html += QStringLiteral("<br><i style=\"color:#8a8d93\">%1</i>").arg(QObject::tr("The preview can't be shown"));
            break;
        }
        size.scale(std::min(size.width(), kThumbnailWidth), std::min(size.height(), kThumbnailHeight), Qt::KeepAspectRatio);
        html += QStringLiteral("<br><a href=\"image:%1\"><img src=\"%2\" width=\"%3\" height=\"%4\"></a>")
                    .arg(image.preview, QUrl::fromLocalFile(preview.path).toString().toHtmlEscaped())
                    .arg(size.width())
                    .arg(size.height());
        break;
    }
    case State::Fetching:
    case State::Missing:
        html += QStringLiteral("<br><i style=\"color:#8a8d93\">%1</i>").arg(QObject::tr("The preview is on its way…"));
        break;
    case State::Failed:
        html += QStringLiteral("<br><i style=\"color:#8a8d93\">%1</i>")
                    .arg(QObject::tr("No preview: %1").arg(preview.error).toHtmlEscaped());
        break;
    }
    if (!image.original.isEmpty()) {
        const PeersController::ImageFile original = controller->imageFile(image.original);
        const QString name = image.originalName.toHtmlEscaped();
        const QString size = sizeText(image.originalBytes);
        html += QStringLiteral("<br>");
        switch (original.state) {
        case State::Done:
            html += QStringLiteral("%1 · <a href=\"open:%2\">%3</a> · <a href=\"reveal:%2\">%4</a>")
                        .arg(name, image.original, QObject::tr("Open"), QObject::tr("Show in Folder"));
            break;
        case State::Fetching:
            html += QObject::tr("%1 · %2% of %3")
                        .arg(name)
                        .arg(original.total > 0 ? int(original.done * 100 / original.total) : 0)
                        .arg(size);
            break;
        case State::Missing:
            html += QStringLiteral("<a href=\"fetch:%1\">%2</a>")
                        .arg(image.original, QObject::tr("Download %1 (%2)").arg(name, size));
            break;
        case State::Failed:
            html += QStringLiteral("%1: %2 · <a href=\"fetch:%3\">%4</a>")
                        .arg(name, original.error.toHtmlEscaped(), image.original, QObject::tr("Try Again"));
            break;
        }
    }
    if (mine) {
        html += controller->isSharing(image.preview)
                    ? QStringLiteral("<br><a href=\"stop:%1\">%2</a>").arg(image.preview, QObject::tr("Stop Sharing"))
                    : QStringLiteral("<br><i style=\"color:#8a8d93\">%1</i>").arg(QObject::tr("No longer shared"));
    }
    if (!image.sharedPath.isEmpty()) {
        html += QStringLiteral("<br><span style=\"color:#8a8d93\">%1</span> <a href=\"shared:%2\">%3</a>")
                    .arg(QObject::tr("On a shared drive:"), image.preview,
                         QDir::toNativeSeparators(image.sharedPath).toHtmlEscaped());
    }
    return html;
}

QIcon dot(const QString& color) {
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(color.isEmpty() ? QStringLiteral("#8a8d93") : color));
    painter.drawEllipse(1, 1, 10, 10);
    return QIcon(pixmap);
}

QString admissionText(Admission admission, bool password) {
    QString text = admission == Admission::Open  ? QObject::tr("public")
                   : admission == Admission::Ask ? QObject::tr("asks the host")
                                                 : QObject::tr("invite only");
    if (password) {
        text += QObject::tr(", password");
    }
    return text;
}

QString stateText(Room* room) {
    switch (room->state()) {
    case Room::State::Connecting:
        return QObject::tr("joining…");
    case Room::State::Reconnecting:
        return QObject::tr("reconnecting…");
    case Room::State::Closed:
        return QObject::tr("closed");
    case Room::State::Open:
        break;
    }
    return room->isHost() ? QObject::tr("you host · %1").arg(room->peers().size())
                          : QObject::tr("%1 here").arg(room->peers().size());
}

}  // namespace

QString chatHtml(const QString& text) {
    // Never HTML from a member: prim paths are found in the text as typed, and every
    // piece of it is escaped on its own.
    static const QRegularExpression primPath(
        QStringLiteral(R"((^|[\s(\[,;])(/[A-Za-z_][A-Za-z0-9_]*(?:/[A-Za-z_][A-Za-z0-9_]*)*))"));
    QString result;
    qsizetype last = 0;
    auto it = primPath.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const qsizetype start = match.capturedStart(2);
        result += text.mid(last, start - last).toHtmlEscaped();
        const QString path = match.captured(2).toHtmlEscaped();
        result += QStringLiteral("<a href=\"prim:%1\">%1</a>").arg(path);
        last = match.capturedEnd(2);
    }
    result += text.mid(last).toHtmlEscaped();
    return result.replace(u'\n', QStringLiteral("<br>"));
}

PeersSidePanel::PeersSidePanel(PeersController* controller, QWidget* parent)
    : SidePanel(tr("Peers"), parent), m_controller(controller) {
    setObjectName(QStringLiteral("peersPanel"));
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    scaled::margins(toolbarLayout, 6, 4, 4, 4);
    scaled::spacing(toolbarLayout, 4);
    m_you = new QLabel(toolbar);
    m_you->setObjectName(QStringLiteral("peersYou"));
    auto* host = new QPushButton(tr("Host Room…"), toolbar);
    host->setObjectName(QStringLiteral("peersHost"));
    auto* join = new QPushButton(tr("Join…"), toolbar);
    join->setObjectName(QStringLiteral("peersJoin"));
    toolbarLayout->addWidget(m_you, 1);
    toolbarLayout->addWidget(host);
    toolbarLayout->addWidget(join);
    layout->addWidget(toolbar);
    connect(host, &QPushButton::clicked, this, &PeersSidePanel::hostRoom);
    connect(join, &QPushButton::clicked, this, &PeersSidePanel::joinRoom);

    auto* split = new QSplitter(Qt::Vertical, container);
    layout->addWidget(split, 1);

    m_rooms = new QTreeWidget(split);
    m_rooms->setObjectName(QStringLiteral("peersRooms"));
    m_rooms->setColumnCount(2);
    m_rooms->setHeaderHidden(true);
    m_rooms->header()->setStretchLastSection(false);
    m_rooms->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_rooms->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_rooms->setRootIsDecorated(false);
    m_rooms->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_rooms, &QTreeWidget::customContextMenuRequested, this, &PeersSidePanel::roomMenu);
    connect(m_rooms, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (item && item->data(0, RoomRole).isValid()) {
            showRoom(static_cast<Room*>(item->data(0, RoomRole).value<QObject*>()));
        }
    });
    connect(m_rooms, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item && item->data(0, FoundRole).isValid()) {
            joinFound(item->data(0, FoundRole).value<FoundRoom>());
        }
    });

    m_roomView = new QWidget(split);
    auto* roomLayout = new QVBoxLayout(m_roomView);
    scaled::margins(roomLayout, 4, 4, 4, 4);
    scaled::spacing(roomLayout, 4);
    m_roomTitle = new QLabel(m_roomView);
    m_roomTitle->setObjectName(QStringLiteral("peersRoomTitle"));
    m_roomTitle->setWordWrap(true);
    roomLayout->addWidget(m_roomTitle);
    m_roomSections = new QVBoxLayout;
    m_roomSections->setContentsMargins(0, 0, 0, 0);
    roomLayout->addLayout(m_roomSections);
    auto* roomSplit = new QSplitter(Qt::Vertical, m_roomView);
    m_people = new QListWidget(roomSplit);
    m_people->setObjectName(QStringLiteral("peersPeople"));
    m_people->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_people, &QListWidget::customContextMenuRequested, this, &PeersSidePanel::personMenu);
    m_chat = new QTextBrowser(roomSplit);
    m_chat->setObjectName(QStringLiteral("peersChat"));
    m_chat->setOpenLinks(false);
    connect(m_chat, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == u"prim" && m_controller->hooks().selectPrim) {
            m_controller->hooks().selectPrim(url.path());
            return;
        }
        // A shared image's: what's here of it, by its bundle hash.
        const QString hash = url.path();
        const PeersController::ImageFile file = m_controller->imageFile(hash);
        const bool here = file.state == PeersController::ImageFile::State::Done && QFileInfo::exists(file.path);
        if (url.scheme() == u"image" || url.scheme() == u"open") {
            if (here) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(file.path));
            }
        } else if (url.scheme() == u"reveal") {
            if (here) {
                revealInFolder(file.path);
            }
        } else if (url.scheme() == u"fetch") {
            m_controller->fetchImage(m_room, hash);
        } else if (url.scheme() == u"accept" || url.scheme() == u"previewonly") {
            m_controller->acceptImage(m_room, hash, url.scheme() == u"accept");
        } else if (url.scheme() == u"decline") {
            m_controller->declineImage(hash);
        } else if (url.scheme() == u"stop") {
            m_controller->stopSharing(hash);
        } else if (url.scheme() == u"shared" && m_room) {
            // Shown in its folder, never run: the path is what the sender typed.
            for (const PeersController::ChatLine& line : m_controller->chat(m_room)) {
                if (line.image && line.image->preview == hash) {
                    if (QFileInfo::exists(line.image->sharedPath)) {
                        revealInFolder(line.image->sharedPath);
                    } else {
                        QMessageBox::information(this, tr("Shared Image"),
                                                 tr("%1 isn't reachable from here.")
                                                     .arg(QDir::toNativeSeparators(line.image->sharedPath)));
                    }
                    break;
                }
            }
        }
    });
    roomSplit->setStretchFactor(0, 1);
    roomSplit->setStretchFactor(1, 3);
    roomLayout->addWidget(roomSplit, 1);
    auto* sayRow = new QHBoxLayout;
    m_say = new QLineEdit(m_roomView);
    m_say->setObjectName(QStringLiteral("peersSay"));
    m_say->setPlaceholderText(tr("Message; a prim path like /World/Cube becomes a link"));
    m_send = new QPushButton(tr("Send"), m_roomView);
    // Emoji, without the system's picker (Win + . works too): the recent ones first.
    m_emoji = new QToolButton(m_roomView);
    m_emoji->setObjectName(QStringLiteral("peersEmoji"));
    m_emoji->setText(QStringLiteral("🙂"));
    m_emoji->setToolTip(tr("Emoji"));
    m_emoji->setAutoRaise(true);
    m_emoji->setPopupMode(QToolButton::InstantPopup);
    auto* emojiMenu = new QMenu(m_emoji);
    emojiMenu->setObjectName(QStringLiteral("peersEmojiMenu"));
    connect(emojiMenu, &QMenu::aboutToShow, this, [this, emojiMenu] { fillEmojiMenu(emojiMenu); });
    m_emoji->setMenu(emojiMenu);
    sayRow->addWidget(m_say, 1);
    sayRow->addWidget(m_emoji);
    sayRow->addWidget(m_send);
    roomLayout->addLayout(sayRow);
    const auto send = [this] {
        m_controller->say(m_room, m_say->text());
        m_say->clear();
    };
    connect(m_say, &QLineEdit::returnPressed, this, send);
    connect(m_send, &QPushButton::clicked, this, send);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    setWidget(container);

    connect(controller, &PeersController::roomsChanged, this, &PeersSidePanel::rebuildRooms);
    connect(controller, &PeersController::peopleChanged, this, [this](Room* room) {
        if (room == m_room) {
            rebuildPeople();
        }
        rebuildRooms();
    });
    connect(controller, &PeersController::chatAdded, this, [this](Room* room, const PeersController::ChatLine& line) {
        if (room == m_room) {
            appendLine(line);
        }
    });
    connect(controller, &PeersController::chatReset, this, [this](Room* room) {
        if (room == m_room) {
            rebuildChat();
        }
    });
    connect(controller, &PeersController::imageChanged, this, [this](const QString& hash) {
        if (!m_room) {
            return;
        }
        for (const PeersController::ChatLine& line : m_controller->chat(m_room)) {
            if (line.image && (line.image->preview == hash || line.image->original == hash)) {
                rebuildChat();
                return;
            }
        }
    });
    connect(controller->peers(), &Peers::roomAdded, this, [this](Room* room) { showRoom(room); });
    connect(controller->peers(), &Peers::roomRemoved, this, [this](Room* room) {
        if (room == m_room) {
            const QList<Room*> rooms = m_controller->chatRooms();
            showRoom(rooms.isEmpty() ? nullptr : rooms.first());
        }
    });
    connect(this, &QDockWidget::visibilityChanged, this, &PeersSidePanel::updateShown);
    connect(this, &SidePanel::collapsedChanged, this, &PeersSidePanel::updateShown);
    rebuildRooms();
    showRoom(nullptr);
}

void PeersSidePanel::updateShown() {
    m_controller->setPanelShown(isVisible() && !isCollapsed() && !visibleRegion().isEmpty());
}

void PeersSidePanel::rebuildRooms() {
    const Profile profile = m_controller->peers()->profile();
    m_you->setText(tr("You: %1").arg(profile.name.toHtmlEscaped()));
    m_you->setToolTip(profile.identity.isValid() ? tr("Your key: %1").arg(shortFingerprint(profile.identity.fingerprint()))
                                                 : tr("Your key is made when you first host or join a room"));

    const QSignalBlocker blocker(m_rooms);
    m_rooms->clear();
    const auto section = [this](const QString& title) {
        auto* item = new QTreeWidgetItem(m_rooms, {title});
        QFont font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);
        item->setFlags(Qt::ItemIsEnabled);
        return item;
    };
    QTreeWidgetItem* current = nullptr;
    const QList<Room*> rooms = m_controller->chatRooms();
    QTreeWidgetItem* yours = section(tr("Your rooms"));
    for (Room* room : rooms) {
        auto* item = new QTreeWidgetItem(yours, {room->name().isEmpty() ? room->address() : room->name(), stateText(room)});
        item->setData(0, RoomRole, QVariant::fromValue<QObject*>(room));
        item->setIcon(0, dot(room->hostPeer().color));
        item->setToolTip(0, tr("Hosted by %1").arg(room->hostPeer().name));
        if (room == m_room) {
            current = item;
        }
    }
    if (rooms.isEmpty()) {
        auto* none = new QTreeWidgetItem(yours, {tr("None yet: host one, or join one below")});
        none->setFlags(Qt::ItemIsEnabled);
        none->setForeground(0, palette().placeholderText());
    }
    const QList<FoundRoom> found = m_controller->peers()->foundRooms(RoomType::Chat);
    for (const FoundRoom::Where where : {FoundRoom::Where::ThisMachine, FoundRoom::Where::Network}) {
        QTreeWidgetItem* header = nullptr;
        for (const FoundRoom& room : found) {
            if (room.where != where) {
                continue;
            }
            if (!header) {
                header = section(where == FoundRoom::Where::ThisMachine ? tr("On this machine") : tr("On this network"));
            }
            const Announcement& a = room.announcement;
            auto* item = new QTreeWidgetItem(header, {tr("%1 — %2").arg(a.name, a.hostName), admissionText(a.admission, a.password)});
            item->setData(0, FoundRole, QVariant::fromValue(room));
            item->setToolTip(0, tr("%1 · key %2 · double-click to join").arg(room.address, shortFingerprint(a.fingerprint)));
        }
    }
    NetworkDiscovery* network = m_controller->peers()->network();
    if (network && !network->problem().isEmpty()) {
        auto* problem = new QTreeWidgetItem(m_rooms, {tr("Can't look on the network: %1").arg(network->problem())});
        problem->setFlags(Qt::ItemIsEnabled);
    } else if (!m_controller->peers()->isLooking()) {
        auto* note = new QTreeWidgetItem(m_rooms, {tr("Not looking on the network (Settings > Peers)")});
        note->setFlags(Qt::ItemIsEnabled);
        note->setForeground(0, palette().placeholderText());
    }
    m_rooms->expandAll();
    if (current) {
        m_rooms->setCurrentItem(current);
    }
}

void PeersSidePanel::showRoom(Room* room) {
    m_room = room;
    m_roomView->setEnabled(room != nullptr);
    if (!room) {
        m_roomTitle->setText(tr("Host a room, or join one, to chat with the people in it."));
    } else {
        QString title = QStringLiteral("<b>%1</b>").arg(room->name().toHtmlEscaped());
        if (room->isHost()) {
            title += tr(" · %1").arg(admissionText(room->admission(), room->hasPassword()));
        } else if (room->hostPeer().isValid()) {
            title += tr(" · hosted by %1").arg(room->hostPeer().name.toHtmlEscaped());
        }
        m_roomTitle->setText(title);
    }
    rebuildPeople();
    rebuildChat();
    rebuildRooms();
    Q_EMIT roomShown(room);
}

void PeersSidePanel::addRoomSection(QWidget* section) {
    m_roomSections->addWidget(section);
}

void PeersSidePanel::rebuildPeople() {
    m_people->clear();
    if (!m_room) {
        return;
    }
    for (const Peer& peer : m_room->peers()) {
        QString text = peer.name;
        if (peer.id == m_room->me().id) {
            text += tr(" (you)");
        }
        if (peer.kind == Peer::Kind::Host) {
            text += tr(" · host");
        } else if (peer.kind == Peer::Kind::Guest) {
            text += tr(" · guest");
        }
        const QString stage = m_controller->stageOf(m_room, peer);
        if (!stage.isEmpty()) {
            text += QStringLiteral(" — ") + stage;
        }
        auto* item = new QListWidgetItem(dot(peer.color), text, m_people);
        item->setData(PeerRole, peer.id);
        QString tip = peer.app;
        if (!peer.fingerprint.isEmpty()) {
            tip += (tip.isEmpty() ? QString() : QStringLiteral(" · ")) + tr("key %1").arg(shortFingerprint(peer.fingerprint));
        }
        if (m_controller->isMuted(peer)) {
            tip += tr(" · muted");
        }
        item->setToolTip(tip);
    }
}

void PeersSidePanel::rebuildChat() {
    // Where it was, unless at the end, which it stays at.
    QScrollBar* bar = m_chat->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum();
    const int was = bar->value();
    m_chat->clear();
    if (!m_room) {
        return;
    }
    for (const PeersController::ChatLine& line : m_controller->chat(m_room)) {
        appendLine(line);
    }
    bar->setValue(atEnd ? bar->maximum() : was);
}

void PeersSidePanel::appendLine(const PeersController::ChatLine& line) {
    const QString time = line.when.toString(QStringLiteral("HH:mm"));
    if (!line.from.isValid()) {
        m_chat->append(QStringLiteral("<span style=\"color:#8a8d93\">%1 · <i>%2</i></span>")
                           .arg(time, line.text.toHtmlEscaped()));
        return;
    }
    const QString color = line.from.color.isEmpty() ? QStringLiteral("#dfe1e5") : line.from.color;
    QString name = line.from.name.toHtmlEscaped();
    if (!line.to.isEmpty() && m_room) {
        // To one member: whom, from the sender's side; "to you" on the other.
        const bool mine = line.from.id == m_room->me().id;
        const Peer to = m_room->peer(line.to);
        name += QStringLiteral(" <span style=\"color:#8a8d93\">→ %1</span>")
                    .arg(mine ? (to.isValid() ? to.name : tr("someone")).toHtmlEscaped() : tr("you"));
    }
    QString html = QStringLiteral("<span style=\"color:#8a8d93\">%1</span> <b style=\"color:%2\">%3</b> %4")
                       .arg(time, color, name, chatHtml(line.text));
    if (line.image) {
        html += imageHtml(m_controller, *line.image, m_room && line.from.id == m_room->me().id);
    }
    m_chat->append(html);
}

Room* joinFoundRoom(PeersController* controller, QWidget* parent, const FoundRoom& room) {
    QString password;
    if (room.announcement.password) {
        bool ok = false;
        password = QInputDialog::getText(parent, PeersSidePanel::tr("Join %1").arg(room.announcement.name),
                                         PeersSidePanel::tr("The room's password:"), QLineEdit::Password, {}, &ok);
        if (!ok) {
            return nullptr;
        }
    }
    return controller->join(room, password);
}

Room* hostRoomDialog(PeersController* controller, QWidget* parent, RoomType type) {
    const auto tr = [](const char* text) { return PeersSidePanel::tr(text); };
    const bool queue = type == RoomType::RenderQueue;
    QDialog dialog(parent);
    dialog.setWindowTitle(queue ? tr("Host Render Queue") : tr("Host Room"));
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(&dialog);
    name->setPlaceholderText((queue ? tr("%1's render queue") : tr("%1's room")).arg(controller->peers()->profile().name));
    auto* who = new QComboBox(&dialog);
    who->addItem(tr("Only those with the invite link"), int(Admission::Invited));
    who->addItem(tr("Anyone on this network, once I allow them"), int(Admission::Ask));
    who->addItem(tr("Anyone on this network (a public room)"), int(Admission::Open));
    who->setCurrentIndex(1);
    auto* password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText(tr("Optional; the invite link needs none"));
    auto* note = new QLabel(QString(queue ? tr("Instances in the room can send each other render jobs; each says in "
                                                "the Swarm panel whether it takes them. ")
                                             : QString()) +
                                tr("Others on this network can reach the room, so Windows may ask whether to let usdcc "
                                   "through its firewall the first time. Everything said in the room is encrypted."),
                            &dialog);
    note->setWordWrap(true);
    note->setObjectName(QStringLiteral("propertyNote"));
    form->addRow(tr("Name"), name);
    form->addRow(tr("Who can join"), who);
    form->addRow(tr("Password"), password);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Host"));
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return nullptr;
    }
    QString error;
    PeersController::HostRequest request{name->text(), Admission(who->currentData().toInt()), password->text()};
    request.type = type;
    Room* room = controller->host(request, &error);
    if (!room) {
        QMessageBox::warning(parent, dialog.windowTitle(), tr("Couldn't host the room: %1").arg(error));
        return nullptr;
    }
    QApplication::clipboard()->setText(room->link());
    return room;
}

Room* joinRoomDialog(PeersController* controller, QWidget* parent, RoomType type) {
    const auto tr = [](const char* text) { return PeersSidePanel::tr(text); };
    QDialog dialog(parent);
    dialog.setWindowTitle(type == RoomType::RenderQueue ? tr("Join Render Queue") : tr("Join Room"));
    auto* form = new QFormLayout(&dialog);
    auto* link = new QLineEdit(&dialog);
    link->setPlaceholderText(tr("usdcc://join/… or 192.168.1.20:47800"));
    scaled::minimumWidth(link, 360);
    const QString clipboard = QApplication::clipboard()->text().trimmed();
    if (clipboard.startsWith(QStringLiteral("usdcc://join/"), Qt::CaseInsensitive)) {
        link->setText(clipboard);
    }
    auto* password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText(tr("Only for an address, if the room has one"));
    form->addRow(tr("Invite link or address"), link);
    form->addRow(tr("Password"), password);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Join"));
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return nullptr;
    }
    QString error;
    Room* room = controller->joinLinkOrAddress(link->text(), password->text(), &error, type);
    if (!room) {
        QMessageBox::warning(parent, dialog.windowTitle(), error);
    }
    return room;
}

void PeersSidePanel::joinFound(const FoundRoom& room) {
    joinFoundRoom(m_controller, this, room);
}

void PeersSidePanel::hostRoom() {
    if (Room* room = hostRoomDialog(m_controller, this, RoomType::Chat)) {
        showRoom(room);
    }
}

void PeersSidePanel::joinRoom() {
    joinRoomDialog(m_controller, this, RoomType::Chat);
}

void PeersSidePanel::roomMenu(const QPoint& position) {
    QTreeWidgetItem* item = m_rooms->itemAt(position);
    if (!item) {
        return;
    }
    QMenu menu(this);
    if (item->data(0, FoundRole).isValid()) {
        const FoundRoom found = item->data(0, FoundRole).value<FoundRoom>();
        menu.addAction(tr("Join"), this, [this, found] { joinFound(found); });
    } else if (item->data(0, RoomRole).isValid()) {
        QPointer<Room> room = static_cast<Room*>(item->data(0, RoomRole).value<QObject*>());
        if (room->isHost()) {
            menu.addAction(tr("Copy Invite Link"), this, [room] {
                if (room) {
                    QApplication::clipboard()->setText(room->link());
                }
            });
            menu.addSeparator();
        }
        QAction* mute = menu.addAction(tr("Mute"), this, [this, room](bool on) {
            if (room) {
                m_controller->setMuted(room, on);
            }
        });
        mute->setCheckable(true);
        mute->setChecked(m_controller->isMuted(room));
        menu.addSeparator();
        menu.addAction(room->isHost() ? tr("Close Room") : tr("Leave Room"), this, [room] {
            if (room) {
                room->close();
            }
        });
    } else {
        return;
    }
    menu.exec(m_rooms->viewport()->mapToGlobal(position));
}

namespace {

const QString kRecentEmojiKey = QStringLiteral("Peers/recentEmoji");

}  // namespace

QStringList PeersSidePanel::emojiChoices() {
    return {
        // Reactions
        QStringLiteral("👍"), QStringLiteral("👎"), QStringLiteral("👏"), QStringLiteral("🙌"),
        QStringLiteral("🙏"), QStringLiteral("👋"), QStringLiteral("👀"), QStringLiteral("💪"),
        // Faces
        QStringLiteral("😀"), QStringLiteral("😂"), QStringLiteral("🙂"), QStringLiteral("😉"),
        QStringLiteral("😍"), QStringLiteral("🤔"), QStringLiteral("😅"), QStringLiteral("😬"),
        QStringLiteral("😮"), QStringLiteral("🤯"), QStringLiteral("😢"), QStringLiteral("😴"),
        // Signs
        QStringLiteral("✅"), QStringLiteral("❌"), QStringLiteral("⚠️"), QStringLiteral("❓"),
        QStringLiteral("❗"), QStringLiteral("💡"), QStringLiteral("🔥"), QStringLiteral("🎉"),
        QStringLiteral("❤️"), QStringLiteral("⭐"), QStringLiteral("🚀"), QStringLiteral("⏱️"),
        // Work
        QStringLiteral("🎬"), QStringLiteral("🎨"), QStringLiteral("🎥"), QStringLiteral("📷"),
        QStringLiteral("🧊"), QStringLiteral("💾"), QStringLiteral("🐛"), QStringLiteral("☕"),
    };
}

QStringList PeersSidePanel::recentEmoji() {
    return appSettings().value(kRecentEmojiKey).toStringList().mid(0, kRecentEmoji);
}

void PeersSidePanel::insertEmoji(const QString& emoji) {
    m_say->insert(emoji);
    m_say->setFocus();
    QStringList recent = recentEmoji();
    recent.removeAll(emoji);
    recent.prepend(emoji);
    QSettings settings = appSettings();
    settings.setValue(kRecentEmojiKey, recent.mid(0, kRecentEmoji));
}

void PeersSidePanel::fillEmojiMenu(QMenu* menu) {
    menu->clear();
    auto* grid = new QWidget(menu);
    auto* layout = new QVBoxLayout(grid);
    scaled::margins(layout, 6, 6, 6, 6);
    scaled::spacing(layout, 4);
    constexpr int kColumns = 8;
    const auto addRow = [&](const QString& title, const QStringList& emoji) {
        auto* label = new QLabel(title, grid);
        label->setObjectName(QStringLiteral("propertyNote"));
        layout->addWidget(label);
        auto* cells = new QGridLayout;
        cells->setSpacing(0);
        for (qsizetype i = 0; i < emoji.size(); ++i) {
            auto* button = new QToolButton(grid);
            button->setText(emoji[i]);
            // Styled by the theme (dark.qss): no padding, a larger font.
            button->setObjectName(QStringLiteral("peersEmojiCell"));
            button->setAutoRaise(true);
            scaled::fixedSize(button, 32, 32);
            const QString chosen = emoji[i];
            connect(button, &QToolButton::clicked, this, [this, menu, chosen] {
                insertEmoji(chosen);
                menu->close();
            });
            cells->addWidget(button, int(i / kColumns), int(i % kColumns));
        }
        layout->addLayout(cells);
    };
    const QStringList recent = recentEmoji();
    if (!recent.isEmpty()) {
        addRow(tr("Recent"), recent);
    }
    addRow(tr("Emoji"), emojiChoices());
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(grid);
    menu->addAction(action);
}

void PeersSidePanel::personMenu(const QPoint& position) {
    QListWidgetItem* item = m_people->itemAt(position);
    if (!item || !m_room) {
        return;
    }
    const Peer peer = m_room->peer(item->data(PeerRole).toString());
    if (!peer.isValid() || peer.id == m_room->me().id) {
        return;
    }
    QMenu menu(this);
    QAction* mute = menu.addAction(tr("Mute %1").arg(peer.name), this,
                                   [this, peer](bool on) { m_controller->setMuted(peer, on); });
    mute->setCheckable(true);
    mute->setChecked(m_controller->isMuted(peer));
    mute->setEnabled(!peer.fingerprint.isEmpty());
    if (m_room->isHost() && peer.kind != Peer::Kind::Host) {
        menu.addSeparator();
        if (!peer.fingerprint.isEmpty()) {
            menu.addAction(tr("Always Allow %1").arg(peer.name), this, [this, peer] { m_controller->trust(peer); });
        }
        QPointer<Room> room = m_room;
        menu.addAction(tr("Remove %1").arg(peer.name), this, [room, peer] {
            if (room) {
                room->remove(peer.id);
            }
        });
    }
    menu.exec(m_people->viewport()->mapToGlobal(position));
}

}  // namespace usdcc::ui
