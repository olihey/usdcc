#pragma once

// Transfers (docs/PEERS.md §7.3): a bundle of files sent to one member in pieces, over
// a channel of the sender's choosing, and the inbox that receives them.
//
//     // The receiver, once per room and channel:
//     TransferInbox* inbox = new TransferInbox(room, "transfer");
//     connect(inbox, &TransferInbox::received, [](Transfer* t) { load(t->directory()); });
//
//     // The sender:
//     QString error;
//     std::optional<TransferManifest> manifest = TransferManifest::fromDirectory(bundle, &error);
//     Transfer* t = Transfer::send(room, "transfer", nodeId, *manifest, {}, this, &error);
//     connect(t, &Transfer::progress, ...);
//     connect(t, &Transfer::finished, ...);
//
// The sender offers the manifest; the receiver answers with the piece to start from (0,
// or further for a bundle it already has part of), or that it has the whole bundle, or
// no. Pieces go to that member alone (`to`), so they never enter the room's history,
// and the sender keeps no more than a window of them unanswered. The wire protocol is
// docs/PEERS_PROTOCOL.md §9.
//
// Everything happens on the room's thread.

#include "usdcc/peers/export.h"
#include "usdcc/peers/types.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace usdcc::peers {

class Channel;
class Room;

// One file of a bundle.
struct USDCC_PEERS_API TransferFile {
    QString path;     // In the bundle: relative, with `/` (isSafeTransferPath())
    qint64 size = 0;
    QString sha256;   // 64 lower-case hex characters
    QString source;   // The sender's file on disk; never sent
};

// What a transfer carries: its files, in order. Two manifests with the same files in
// the same order are one bundle (hash()), which is what the inbox is addressed by.
struct USDCC_PEERS_API TransferManifest {
    QList<TransferFile> files;

    qint64 totalBytes() const;
    // sha256 of the manifest's JSON (toJson(), compact): the bundle's name in the inbox.
    QString hash() const;
    QJsonArray toJson() const;  // [{path, size, sha256}]
    // Checks every path, size and hash; nullopt with why in `error` if one is wrong.
    static std::optional<TransferManifest> fromJson(const QJsonArray& files, QString* error = nullptr);

    // Reads and hashes each file (on this thread: a large bundle takes a while).
    // `files` are relative to `root`, with `/`. Symbolic links are refused.
    static std::optional<TransferManifest> fromFiles(const QString& root, const QStringList& files,
                                                     QString* error = nullptr);
    // Every file under `root`, sorted by path.
    static std::optional<TransferManifest> fromDirectory(const QString& root, QString* error = nullptr);
};

// A path a bundle may hold: relative, with `/` between its parts, none of them empty,
// `.` or `..`, and none with `\`, `:`, a control character, a trailing dot or space, or
// a name Windows keeps for itself (`con`, `nul`, `com1`…). At most 1024 characters.
USDCC_PEERS_API bool isSafeTransferPath(const QString& path, QString* why = nullptr);

class TransferInbox;

// How a transfer is sent (Transfer::Options). Outside the class, as GCC won't take a
// nested struct with member initializers as a default argument of its own class.
struct USDCC_PEERS_API TransferOptions {
    qint64 chunkBytes = 1024 * 1024;       // One piece; at most Transfer::kMaxChunkBytes
    qint64 windowBytes = 4 * 1024 * 1024;  // Sent but not yet acknowledged
    qint64 queueBytes = 2 * 1024 * 1024;   // Waiting in this side's send queue (Room::queuedBytes())
    // Pieces a second, from every transfer of this room together: half the room's
    // usual limit (Limits::messagesPerSecond), so the rest of the room still fits.
    double messagesPerSecond = 100;
    // Nothing heard from the receiver this long: offer again, which also resumes.
    int stallMs = 5000;
    int giveUpMs = 120000;  // Nothing heard this long: failed
};

// What an inbox takes (TransferInbox::Options).
struct USDCC_PEERS_API TransferInboxOptions {
    QString directory;  // Empty: <AppLocalData>/peers/inbox
    qint64 maxTransferBytes = qint64(64) * 1024 * 1024 * 1024;
    int maxFiles = 100000;
    // Asked about each offer that fits the limits; empty: every one is accepted.
    std::function<bool(const Peer& from, const TransferManifest& manifest)> accept;
    int ackIntervalMs = 20;  // Acknowledgements are gathered this long
    // Nothing heard from the sender this long: failed (what arrived is kept).
    int giveUpMs = 120000;
};

// One transfer, either way: Transfer::send() makes an outgoing one, a TransferInbox the
// incoming ones. It ends finished() or failed(), and is deleted with its parent.
class USDCC_PEERS_API Transfer : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 kMaxChunkBytes = 1024 * 1024;
    static constexpr qint64 kMinChunkBytes = 1024;

    enum class State {
        Offered,    // The sender is waiting for the receiver's answer
        Sending,    // Pieces are on their way
        Done,       // Every file arrived and matched its hash (or the receiver had them)
        Failed,     // Refused, broken off, or a file didn't match
        Cancelled,  // Either end cancelled it
    };

    using Options = TransferOptions;

    ~Transfer() override;

    // Offers `manifest` to `to` on `channel` (opened Direct if it isn't open). nullptr,
    // with why in `error`, for a room that isn't open, an invalid channel or member, or
    // a manifest whose paths aren't safe or whose files aren't where it says.
    static Transfer* send(Room* room, const QString& channel, const PeerId& to, const TransferManifest& manifest,
                          const Options& options = {}, QObject* parent = nullptr, QString* error = nullptr);

    QString id() const;
    bool isIncoming() const;
    State state() const;
    Room* room() const;
    QString channel() const;
    PeerId peer() const;  // The other end
    const TransferManifest& manifest() const;
    QString bundleHash() const;  // manifest().hash()
    qint64 bytesDone() const;    // Acknowledged (sending) or written and in order (receiving)
    qint64 bytesTotal() const;
    qint64 bytesSent() const;    // Pieces this side sent, resent ones included; 0 when receiving
    QString error() const;       // Why it failed or was cancelled
    // Receiving: where the bundle is once done (<inbox>/<bundleHash>/).
    QString directory() const;

    // Stops it and tells the other end. Nothing once it has ended.
    void cancel(const QString& reason = {});

Q_SIGNALS:
    void stateChanged(usdcc::peers::Transfer::State state);
    void progress(qint64 done, qint64 total);
    // Sending: the receiver resumed at `done` bytes, after a broken connection or from a
    // part it kept from before.
    void resumed(qint64 done);
    void finished();
    void failed(const QString& reason);  // Failed or cancelled

private:
    friend class TransferInbox;
    struct Impl;
    explicit Transfer(QObject* parent);
    std::unique_ptr<Impl> d;
};

// Receives the transfers offered to this member on one channel, into
// <directory>/<bundle hash>/. A bundle that is already there is acknowledged at once and
// not sent again; one that broke off is resumed from what arrived
// (<directory>/<bundle hash>.part/), also by a later transfer of the same bundle.
class USDCC_PEERS_API TransferInbox : public QObject {
    Q_OBJECT

public:
    using Options = TransferInboxOptions;

    // Opens `channel` (Direct, if it isn't open yet) and listens on it.
    TransferInbox(Room* room, const QString& channel = QStringLiteral("transfer"), Options options = {},
                  QObject* parent = nullptr);
    ~TransferInbox() override;

    QString directory() const;
    // <directory>/<hash>, and whether it is complete there.
    QString bundleDirectory(const QString& bundleHash) const;
    bool hasBundle(const QString& bundleHash) const;
    // The incoming transfers, ended ones included until deleted.
    QList<Transfer*> transfers() const;

Q_SIGNALS:
    // An offer was accepted and pieces are coming.
    void started(usdcc::peers::Transfer* transfer);
    // A bundle is complete in directory(): it arrived, or was already there.
    void received(usdcc::peers::Transfer* transfer);
    // An offer that was turned down, and why: a path that isn't safe, too large, …
    void refused(const usdcc::peers::Peer& from, const QString& transferId, const QString& reason);

private:
    friend class Transfer;
    void onMessage(const Message& message);
    void onOffer(const Message& message);
    void refuse(const PeerId& to, const QString& id, const QString& reason, const Peer& from);

    QPointer<Room> m_room;
    Channel* m_channel = nullptr;
    Options m_options;
    QString m_directory;
    QHash<QString, QPointer<Transfer>> m_transfers;  // By id
};

}  // namespace usdcc::peers

Q_DECLARE_METATYPE(usdcc::peers::Transfer::State)
