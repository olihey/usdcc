#include "usdcc/ui/peers_side_panel.h"

#include "usdcc/peers/room.h"
#include "usdcc/ui/log_model.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QActionGroup>
#include <QSplitter>
#include <QTabBar>
#include <QKeyEvent>
#include <QTextBlock>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QToolButton>
#include <QTreeWidget>
#include <QWidgetAction>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <optional>

namespace usdcc::ui {

using namespace usdcc::peers;

namespace {

enum Role { RoomRole = Qt::UserRole + 1, FoundRole, PeerRole };

// A person's color, with whether they are around as a small mark at its foot.
QIcon dot(const QString& color, std::optional<PeersController::Status> status = std::nullopt) {
    QPixmap pixmap(28, 28);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(color.isEmpty() ? QStringLiteral("#8a8d93") : color));
    painter.drawEllipse(QRectF(1, 1, 10, 10));
    if (status) {
        const QColor mark = *status == PeersController::Status::DoNotDisturb ? QColor(0xe5, 0x48, 0x4d)
                            : *status == PeersController::Status::Away       ? QColor(0xf0, 0xb2, 0x32)
                                                                             : QColor(0x4c, 0xc3, 0x8a);
        painter.setPen(QPen(QColor(0x1b, 0x1b, 0x1e), 1.5));
        painter.setBrush(mark);
        painter.drawEllipse(QRectF(7.5, 7.5, 5.5, 5.5));
    }
    return QIcon(pixmap);
}

QString statusText(PeersController::Status status) {
    switch (status) {
    case PeersController::Status::Away:
        return QObject::tr("away");
    case PeersController::Status::DoNotDisturb:
        return QObject::tr("do not disturb");
    case PeersController::Status::Active:
        break;
    }
    return QObject::tr("active");
}

// A log level's color as the Log panel shows it, made opaque over the chat's background.
QString levelColor(const QString& level, const QColor& background) {
    core::LogLevel parsed = core::LogLevel::Info;
    core::parseLogLevel(level.toStdString(), &parsed);
    const QColor color = LogModel::levelColor(parsed);
    const double a = color.alphaF();
    return QColor::fromRgbF(float(color.redF() * a + background.redF() * (1 - a)),
                            float(color.greenF() * a + background.greenF() * (1 - a)),
                            float(color.blueF() * a + background.blueF() * (1 - a)))
        .name();
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

// The message: written over lines, Enter sends it and Shift+Enter starts a new one.
// It grows with what is written, up to kMaxLines, and scrolls after that.
class ChatInput : public QPlainTextEdit {
public:
    static constexpr int kMaxLines = 6;

    explicit ChatInput(QWidget* parent) : QPlainTextEdit(parent) {
        setTabChangesFocus(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(document(), &QTextDocument::contentsChanged, this, [this] { fit(); });
        fit();
    }

    std::function<void()> onSend;
    // Told how tall one line is, now and whenever that changes: what sizes the buttons beside it.
    std::function<void(int)> onLineHeight;

    // How tall it is for `lines` lines of text: theirs, the document's margin, and what is
    // around the text (frame, the theme's padding), measured once it is laid out.
    int heightFor(int lines) const {
        int around = contentsMargins().top() + contentsMargins().bottom();
        if (testAttribute(Qt::WA_WState_Polished) && viewport()->height() > 0 && viewport()->height() < height()) {
            around = height() - viewport()->height();
        }
        return lines * fontMetrics().lineSpacing() + int(2 * document()->documentMargin()) + around;
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
        if (enter && !(event->modifiers() & Qt::ShiftModifier)) {
            if (onSend) {
                onSend();
            }
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override {
        QPlainTextEdit::resizeEvent(event);
        fit();  // Wrapped lines change with the width
    }

    void changeEvent(QEvent* event) override {
        QPlainTextEdit::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
            fit();
        }
    }

private:
    void fit() {
        int lines = 0;
        for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
            lines += std::max(1, block.layout() ? block.layout()->lineCount() : 1);
        }
        setVerticalScrollBarPolicy(lines > kMaxLines ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        const int height = heightFor(std::clamp(lines, 1, kMaxLines));
        if (height != this->height()) {
            setFixedHeight(height);
        }
        const int oneLine = heightFor(1);
        if (oneLine != m_oneLine) {
            m_oneLine = oneLine;
            if (onLineHeight) {
                onLineHeight(oneLine);
            }
        }
    }

    int m_oneLine = 0;
};

// A button beside the message, as wide as it is tall. Its size is asked of it (the
// theme's min-height would override a fixed one).
class SquareButton : public QToolButton {
public:
    using QToolButton::QToolButton;

    void setSide(int side) {
        m_side = side;
        updateGeometry();
    }
    QSize sizeHint() const override { return {m_side, m_side}; }
    QSize minimumSizeHint() const override { return sizeHint(); }

private:
    int m_side = 24;
};

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
    // You, and whether you are around: Do not disturb is chosen here, Away comes by itself.
    m_you = new QToolButton(toolbar);
    m_you->setObjectName(QStringLiteral("peersYou"));
    m_you->setAutoRaise(true);
    m_you->setPopupMode(QToolButton::InstantPopup);
    m_you->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto* youMenu = new QMenu(m_you);
    youMenu->setObjectName(QStringLiteral("peersYouMenu"));
    auto* statusGroup = new QActionGroup(youMenu);
    QAction* available = youMenu->addAction(tr("Available"));
    QAction* dnd = youMenu->addAction(tr("Do Not Disturb"));
    for (QAction* action : {available, dnd}) {
        action->setCheckable(true);
        statusGroup->addAction(action);
    }
    dnd->setToolTip(tr("No chat toasts; the others see you don't want to be disturbed"));
    connect(youMenu, &QMenu::aboutToShow, this, [this, available, dnd] {
        (m_controller->isDoNotDisturb() ? dnd : available)->setChecked(true);
    });
    connect(available, &QAction::triggered, this, [this] { m_controller->setDoNotDisturb(false); });
    connect(dnd, &QAction::triggered, this, [this] { m_controller->setDoNotDisturb(true); });
    m_you->setMenu(youMenu);
    auto* host = new QPushButton(tr("Host Room…"), toolbar);
    host->setObjectName(QStringLiteral("peersHost"));
    auto* join = new QPushButton(tr("Join…"), toolbar);
    join->setObjectName(QStringLiteral("peersJoin"));
    toolbarLayout->addWidget(m_you);
    toolbarLayout->addStretch(1);
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
    connect(m_people, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (m_room && item) {
            openConversation(m_room->peer(item->data(PeerRole).toString()));
        }
    });
    // The chat: the room's, and a tab for each private conversation over it; who is
    // writing under it.
    auto* chatArea = new QWidget(roomSplit);
    auto* chatLayout = new QVBoxLayout(chatArea);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(0);
    m_tabs = new QTabBar(chatArea);
    m_tabs->setObjectName(QStringLiteral("peersChatTabs"));
    m_tabs->setTabsClosable(true);
    m_tabs->setExpanding(false);
    m_tabs->setDocumentMode(true);
    m_tabs->setDrawBase(false);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->hide();
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index < 0) {
            return;
        }
        m_with = m_tabs->tabData(index).toString();
        if (!m_with.isEmpty()) {
            m_controller->markRead(m_room, m_with);
        }
        rebuildChat();
        updateTyping();
        updateSayState();
    });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, [this](int index) {
        const QString with = m_tabs->tabData(index).toString();
        if (!with.isEmpty()) {
            m_controller->closeConversation(m_room, with);
        }
    });
    chatLayout->addWidget(m_tabs);
    m_chat = new QTextBrowser(chatArea);
    m_chat->setObjectName(QStringLiteral("peersChat"));
    m_chat->setOpenLinks(false);
    connect(m_chat, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == u"prim" && m_controller->hooks().selectPrim) {
            m_controller->hooks().selectPrim(url.path());
        }
    });
    chatLayout->addWidget(m_chat, 1);
    m_typing = new QLabel(chatArea);
    m_typing->setObjectName(QStringLiteral("peersTyping"));
    m_typing->setMinimumHeight(m_typing->fontMetrics().height());
    chatLayout->addWidget(m_typing);
    roomSplit->setStretchFactor(0, 1);
    roomSplit->setStretchFactor(1, 3);
    roomLayout->addWidget(roomSplit, 1);
    auto* sayRow = new QHBoxLayout;
    scaled::spacing(sayRow, 4);
    auto* say = new ChatInput(m_roomView);
    m_say = say;
    m_say->setObjectName(QStringLiteral("peersSay"));
    m_say->setPlaceholderText(tr("Message; Shift+Enter for a new line, a prim path like /World/Cube becomes a link"));
    say->onSend = [this] { send(); };
    // Emoji, without the system's picker (Win + . works too): the recent ones first.
    auto* emoji = new SquareButton(m_roomView);
    m_emoji = emoji;
    m_emoji->setObjectName(QStringLiteral("peersEmoji"));
    m_emoji->setText(QStringLiteral("🙂"));
    m_emoji->setToolTip(tr("Emoji"));
    m_emoji->setPopupMode(QToolButton::InstantPopup);
    auto* emojiMenu = new QMenu(m_emoji);
    emojiMenu->setObjectName(QStringLiteral("peersEmojiMenu"));
    connect(emojiMenu, &QMenu::aboutToShow, this, [this, emojiMenu] { fillEmojiMenu(emojiMenu); });
    m_emoji->setMenu(emojiMenu);
    auto* sendButton = new SquareButton(m_roomView);
    m_send = sendButton;
    m_send->setObjectName(QStringLiteral("peersSend"));
    m_send->setIcon(QIcon(QStringLiteral(":/usdcc/theme/send.png")));
    scaled::iconSize(m_send, 16, 16);
    m_send->setToolTip(tr("Send (Enter)"));
    // Square, as tall as the message is for one line; they stay at its foot as it grows.
    for (SquareButton* button : {emoji, sendButton}) {
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    say->onLineHeight = [emoji, sendButton](int side) {
        emoji->setSide(side);
        sendButton->setSide(side);
    };
    say->ensurePolished();
    say->onLineHeight(say->heightFor(1));
    sayRow->addWidget(m_say, 1, Qt::AlignBottom);
    sayRow->addWidget(m_emoji, 0, Qt::AlignBottom);
    sayRow->addWidget(m_send, 0, Qt::AlignBottom);
    roomLayout->addLayout(sayRow);
    connect(m_send, &QToolButton::clicked, this, &PeersSidePanel::send);
    connect(m_say, &QPlainTextEdit::textChanged, this, [this] {
        if (m_say->document()->isEmpty()) {
            m_controller->stoppedTyping(m_room, m_with);
        } else {
            m_controller->typing(m_room, m_with);
        }
    });
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    setWidget(container);

    connect(controller, &PeersController::roomsChanged, this, &PeersSidePanel::rebuildRooms);
    connect(controller, &PeersController::peopleChanged, this, [this](Room* room) {
        if (room == m_room) {
            rebuildPeople();
            rebuildTabs();
            updateSayState();
        }
        rebuildRooms();
    });
    connect(controller, &PeersController::conversationsChanged, this, [this](Room* room) {
        if (room == m_room) {
            rebuildTabs();
        }
    });
    connect(controller, &PeersController::typingChanged, this, [this](Room* room, const QString& with) {
        if (room == m_room && with == m_with) {
            updateTyping();
        }
    });
    connect(controller, &PeersController::statusChanged, this, &PeersSidePanel::updateYou);
    connect(controller, &PeersController::roomRenamed, this, [this](Room* room) {
        if (room == m_room) {
            updateTitle();
        }
    });
    connect(controller, &PeersController::chatAdded, this,
            [this](Room* room, const QString& with, const PeersController::ChatLine& line) {
                if (room != m_room || with != m_with) {
                    return;
                }
                appendLine(line);
                if (!with.isEmpty() && isVisible()) {
                    m_controller->markRead(room, with);  // Read as it arrives
                }
            });
    connect(controller, &PeersController::chatReset, this, [this](Room* room) {
        if (room == m_room) {
            rebuildChat();
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
    updateYou();
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

void PeersSidePanel::send() {
    if (m_with.isEmpty()) {
        m_controller->say(m_room, m_say->toPlainText());
    } else if (!m_controller->sayTo(m_room, m_with, m_say->toPlainText())) {
        return;  // They left: the message stays
    }
    m_say->clear();
}

void PeersSidePanel::updateYou() {
    const Profile profile = m_controller->peers()->profile();
    const PeersController::Status status = m_controller->status();
    m_you->setText(tr("You: %1").arg(profile.name));
    m_you->setIcon(dot(profile.color, status));
    QString tip = tr("You are %1; click to change").arg(statusText(status));
    tip += u'\n' + (profile.identity.isValid() ? tr("Your key: %1").arg(shortFingerprint(profile.identity.fingerprint()))
                                              : tr("Your key is made when you first host or join a room"));
    m_you->setToolTip(tip);
}

void PeersSidePanel::rebuildTabs() {
    const QSignalBlocker blocker(m_tabs);
    while (m_tabs->count() > 0) {
        m_tabs->removeTab(0);
    }
    const QList<Peer> people = m_room ? m_controller->conversations(m_room) : QList<Peer>();
    m_tabs->addTab(tr("Everyone"));
    m_tabs->setTabData(0, QString());
    m_tabs->setTabButton(0, QTabBar::RightSide, nullptr);
    m_tabs->setTabButton(0, QTabBar::LeftSide, nullptr);
    int current = 0;
    for (const Peer& peer : people) {
        const int unread = m_controller->unread(m_room, peer.id);
        const bool here = m_room->peer(peer.id).isValid();
        const int index = m_tabs->addTab(dot(peer.color, here ? std::optional(m_controller->statusOf(m_room, peer))
                                                              : std::nullopt),
                                         unread > 0 ? tr("%1 (%2)").arg(peer.name).arg(unread) : peer.name);
        m_tabs->setTabData(index, peer.id);
        m_tabs->setTabToolTip(index, here ? tr("Private: only you and %1 can read this").arg(peer.name)
                                          : tr("%1 left the room").arg(peer.name));
        if (peer.id == m_with) {
            current = index;
        }
    }
    m_tabs->setCurrentIndex(current);
    m_tabs->setVisible(m_tabs->count() > 1);
    const QString with = m_tabs->tabData(current).toString();
    if (with != m_with) {  // Its tab closed: back to the room's chat
        m_with = with;
        rebuildChat();
        updateTyping();
        updateSayState();
    }
}

void PeersSidePanel::openConversation(const Peer& peer) {
    if (!m_room || !m_controller->canMessage(m_room, peer)) {
        return;
    }
    m_controller->openConversation(m_room, peer);
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (m_tabs->tabData(i).toString() == peer.id) {
            m_tabs->setCurrentIndex(i);
        }
    }
    m_say->setFocus();
}

void PeersSidePanel::showConversation(Room* room, const QString& with) {
    if (room != m_room) {
        showRoom(room);
    }
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (m_tabs->tabData(i).toString() == with) {
            m_tabs->setCurrentIndex(i);
        }
    }
}

void PeersSidePanel::updateTyping() {
    const QList<Peer> typists = m_room ? m_controller->typingIn(m_room, m_with) : QList<Peer>();
    QString text;
    if (typists.size() == 1) {
        text = tr("%1 is typing…").arg(typists[0].name);
    } else if (typists.size() == 2) {
        text = tr("%1 and %2 are typing…").arg(typists[0].name, typists[1].name);
    } else if (typists.size() > 2) {
        text = tr("Several people are typing…");
    }
    m_typing->setText(text);
}

void PeersSidePanel::updateSayState() {
    const Peer with = m_room && !m_with.isEmpty() ? m_room->peer(m_with) : Peer();
    const bool canWrite = m_with.isEmpty() || m_controller->canMessage(m_room, with);
    m_say->setEnabled(canWrite);
    m_emoji->setEnabled(canWrite);
    m_send->setEnabled(canWrite);
    if (m_with.isEmpty()) {
        m_say->setPlaceholderText(tr("Message; Shift+Enter for a new line, a prim path like /World/Cube becomes a link"));
    } else if (canWrite) {
        m_say->setPlaceholderText(tr("Private message to %1").arg(with.name));
    } else {
        m_say->setPlaceholderText(tr("They left the room"));
    }
}

void PeersSidePanel::updateTitle() {
    Room* room = m_room;
    if (!room) {
        m_roomTitle->setText(tr("Host a room, or join one, to chat with the people in it."));
        return;
    }
    QString title = QStringLiteral("<b>%1</b>").arg(room->name().toHtmlEscaped());
    if (room->isHost()) {
        title += tr(" · %1").arg(admissionText(room->admission(), room->hasPassword()));
    } else if (room->hostPeer().isValid()) {
        title += tr(" · hosted by %1").arg(room->hostPeer().name.toHtmlEscaped());
    }
    m_roomTitle->setText(title);
}

void PeersSidePanel::showRoom(Room* room) {
    m_room = room;
    m_with.clear();
    m_roomView->setEnabled(room != nullptr);
    updateTitle();
    rebuildPeople();
    rebuildTabs();
    rebuildChat();
    updateTyping();
    updateSayState();
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
        const PeersController::Status status = m_controller->statusOf(m_room, peer);
        auto* item = new QListWidgetItem(dot(peer.color, status), text, m_people);
        item->setData(PeerRole, peer.id);
        QString tip = statusText(status);
        tip += QStringLiteral(" · ") + peer.app;
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
    m_chat->clear();
    m_lastFrom.clear();
    if (!m_room) {
        return;
    }
    for (const PeersController::ChatLine& line : m_controller->chat(m_room, m_with)) {
        appendLine(line);
    }
}

void PeersSidePanel::appendLine(const PeersController::ChatLine& line) {
    // Grouped: a person's name and the time over what they said, indented below it. What
    // they say again soon after goes under the same name; what the room says ends a group.
    // The margins are the blocks' own: append() would drop a <div>'s.
    const auto add = [this](const QString& html, int left, int top) {
        QTextCursor cursor(m_chat->document());
        cursor.movePosition(QTextCursor::End);
        QTextBlockFormat format;
        format.setLeftMargin(left);
        format.setTopMargin(top);
        if (m_chat->document()->isEmpty()) {
            cursor.setBlockFormat(format);
        } else {
            cursor.insertBlock(format, QTextCharFormat());
        }
        cursor.insertHtml(html);
        m_chat->verticalScrollBar()->setValue(m_chat->verticalScrollBar()->maximum());
    };
    const QString time = line.when.toString(QStringLiteral("HH:mm"));
    const int gap = m_chat->document()->isEmpty() ? 0 : scaled::pixels(8);
    const QString timeHtml = QStringLiteral("<span style=\"color:#8a8d93; font-size:small\">%1</span>").arg(time);
    if (!line.from.isValid()) {
        add(QStringLiteral("<span style=\"color:#8a8d93\"><i>%1</i></span>&nbsp;&nbsp;%2")
                .arg(line.text.toHtmlEscaped(), timeHtml),
            0, gap);
        m_lastFrom.clear();
        return;
    }
    const bool sameGroup = line.from.id == m_lastFrom && m_lastWhen.isValid() &&
                           m_lastWhen.secsTo(line.when) < kChatGroupSeconds;
    if (!sameGroup) {
        const QString color = line.from.color.isEmpty() ? QStringLiteral("#dfe1e5") : line.from.color;
        add(QStringLiteral("<b style=\"color:%1\">%2</b>&nbsp;&nbsp;%3").arg(color, line.from.name.toHtmlEscaped(), timeHtml),
            0, gap);
    }
    if (line.records.isEmpty()) {
        add(chatHtml(line.text), scaled::pixels(12), sameGroup ? scaled::pixels(3) : scaled::pixels(1));
    } else {
        // Shared log messages: one line each, as the Log panel shows them, details below.
        add(QStringLiteral("<span style=\"color:#8a8d93\"><i>%1</i></span>")
                .arg((line.records.size() == 1 ? tr("shared a log message") : tr("shared %1 log messages").arg(line.records.size()))),
            scaled::pixels(12), sameGroup ? scaled::pixels(3) : scaled::pixels(1));
        const QColor background = m_chat->palette().color(QPalette::Base);
        for (const PeersController::SharedLogRecord& record : line.records) {
            add(QStringLiteral("<span style=\"font-family:Consolas,'Cascadia Mono',monospace; font-size:small\">"
                               "<span style=\"color:#8a8d93\">%1</span>&nbsp;<b style=\"color:%2\">%3</b>&nbsp;"
                               "<span style=\"color:#8a8d93\">%4</span>&nbsp;%5</span>")
                    .arg(record.time.toString(QStringLiteral("HH:mm:ss")), levelColor(record.level, background),
                         record.level.toUpper().toHtmlEscaped(), record.category.toHtmlEscaped(),
                         record.text.toHtmlEscaped().replace(u'\n', QStringLiteral("<br>"))),
                scaled::pixels(12), scaled::pixels(2));
            if (!record.details.isEmpty()) {
                add(QStringLiteral("<span style=\"font-family:Consolas,'Cascadia Mono',monospace; font-size:small; "
                                   "color:#8a8d93\">%1</span>")
                        .arg(record.details.toHtmlEscaped().replace(u'\n', QStringLiteral("<br>"))),
                    scaled::pixels(28), 0);
            }
        }
    }
    m_lastFrom = line.from.id;
    m_lastWhen = line.when;
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

void PeersSidePanel::renameRoom(Room* room) {
    if (!room || !room->isHost()) {
        return;
    }
    QPointer<Room> guarded = room;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Room"), tr("The room's new name:"), QLineEdit::Normal,
                                               room->name(), &ok);
    if (!ok || !guarded || name.trimmed().isEmpty()) {
        return;
    }
    guarded->rename(name);
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
            menu.addAction(tr("Rename Room…"), this, [this, room] { renameRoom(room); });
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
    m_say->insertPlainText(emoji);
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
    QAction* message = menu.addAction(tr("Message %1").arg(peer.name), this, [this, peer] { openConversation(peer); });
    message->setEnabled(m_controller->canMessage(m_room, peer));
    if (!message->isEnabled()) {
        message->setText(peer.kind == Peer::Kind::Guest ? tr("Message %1 (a guest can't be messaged privately)").arg(peer.name)
                                                        : tr("Message %1 (needs a newer usdcc)").arg(peer.name));
    }
    menu.addSeparator();
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
