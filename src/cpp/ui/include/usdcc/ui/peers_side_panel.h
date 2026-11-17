#pragma once

// The Peers side panel (docs/PEERS.md §10.1): rooms this instance is in, rooms found on
// this machine and network, **Host Room…** and **Join…**, the people in a room with
// their colors and what they have open, and its chat.

#include "usdcc/core/export.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/ui/side_panel.h"

#include <QPointer>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::ui {

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

private:
    void rebuildRooms();
    void rebuildPeople();
    void rebuildChat();
    void appendLine(const PeersController::ChatLine& line);
    void joinFound(const usdcc::peers::FoundRoom& room);
    void roomMenu(const QPoint& position);
    void personMenu(const QPoint& position);
    void updateShown();

    PeersController* m_controller;
    QPointer<usdcc::peers::Room> m_room;
    QLabel* m_you = nullptr;
    QTreeWidget* m_rooms = nullptr;
    QLabel* m_roomTitle = nullptr;
    QListWidget* m_people = nullptr;
    QTextBrowser* m_chat = nullptr;
    QLineEdit* m_say = nullptr;
    QPushButton* m_send = nullptr;
    QWidget* m_roomView = nullptr;
};

// Turns chat text into HTML: escaped, with prim paths (/World/Cube) as links "prim:<path>".
USDCC_UI_API QString chatHtml(const QString& text);

}  // namespace usdcc::ui
