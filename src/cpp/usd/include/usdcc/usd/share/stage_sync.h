#pragma once

// Keeps one live layer equal on every peer of a shared stage (docs/SHARED_STAGE.md
// §4–§7). Knows nothing of rooms: it is given functions that send, and is handed what
// arrives, so a test can join two of them with a pipe. SharedStage connects it to a
// room's channel.
//
// - Every change to the live layer is seen as an op (UndoManager::observeLayer()) and
//   collected. The ops are sent as one batch when the event loop next runs, or, while
//   a local edit session is open (a drag), when it ends; meanwhile they go out a few
//   times a second as a latest-only preview.
// - The host's order is the truth. Each peer keeps a confirmed layer: the live layer
//   as the host has ordered it. A batch that comes back is applied to it; the live
//   layer is the confirmed one plus this peer's own batches not yet back. A peer's
//   batch arriving while this one has its own on the way rebuilds the live layer that
//   way, so for one field the last writer in the host's order wins (§5).
// - A batch is numbered by its sender, and a number seen before is ignored: a peer
//   that resends after reconnecting can't apply anything twice.
// - A joiner gets the host's confirmed layer as a snapshot, with the room's sequence
//   number and every sender's last batch in it, and then the batches after it (§7).
//   A snapshot too large for a message is sent as a file.
// - When the room is quiet the host sends a digest of its confirmed layer. A peer
//   whose own differs asks for a snapshot (§5's check).

#include "usdcc/core/export.h"
#include "usdcc/peers/types.h"
#include "usdcc/usd/share/layer_ops.h"

#include <pxr/usd/sdf/layer.h>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <deque>
#include <functional>
#include <map>
#include <optional>

class QTimer;

namespace usdcc::usd::share {

struct USDCC_USD_UI_API StageSyncOptions {
    // The layer kept equal. Its changes are seen through UndoManager's recorder.
    PXR_NS::SdfLayerHandle live;
    usdcc::peers::PeerId me;
    bool host = false;

    // To everyone, in the host's order, back to the sender too.
    std::function<void(const QString& type, const QJsonObject& body, const QByteArray& bytes)> sendOrdered;
    // To everyone else, latest only: a drag's values.
    std::function<void(const QString& type, const QJsonObject& body, const QByteArray& bytes)> sendLatest;
    // To one member; an empty `to` is the host.
    std::function<void(const usdcc::peers::PeerId& to, const QString& type, const QJsonObject& body,
                       const QByteArray& bytes)>
        sendDirect;
    // A file to one member, named `id`; it is handed to fileArrived() there. Without
    // it, every snapshot goes in a message.
    std::function<void(const usdcc::peers::PeerId& to, const QString& path, const QString& id)> sendFile;
    // Whether a local edit session is open: its batch waits until it ends.
    std::function<bool()> editing;

    qint64 partBytes = 1024 * 1024;            // A batch's bytes per message
    qint64 maxBatchBytes = 12 * 1024 * 1024;   // Larger can't be sent; the change is taken back
    qint64 inlineSnapshotBytes = 8 * 1024 * 1024;  // Larger goes as a file (sendFile)
    int previewIntervalMs = 50;                // 20 a second while dragging
    int checkAfterMs = 3000;                   // Quiet this long, the host sends its digest
    bool timers = true;                        // Off in tests: they call flush() themselves
};

class USDCC_USD_UI_API StageSync : public QObject {
    Q_OBJECT

public:
    explicit StageSync(StageSyncOptions options, QObject* parent = nullptr);
    ~StageSync() override;

    const StageSyncOptions& options() const { return m_options; }
    PXR_NS::SdfLayerHandle live() const { return m_options.live; }
    // The live layer as the host has ordered it.
    PXR_NS::SdfLayerHandle confirmed() const { return m_confirmed; }
    // The host's last number applied here.
    qint64 lastSeq() const { return m_seq; }
    // The host is from the start; a joiner once its snapshot is in.
    bool isSynced() const { return m_synced; }
    // This peer's changes not yet confirmed by the host.
    int pendingBatches() const { return int(m_pending.size()) + (m_unsent.empty() ? 0 : 1); }

    // The host's: what a joiner gets first (the channel's snapshot provider).
    QJsonObject snapshotBody(QByteArray* bytes);
    // Everything that arrives on the stage's channels for this peer.
    void receive(const usdcc::peers::Message& message);
    // A file sendFile() sent, now here.
    void fileArrived(const QString& id, const QString& path);
    // The host turned down something this peer sent (it may only watch).
    void refused(const QString& type, const QString& reason);
    void peerLeft(const usdcc::peers::PeerId& peer);

    // A member's: asks the host for its confirmed layer. A room sends a channel's
    // snapshot only to members who join after the channel opened, so a member already
    // in the room asks when it opens the stage.
    void requestSnapshot();
    // Sends what changed here now, unless an edit session is open.
    void flush();
    // Host: sends the digest of the confirmed layer now (the check, §5).
    void sendCheck();
    // While an edit session is open: the changes so far, latest only, so the others
    // see a drag move. A timer calls it 20 times a second.
    void sendPreview();

Q_SIGNALS:
    // The live layer changed because of a peer.
    void remoteChanged();
    // A joiner's snapshot is in: the live layer is the room's.
    void synced();
    // The host refused this peer's changes, and they were taken back.
    void editsRefused(const QString& reason);
    // This peer's live layer differed from the host's; a snapshot was asked for.
    void mismatched();
    // Something couldn't be sent or read.
    void failed(const QString& error);

private:
    struct Pending {
        qint64 n = 0;
        LayerOps ops;
    };
    struct Incoming {
        qint64 n = 0;
        int parts = 0;
        std::vector<QByteArray> chunks;
    };

    void onLocalOp(const LayerOp& op);
    void scheduleFlush();
    void sendBatch(qint64 n, const LayerOps& ops);
    void onBatchPart(const usdcc::peers::Message& message);
    void applyBatch(const usdcc::peers::PeerId& from, qint64 n, const LayerOps& ops);
    void onPreview(const usdcc::peers::Message& message);
    void onSnapshot(const QJsonObject& body, const QByteArray& bytes);
    void applySnapshot(const QJsonObject& body, const QByteArray& bytes);
    void onCheck(const QJsonObject& body);
    void onFetch(const usdcc::peers::PeerId& from, const QJsonObject& body);
    void askForSnapshot();
    void sendSnapshotTo(const usdcc::peers::PeerId& to);
    // live := confirmed + pending + unsent.
    void rebuild();
    void applyToLive(const LayerOps& ops);
    void takeBackUnsent(const QString& why);
    void noteActivity();

    StageSyncOptions m_options;
    PXR_NS::SdfLayerRefPtr m_confirmed;
    qint64 m_seq = 0;
    bool m_synced = false;
    qint64 m_nextN = 0;
    std::deque<Pending> m_pending;
    LayerOps m_unsent;
    QHash<usdcc::peers::PeerId, qint64> m_last;          // each sender's last batch applied
    QHash<usdcc::peers::PeerId, Incoming> m_incoming;     // batches arriving in parts
    QHash<usdcc::peers::PeerId, bool> m_previewed;        // previews applied to the live layer
    std::deque<usdcc::peers::Message> m_waiting;          // ordered, before the snapshot is in
    std::optional<QJsonObject> m_fetching;                // a snapshot coming as a file
    std::map<QString, QString> m_snapshotFiles;           // host: id → file it was written to
    int m_snapshotSerial = 0;
    QTimer* m_flushTimer = nullptr;
    QTimer* m_previewTimer = nullptr;
    QTimer* m_checkTimer = nullptr;
    bool m_changedSinceCheck = false;
};

}  // namespace usdcc::usd::share
