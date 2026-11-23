#pragma once

// The Peers side panel (docs/PEERS.md §10.1): rooms this instance is in, rooms found on
// this machine and network, **Host Room…** and **Join…**, the people in a room with
// their colors and what they have open, and its chat.

#include "usdcc/core/export.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/ui/side_panel.h"

#include <QDateTime>
#include <QPointer>
#include <QStringList>

class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QTabBar;
class QToolButton;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::ui {

// The dialogs to host and join a room, for the Peers panel (chat rooms) and the Swarm panel
// (render queues). Each returns the room, or null when cancelled or it couldn't (and said so).
// Hosting copies the invite link.
USDCC_UI_API usdcc::peers::Room* hostRoomDialog(PeersController* controller, QWidget* parent,
                                                usdcc::peers::RoomType type = usdcc::peers::RoomType::Chat);
USDCC_UI_API usdcc::peers::Room* joinRoomDialog(PeersController* controller, QWidget* parent,
                                                usdcc::peers::RoomType type = usdcc::peers::RoomType::Chat);
// Asks for the password, if the room has one.
USDCC_UI_API usdcc::peers::Room* joinFoundRoom(PeersController* controller, QWidget* parent,
                                               const usdcc::peers::FoundRoom& room);

class USDCC_UI_API PeersSidePanel : public SidePanel {
    Q_OBJECT

public:
    explicit PeersSidePanel(PeersController* controller, QWidget* parent = nullptr);

    // The room whose people and chat show; null for none.
    usdcc::peers::Room* currentRoom() const { return m_room; }
    void showRoom(usdcc::peers::Room* room);

    // The dialogs, for **Host Room…** and **Join…**.
    void hostRoom();
    void joinRoom();
    // Asks the host for the room's new name (Room::rename()).
    void renameRoom(usdcc::peers::Room* room);

    // The conversation on show: empty for the room's chat, else the person of a private
    // one, each a tab over the chat.
    QString currentConversation() const { return m_with; }
    // Opens a private conversation with someone in the room on show, as a tab.
    void openConversation(const usdcc::peers::Peer& peer);
    // Shows that room, on that conversation (from a private message's toast).
    void showConversation(usdcc::peers::Room* room, const QString& with);

    // The message is written over lines: Enter sends it, Shift+Enter starts a new line.
    // The emoji button beside the message: puts `emoji` where the cursor is, and
    // first among the recent ones.
    void insertEmoji(const QString& emoji);
    // The emoji it offers, and the ones used last (kept in usdcc.ini), newest first.
    static QStringList emojiChoices();
    static QStringList recentEmoji();
    static constexpr int kRecentEmoji = 8;

    // A section of the room view, above its people: what another part of usdcc shows
    // for the room on show (the stages shared in it). roomShown() says which room.
    void addRoomSection(QWidget* section);

Q_SIGNALS:
    void roomShown(usdcc::peers::Room* room);

protected:
    bool event(QEvent* event) override;

private:
    void rebuildRooms();
    void rebuildPeople();
    void rebuildChat();
    void appendLine(const PeersController::ChatLine& line);
    void joinFound(const usdcc::peers::FoundRoom& room);
    void roomMenu(const QPoint& position);
    void personMenu(const QPoint& position);
    void updateShown();
    void fillEmojiMenu(QMenu* menu);
    void updateTitle();
    void updateYou();
    void rebuildTabs();
    void updateTyping();
    void updateSayState();
    void send();

    PeersController* m_controller;
    QPointer<usdcc::peers::Room> m_room;
    QToolButton* m_you = nullptr;
    QTabBar* m_tabs = nullptr;
    QLabel* m_typing = nullptr;
    QString m_with;
    QTreeWidget* m_rooms = nullptr;
    QLabel* m_roomTitle = nullptr;
    QListWidget* m_people = nullptr;
    QTextBrowser* m_chat = nullptr;
    QPlainTextEdit* m_say = nullptr;
    QToolButton* m_send = nullptr;
    QToolButton* m_emoji = nullptr;
    QWidget* m_roomView = nullptr;
    class QVBoxLayout* m_roomSections = nullptr;
    // The chat's last group: whose it is and when they last said something in it.
    QString m_lastFrom;
    QDateTime m_lastWhen;
    static constexpr int kChatGroupSeconds = 5 * 60;
};

// Turns chat text into HTML: escaped, with prim paths (/World/Cube) as links "prim:<path>".
USDCC_UI_API QString chatHtml(const QString& text);

}  // namespace usdcc::ui
