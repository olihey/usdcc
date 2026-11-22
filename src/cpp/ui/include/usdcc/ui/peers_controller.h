#pragma once

// Peers in the app (docs/PEERS.md, phase 2): the rooms this instance hosts and is in,
// chat in each, private chats between two people, who is there, what they have open and
// whether they are around, the questions to the host as toasts, and Settings > Peers.
// The Peers side panel (PeersSidePanel) shows it.

#include "usdcc/core/export.h"
#include "usdcc/core/log.h"
#include "usdcc/peers/peers.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>
#include <memory>

class QMenu;
class QTimer;
class QToolButton;

namespace usdcc::ui {

class MainWindow;

class USDCC_UI_API PeersController : public QObject {
    Q_OBJECT

public:
    static constexpr auto nameKey = "Peers/name";
    static constexpr auto colorKey = "Peers/color";
    static constexpr auto lookOnNetworkKey = "Peers/lookOnNetwork";
    static constexpr auto portKey = "Peers/port";
    static constexpr auto askBeforeLinkJoinKey = "Peers/askBeforeLinkJoin";
    static constexpr auto chatToastsKey = "Peers/chatToasts";
    static void defineSettings();

    struct Hooks {
        // A prim path in a chat message was clicked.
        std::function<void(const QString& path)> selectPrim;
        // What `presence` says this instance has open.
        std::function<QString()> currentStage;
    };

    // One log message as shared in a chat (the Log panel's Share).
    struct SharedLogRecord {
        QDateTime time;
        QString level;     // "info", "warning", ... (core::logLevelName)
        QString source;
        QString category;
        QString text;
        QString details;
    };
    // At most this many go in one message; the rest are left out, and the menu says so.
    static constexpr int kMaxSharedRecords = 50;
    static SharedLogRecord sharedRecord(const usdcc::core::LogRecord& record);

    struct ChatLine {
        usdcc::peers::Peer from;  // Invalid for what the room itself says ("Anna joined")
        QString text;
        QDateTime when;
        QList<SharedLogRecord> records;  // Log messages someone shared
    };

    // Whether someone is around (PEERS.md §10.1): from `presence`.
    enum class Status { Active, Away, DoNotDisturb };

    // `window` may be null (tests): then no toasts, no status bar and no idle watch.
    PeersController(std::unique_ptr<usdcc::peers::Peers> peers, MainWindow* window, Hooks hooks,
                    QObject* parent = nullptr);
    ~PeersController() override;

    usdcc::peers::Peers* peers() const { return m_peers.get(); }
    const Hooks& hooks() const { return m_hooks; }

    struct HostRequest {
        QString name;
        usdcc::peers::Admission admission = usdcc::peers::Admission::Ask;
        QString password;
        // On every network address; off, on 127.0.0.1 only (tests: no firewall question).
        bool onNetwork = true;
        // The Peers panel hosts chat rooms; the Swarm panel render queues.
        usdcc::peers::RoomType type = usdcc::peers::RoomType::Chat;
    };
    // On every network address (Windows asks whether to let it through, the first
    // time), on the port of Settings > Peers or any free one.
    usdcc::peers::Room* host(const HostRequest& request, QString* error = nullptr);
    usdcc::peers::Room* join(const usdcc::peers::FoundRoom& room, const QString& password = {});
    // A usdcc:// link, or "host:port" to ask the host.
    // `type`: the kind of room expected (a link to another kind is left with a toast).
    usdcc::peers::Room* joinLinkOrAddress(const QString& text, const QString& password, QString* error = nullptr,
                                          usdcc::peers::RoomType type = usdcc::peers::RoomType::Chat);
    // The chat rooms: what the Peers panel shows.
    QList<usdcc::peers::Room*> chatRooms() const;

    // A conversation: the room's, with `with` empty, or a private one with that person.
    QList<ChatLine> chat(usdcc::peers::Room* room, const usdcc::peers::PeerId& with = {}) const;
    void say(usdcc::peers::Room* room, const QString& text);
    // Sealed for that person alone (Channel::sendPrivate()); shown here at once. False if
    // they can't be messaged privately.
    bool sayTo(usdcc::peers::Room* room, const usdcc::peers::PeerId& with, const QString& text);
    // Log messages, to everyone in the room or privately to one person.
    bool shareLog(usdcc::peers::Room* room, const usdcc::peers::PeerId& with, QList<SharedLogRecord> records);
    // The Log panel's Share submenu: each chat room, and in it everyone or one person.
    void fillShareMenu(QMenu* menu, const QList<SharedLogRecord>& records);

    // Private conversations in a room, the people they are with (who may have left).
    QList<usdcc::peers::Peer> conversations(usdcc::peers::Room* room) const;
    // Someone can be messaged privately: in the room, not a guest, and with a key.
    bool canMessage(usdcc::peers::Room* room, const usdcc::peers::Peer& peer) const;
    void openConversation(usdcc::peers::Room* room, const usdcc::peers::Peer& peer);
    void closeConversation(usdcc::peers::Room* room, const usdcc::peers::PeerId& with);
    int unread(usdcc::peers::Room* room, const usdcc::peers::PeerId& with) const;
    void markRead(usdcc::peers::Room* room, const usdcc::peers::PeerId& with);

    // What a peer has open, and whether they are around, from `presence`.
    QString stageOf(usdcc::peers::Room* room, const usdcc::peers::Peer& peer) const;
    Status statusOf(usdcc::peers::Room* room, const usdcc::peers::Peer& peer) const;
    Status status() const;
    // Chosen by the user: no chat toasts, and the others see it. Kept in usdcc.ini.
    bool isDoNotDisturb() const;
    void setDoNotDisturb(bool on);
    // No input for a while, or the window minimized (the idle watch says so).
    bool isAway() const { return m_away; }
    void setAway(bool away);

    // This instance is writing in a conversation: the others in it see "… is typing".
    // Called on every change; sent at most every few seconds. stoppedTyping() on sending.
    void typing(usdcc::peers::Room* room, const usdcc::peers::PeerId& with);
    void stoppedTyping(usdcc::peers::Room* room, const usdcc::peers::PeerId& with);
    // Who is writing in a conversation now, besides this instance.
    QList<usdcc::peers::Peer> typingIn(usdcc::peers::Room* room, const usdcc::peers::PeerId& with) const;

    // Muted rooms and people get no toasts (PEERS.md §10.2).
    bool isMuted(usdcc::peers::Room* room) const;
    void setMuted(usdcc::peers::Room* room, bool muted);
    bool isMuted(const usdcc::peers::Peer& peer) const;
    void setMuted(const usdcc::peers::Peer& peer, bool muted);
    // "Always allow": never asked about again, in any room this instance hosts.
    void trust(const usdcc::peers::Peer& peer);

    // The panel shows: the network is asked for rooms only now (§5.2), and chat
    // messages don't need toasts.
    void setPanelShown(bool shown);
    // The Swarm panel looks for render queues on the network as the Peers panel does for rooms.
    void setSwarmShown(bool shown);
    // Tells every room what this instance has open now, and whether it is around.
    void updatePresence();
    // Opens the panel (from a toast or the status bar).
    std::function<void()> showPanel;
    // Opens it on a conversation (from a private message's toast).
    std::function<void(usdcc::peers::Room* room, const usdcc::peers::PeerId& with)> showConversation;

    bool eventFilter(QObject* watched, QEvent* event) override;

Q_SIGNALS:
    void roomsChanged();
    // `with`: empty for the room's chat, else the person of a private one.
    void chatAdded(usdcc::peers::Room* room, const QString& with, const usdcc::ui::PeersController::ChatLine& line);
    void chatReset(usdcc::peers::Room* room);
    void peopleChanged(usdcc::peers::Room* room);
    // The host gave it another name (Room::renamed).
    void roomRenamed(usdcc::peers::Room* room);
    // A private conversation was opened or closed, or its unread count changed.
    void conversationsChanged(usdcc::peers::Room* room);
    void typingChanged(usdcc::peers::Room* room, const QString& with);
    // This instance's own status (do not disturb, away).
    void statusChanged();

private:
    struct Conversation {
        usdcc::peers::Peer with;
        QList<ChatLine> lines;
        int unread = 0;
    };
    struct Typist {
        usdcc::peers::Room* room;
        QString with;  // The conversation, as this instance sees it
        usdcc::peers::Peer peer;
        QDateTime until;
    };

    void adopt(usdcc::peers::Room* room);
    void addLine(usdcc::peers::Room* room, const ChatLine& line);
    void addPrivateLine(usdcc::peers::Room* room, const usdcc::peers::Peer& with, const ChatLine& line);
    Conversation* conversation(usdcc::peers::Room* room, const usdcc::peers::PeerId& with);
    void onChat(usdcc::peers::Room* room, const usdcc::peers::Message& message);
    void onTyping(usdcc::peers::Room* room, const usdcc::peers::Message& message);
    void toastChat(usdcc::peers::Room* room, const usdcc::peers::Peer& from, const QString& text, bool privately);
    void expireTypists();
    void askHost(usdcc::peers::JoinRequest* request, usdcc::peers::Room* room);
    void updateStatus();
    QStringList storedList(const QString& key) const;
    void storeList(const QString& key, const QStringList& list) const;

    std::unique_ptr<usdcc::peers::Peers> m_peers;
    QPointer<MainWindow> m_window;
    Hooks m_hooks;
    QHash<usdcc::peers::Room*, QList<ChatLine>> m_chat;
    QHash<usdcc::peers::Room*, QList<Conversation>> m_private;
    QList<Typist> m_typists;
    QHash<QString, QDateTime> m_typingSent;  // room/with -> when this instance last said it was typing
    QTimer* m_typingTimer = nullptr;
    QTimer* m_idleTimer = nullptr;
    QDateTime m_lastInput;
    bool m_away = false;
    QToolButton* m_status = nullptr;
    bool m_panelShown = false;
    bool m_swarmShown = false;
};

// The Profile of this instance: the name and color from Settings > Peers, and
// "usdcc <version>". Its key comes from peersIdentity(), when first needed.
USDCC_UI_API usdcc::peers::Profile peersProfile();
// This profile's key, from the credential store, made on first use; one for this run
// only if it can't be stored.
USDCC_UI_API usdcc::peers::Identity peersIdentity();
// <AppLocalData>/peers/rooms: this machine's rooms (PEERS.md §5.1).
USDCC_UI_API QString peersRoomsFolder();

}  // namespace usdcc::ui
