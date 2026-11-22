#pragma once

// Peers in the app (docs/PEERS.md, phase 2): the rooms this instance hosts and is in,
// chat in each, who is there and what they have open, the questions to the host as
// toasts, and Settings > Peers. The Peers side panel (PeersSidePanel) shows it.

#include "usdcc/core/export.h"
#include "usdcc/peers/peers.h"

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class QToolButton;

namespace usdcc::peers {
class Transfer;
class TransferInbox;
struct TransferManifest;
}  // namespace usdcc::peers

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
    static constexpr auto acceptImagesKey = "Peers/acceptImages";
    static constexpr auto receivedImagesMaxKey = "Peers/receivedImagesMaxGB";
    static constexpr auto keepReceivedImagesKey = "Peers/keepReceivedImagesDays";
    static void defineSettings();

    struct Hooks {
        // A prim path in a chat message was clicked.
        std::function<void(const QString& path)> selectPrim;
        // What `presence` says this instance has open.
        std::function<QString()> currentStage;
    };

    // An image shared in the chat (PEERS.md §10.1): a preview, and the original file when the
    // sender offers it. Each is a bundle of one file, fetched from the sender.
    struct ChatImage {
        QString preview;         // the preview's bundle hash
        QString previewName;     // its file
        qint64 previewBytes = 0;
        QString original;        // the original's bundle hash; empty: not offered
        QString originalName;
        qint64 originalBytes = 0;
        QString sharedPath;      // where the original is on a shared drive, as the sender sees it
    };

    struct ChatLine {
        usdcc::peers::Peer from;  // Invalid for what the room itself says ("Anna joined")
        QString text;
        QDateTime when;
        std::optional<ChatImage> image;
        usdcc::peers::PeerId to;  // One member's only; empty: the room's
    };

    // A shared image's file here.
    struct ImageFile {
        enum class State { Missing, Fetching, Done, Failed };
        State state = State::Missing;
        QString path;  // once Done
        qint64 done = 0;
        qint64 total = 0;
        QString error;
    };

    // What Share sends (the Render Queue's Share, PEERS.md §10.1).
    struct ShareRequest {
        usdcc::peers::Room* room = nullptr;
        usdcc::peers::PeerId to;  // empty: everyone in the room
        QString caption;
        QImage preview;           // as it should look: through the view transform
        QString previewName;      // "shot010_0012.jpg"
        QString original;         // a file offered with it; empty: the preview only
        QString sharedPath;       // shown as where the original is, for a file on a shared drive
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
    // Shares an image in a chat room, or with one person there: the chat line goes out at
    // once, the files when each member asks for them. False, with `error`, when it can't.
    bool shareImage(const ShareRequest& request, QString* error = nullptr);
    ImageFile imageFile(const QString& hash) const;
    // Asks the sender for a shared image's file. Only on the receiver's word: nothing is
    // fetched by itself (PEERS.md §10.1).
    void fetchImage(usdcc::peers::Room* room, const QString& hash);
    // The receiver's answer to a shared image, by its preview's hash: the preview, and the
    // original too with `original`; or no.
    void acceptImage(usdcc::peers::Room* room, const QString& preview, bool original);
    void declineImage(const QString& preview);
    bool isAccepted(const QString& preview) const { return m_accepted.contains(preview); }
    bool isDeclined(const QString& preview) const { return m_declined.contains(preview); }
    // Whether Accept is asked at all (Settings > Peers > Accept shared images).
    static bool acceptsImages();
    // The sender's: whether an image of this instance's is still offered, and taking it back.
    bool isSharing(const QString& preview) const { return m_offers.contains(preview); }
    void stopSharing(const QString& preview);
    // Where received images are, and emptying it (Settings > Peers > Clear Received Images).
    static QString receivedImagesFolder();
    void clearReceivedImages();
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
    // A shared image's file arrived, is on its way, or failed.
    void imageChanged(const QString& hash);

private:
    void adopt(usdcc::peers::Room* room);
    void addLine(usdcc::peers::Room* room, const ChatLine& line);
    void askHost(usdcc::peers::JoinRequest* request, usdcc::peers::Room* room);
    void updateStatus();
    void openImageInbox(usdcc::peers::Room* room);
    void onImageLine(usdcc::peers::Room* room, const ChatLine& line);
    void onFetch(usdcc::peers::Room* room, const usdcc::peers::Message& message);
    void watchTransfer(usdcc::peers::Transfer* transfer, const QString& hash);
    QStringList storedList(const QString& key) const;
    void storeList(const QString& key, const QStringList& list) const;

    std::unique_ptr<usdcc::peers::Peers> m_peers;
    QPointer<MainWindow> m_window;
    Hooks m_hooks;
    QHash<usdcc::peers::Room*, QList<ChatLine>> m_chat;
    // Shared images: what this instance offers, by bundle hash, and what it expects.
    struct Offer {
        QPointer<usdcc::peers::Room> room;
        usdcc::peers::PeerId to;  // empty: anyone in the room
        std::shared_ptr<usdcc::peers::TransferManifest> manifest;
        QString path;  // the file here
        QDateTime expires;
        QString original;  // a preview's original, taken back with it
    };
    QHash<QString, Offer> m_offers;
    struct Expected {
        QPointer<usdcc::peers::Room> room;
        usdcc::peers::PeerId from;
        QString name;
        qint64 bytes = 0;
    };
    QHash<QString, Expected> m_expected;
    QHash<QString, ImageFile> m_images;
    QHash<QString, QString> m_originals;  // a preview's original
    QSet<QString> m_accepted;  // preview hashes the receiver said yes to
    QSet<QString> m_declined;
    // Image lines a minute from each sender, so one can't flood the chat (by peer id).
    QHash<QString, QList<QDateTime>> m_imageTimes;
    QHash<QString, QDateTime> m_floodNoted;
    QHash<usdcc::peers::Room*, usdcc::peers::TransferInbox*> m_imageInboxes;
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
