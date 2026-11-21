#pragma once

// Peers in the app (docs/PEERS.md, phase 2): the rooms this instance hosts and is in,
// chat in each, who is there and what they have open, the questions to the host as
// toasts, and Settings > Peers. The Peers side panel (PeersSidePanel) shows it.

#include "usdcc/core/export.h"
#include "usdcc/peers/peers.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>
#include <memory>

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

    struct ChatLine {
        usdcc::peers::Peer from;  // Invalid for what the room itself says ("Anna joined")
        QString text;
        QDateTime when;
    };

    // `window` may be null (tests): then no toasts and no status bar.
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

    QList<ChatLine> chat(usdcc::peers::Room* room) const { return m_chat.value(room); }
    void say(usdcc::peers::Room* room, const QString& text);
    // What a peer has open, from `presence`.
    QString stageOf(usdcc::peers::Room* room, const usdcc::peers::Peer& peer) const;

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
    // Tells every room what this instance has open now.
    void updatePresence();
    // Opens the panel (from a toast or the status bar).
    std::function<void()> showPanel;

Q_SIGNALS:
    void roomsChanged();
    void chatAdded(usdcc::peers::Room* room, const usdcc::ui::PeersController::ChatLine& line);
    void chatReset(usdcc::peers::Room* room);
    void peopleChanged(usdcc::peers::Room* room);

private:
    void adopt(usdcc::peers::Room* room);
    void addLine(usdcc::peers::Room* room, const ChatLine& line);
    void askHost(usdcc::peers::JoinRequest* request, usdcc::peers::Room* room);
    void updateStatus();
    QStringList storedList(const QString& key) const;
    void storeList(const QString& key, const QStringList& list) const;

    std::unique_ptr<usdcc::peers::Peers> m_peers;
    QPointer<MainWindow> m_window;
    Hooks m_hooks;
    QHash<usdcc::peers::Room*, QList<ChatLine>> m_chat;
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
