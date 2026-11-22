#include "usdcc/usd/share/stage_sync.h"

#include "usdcc/core/log.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/layerStateDelegate.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

#include <algorithm>

using namespace PXR_NS;
using usdcc::peers::Message;
using usdcc::peers::PeerId;

namespace usdcc::usd::share {

namespace {

constexpr auto kLog = "usdcc.share";

// A batch's ops and values as one payload: the header's JSON, then the scratch layers.
QByteArray packBatch(const EncodedOps& encoded) {
    std::vector<QByteArray> chunks;
    chunks.push_back(QJsonDocument(encoded.header).toJson(QJsonDocument::Compact));
    chunks.insert(chunks.end(), encoded.layers.begin(), encoded.layers.end());
    return packChunks(chunks);
}

std::optional<LayerOps> unpackBatch(const QByteArray& payload, QString* error) {
    std::optional<std::vector<QByteArray>> chunks = unpackChunks(payload);
    if (!chunks || chunks->empty()) {
        if (error) *error = QStringLiteral("A batch that can't be read");
        return std::nullopt;
    }
    EncodedOps encoded;
    encoded.header = QJsonDocument::fromJson(chunks->front()).array();
    encoded.layers.assign(chunks->begin() + 1, chunks->end());
    return decodeOps(encoded, error);
}

QJsonObject lastToJson(const QHash<PeerId, qint64>& last) {
    QJsonObject json;
    for (auto it = last.cbegin(); it != last.cend(); ++it) json.insert(it.key(), it.value());
    return json;
}

}  // namespace

StageSync::StageSync(StageSyncOptions options, QObject* parent)
    : QObject(parent), m_options(std::move(options)), m_confirmed(SdfLayer::CreateAnonymous("confirmed.usda")) {
    m_synced = m_options.host;
    if (m_options.host && m_options.live) {
        // What is in the live layer already (kept from an earlier share) is the start.
        m_confirmed->TransferContent(m_options.live);
    }
    if (m_options.timers) {
        m_flushTimer = new QTimer(this);
        m_flushTimer->setSingleShot(true);
        m_flushTimer->setInterval(0);
        connect(m_flushTimer, &QTimer::timeout, this, &StageSync::flush);
        m_previewTimer = new QTimer(this);
        m_previewTimer->setInterval(m_options.previewIntervalMs);
        connect(m_previewTimer, &QTimer::timeout, this, [this] {
            if (m_options.editing && m_options.editing()) {
                sendPreview();
            } else {
                m_previewTimer->stop();
                flush();
            }
        });
        m_checkTimer = new QTimer(this);
        m_checkTimer->setSingleShot(true);
        m_checkTimer->setInterval(m_options.checkAfterMs);
        connect(m_checkTimer, &QTimer::timeout, this, &StageSync::sendCheck);
    }
    UndoManager::observeLayer(m_options.live, [this](const LayerOp& op) { onLocalOp(op); });
}

StageSync::~StageSync() {
    if (m_options.live) UndoManager::observeLayer(m_options.live, {});
    for (const auto& [id, file] : m_snapshotFiles) QFile::remove(file);
}

// ---------------------------------------------------------------------------------
// This peer's changes

void StageSync::onLocalOp(const LayerOp& op) {
    appendCoalesced(m_unsent, op);
    scheduleFlush();
}

void StageSync::scheduleFlush() {
    if (!m_options.timers) return;
    if (m_options.editing && m_options.editing()) {
        if (!m_previewTimer->isActive()) m_previewTimer->start();
        return;
    }
    if (!m_flushTimer->isActive()) m_flushTimer->start();
}

void StageSync::flush() {
    if (m_unsent.empty()) return;
    if (m_options.editing && m_options.editing()) {
        scheduleFlush();
        return;
    }
    if (!m_synced) return;  // sent once the snapshot is in, on top of it
    LayerOps ops = std::move(m_unsent);
    m_unsent.clear();
    const qint64 n = ++m_nextN;
    m_pending.push_back({n, ops});  // before sending: on the host it comes back at once
    sendBatch(n, m_pending.back().ops);
}

void StageSync::sendBatch(qint64 n, const LayerOps& ops) {
    QString error;
    std::optional<EncodedOps> encoded = encodeOps(ops, &error);
    QByteArray payload;
    if (encoded) payload = packBatch(*encoded);
    if (!encoded || payload.size() > m_options.maxBatchBytes) {
        if (encoded) {
            error = tr("The change is too large to share (%1 MB)").arg(double(payload.size()) / (1024.0 * 1024.0), 0, 'f', 1);
        }
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), [n](const Pending& p) { return p.n == n; }),
                        m_pending.end());
        takeBackUnsent(error);
        return;
    }
    const qint64 partBytes = std::max<qint64>(1024, m_options.partBytes);
    const int parts = std::max(1, int((payload.size() + partBytes - 1) / partBytes));
    for (int part = 0; part < parts; ++part) {
        const QByteArray chunk = payload.mid(qsizetype(part * partBytes), qsizetype(partBytes));
        m_options.sendOrdered(QStringLiteral("batch"),
                              {{QStringLiteral("n"), n}, {QStringLiteral("part"), part}, {QStringLiteral("parts"), parts}},
                              chunk);
    }
}

void StageSync::sendPreview() {
    if (m_unsent.empty() || !m_options.sendLatest) return;
    std::optional<EncodedOps> encoded = encodeOps(m_unsent);
    if (!encoded) return;
    const QByteArray payload = packBatch(*encoded);
    if (payload.size() > m_options.partBytes) return;  // a preview is only for small changes
    m_options.sendLatest(QStringLiteral("preview"), {}, payload);
}

void StageSync::takeBackUnsent(const QString& why) {
    USDCC_LOG_WARNING(kLog, "Took back a change that couldn't be shared: {}", why.toStdString());
    m_unsent.clear();
    rebuild();
    Q_EMIT failed(why);
}

// ---------------------------------------------------------------------------------
// What arrives

void StageSync::receive(const Message& message) {
    if (message.type == QLatin1String("snapshot")) {
        onSnapshot(message.body, message.bytes);
        return;
    }
    if (message.type == QLatin1String("preview")) {
        if (message.from != m_options.me) onPreview(message);
        return;
    }
    if (message.type == QLatin1String("fetch")) {
        if (m_options.host) onFetch(message.from, message.body);
        return;
    }
    if (message.type == QLatin1String("resync")) {
        if (m_options.host) sendSnapshotTo(message.from);
        return;
    }
    if (message.seq <= 0) return;  // the rest are ordered
    if (!m_synced || m_fetching) {
        m_waiting.push_back(message);
        return;
    }
    if (message.seq <= m_seq) return;  // in the snapshot already
    m_seq = message.seq;
    if (message.type == QLatin1String("batch")) {
        onBatchPart(message);
    } else if (message.type == QLatin1String("check")) {
        onCheck(message.body);
    }
}

void StageSync::onBatchPart(const Message& message) {
    const qint64 n = message.body.value(QLatin1String("n")).toInteger();
    const int part = message.body.value(QLatin1String("part")).toInt();
    const int parts = message.body.value(QLatin1String("parts")).toInt(1);
    Incoming& incoming = m_incoming[message.from];
    if (part == 0 || incoming.n != n) incoming = Incoming{n, parts, {}};
    if (int(incoming.chunks.size()) != part) {
        USDCC_LOG_WARNING(kLog, "A batch from {} arrived out of order", message.from.toStdString());
        m_incoming.remove(message.from);
        askForSnapshot();
        return;
    }
    incoming.chunks.push_back(message.bytes);
    if (int(incoming.chunks.size()) < parts) return;
    QByteArray payload;
    for (const QByteArray& chunk : incoming.chunks) payload += chunk;
    m_incoming.remove(message.from);

    if (n <= m_last.value(message.from, 0)) return;  // sent again after a reconnect
    m_last[message.from] = n;
    QString error;
    std::optional<LayerOps> ops = unpackBatch(payload, &error);
    if (!ops) {
        USDCC_LOG_WARNING(kLog, "Couldn't read a batch from {}: {}", message.from.toStdString(), error.toStdString());
        askForSnapshot();
        return;
    }
    applyBatch(message.from, n, *ops);
}

void StageSync::applyBatch(const PeerId& from, qint64 n, const LayerOps& ops) {
    applyOps(m_confirmed, ops);
    noteActivity();
    if (from == m_options.me) {
        while (!m_pending.empty() && m_pending.front().n <= n) m_pending.pop_front();
        // The live layer has it already, unless a peer's batch was put under it since,
        // which rebuilt it with this one on top: the same either way.
        return;
    }
    if (m_pending.empty() && m_unsent.empty() && !m_previewed.value(from)) {
        applyToLive(ops);
    } else {
        m_previewed.remove(from);
        rebuild();
    }
    Q_EMIT remoteChanged();
}

void StageSync::onPreview(const Message& message) {
    if (!m_synced || m_fetching) return;
    std::optional<LayerOps> ops = unpackBatch(message.bytes, nullptr);
    if (!ops) return;
    m_previewed[message.from] = true;
    applyToLive(*ops);
    Q_EMIT remoteChanged();
}

void StageSync::peerLeft(const PeerId& peer) {
    m_incoming.remove(peer);
    if (m_previewed.take(peer)) {
        rebuild();  // its drag never ended
        Q_EMIT remoteChanged();
    }
}

void StageSync::refused(const QString& type, const QString& reason) {
    if (type != QLatin1String("batch") && type != QLatin1String("preview")) return;
    if (m_pending.empty() && m_unsent.empty()) return;
    m_pending.clear();
    m_unsent.clear();
    rebuild();
    Q_EMIT editsRefused(reason);
}

// ---------------------------------------------------------------------------------
// Joining, and the check

QJsonObject StageSync::snapshotBody(QByteArray* bytes) {
    QJsonObject body{{QStringLiteral("seq"), m_seq}, {QStringLiteral("last"), lastToJson(m_last)}};
    QString error;
    std::optional<QByteArray> usdc = layerToUsdc(m_confirmed, &error);
    if (!usdc) {
        USDCC_LOG_ERROR(kLog, "Couldn't write the snapshot: {}", error.toStdString());
        body.insert(QStringLiteral("error"), error);
        return body;
    }
    if (usdc->size() <= m_options.inlineSnapshotBytes || !m_options.sendFile) {
        if (bytes) *bytes = std::move(*usdc);
        return body;
    }
    // Too large for a message: written to a file the joiner asks for.
    const QString id = QStringLiteral("snapshot-%1-%2").arg(m_seq).arg(++m_snapshotSerial);
    const QString path = QDir::temp().filePath(QStringLiteral("usdcc_share_%1.usdc").arg(id));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(*usdc) != usdc->size()) {
        body.insert(QStringLiteral("error"), QStringLiteral("Couldn't write %1").arg(path));
        return body;
    }
    file.close();
    m_snapshotFiles[id] = path;
    while (m_snapshotFiles.size() > 4) {
        QFile::remove(m_snapshotFiles.begin()->second);
        m_snapshotFiles.erase(m_snapshotFiles.begin());
    }
    body.insert(QStringLiteral("file"), id);
    body.insert(QStringLiteral("size"), qint64(usdc->size()));
    return body;
}

void StageSync::onSnapshot(const QJsonObject& body, const QByteArray& bytes) {
    if (m_options.host) return;
    if (body.contains(QLatin1String("error"))) {
        Q_EMIT failed(body.value(QLatin1String("error")).toString());
        return;
    }
    if (body.contains(QLatin1String("file"))) {
        m_fetching = body;
        m_options.sendDirect({}, QStringLiteral("fetch"), {{QStringLiteral("file"), body.value(QLatin1String("file"))}}, {});
        return;
    }
    applySnapshot(body, bytes);
}

void StageSync::onFetch(const PeerId& from, const QJsonObject& body) {
    const auto it = m_snapshotFiles.find(body.value(QLatin1String("file")).toString());
    if (it == m_snapshotFiles.end()) {
        sendSnapshotTo(from);  // gone: a new one
        return;
    }
    m_options.sendFile(from, it->second, it->first);
}

void StageSync::fileArrived(const QString& id, const QString& path) {
    if (!m_fetching || m_fetching->value(QLatin1String("file")).toString() != id) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        Q_EMIT failed(tr("Couldn't read the stage's snapshot at %1").arg(path));
        return;
    }
    const QJsonObject body = *m_fetching;
    m_fetching.reset();
    applySnapshot(body, file.readAll());
}

void StageSync::applySnapshot(const QJsonObject& body, const QByteArray& bytes) {
    QString error;
    SdfLayerRefPtr fresh = SdfLayer::CreateAnonymous("confirmed.usda");
    if (!bytes.isEmpty() && !usdcToLayer(bytes, fresh, &error)) {
        Q_EMIT failed(tr("Couldn't read the stage's snapshot: %1").arg(error));
        return;
    }
    m_confirmed->TransferContent(fresh);
    m_seq = body.value(QLatin1String("seq")).toInteger();
    m_last.clear();
    const QJsonObject last = body.value(QLatin1String("last")).toObject();
    for (auto it = last.begin(); it != last.end(); ++it) m_last.insert(it.key(), it.value().toInteger());
    m_incoming.clear();
    m_previewed.clear();
    // This peer's batches the snapshot has are done; the others are sent again (a
    // number already seen is ignored, so one that was only late does no harm).
    const qint64 mine = m_last.value(m_options.me, 0);
    while (!m_pending.empty() && m_pending.front().n <= mine) m_pending.pop_front();
    m_nextN = std::max(m_nextN, mine);
    rebuild();
    const bool first = !m_synced;
    m_synced = true;
    for (const Pending& pending : m_pending) sendBatch(pending.n, pending.ops);
    std::deque<Message> waiting = std::move(m_waiting);
    m_waiting.clear();
    for (const Message& message : waiting) receive(message);
    if (!m_unsent.empty()) scheduleFlush();
    if (first) Q_EMIT synced();
    Q_EMIT remoteChanged();
}

void StageSync::requestSnapshot() { askForSnapshot(); }

void StageSync::askForSnapshot() {
    if (m_options.host) return;
    m_options.sendDirect({}, QStringLiteral("resync"), {}, {});
}

void StageSync::sendSnapshotTo(const PeerId& to) {
    QByteArray bytes;
    const QJsonObject body = snapshotBody(&bytes);
    m_options.sendDirect(to, QStringLiteral("snapshot"), body, bytes);
}

void StageSync::noteActivity() {
    m_changedSinceCheck = true;
    if (m_options.host && m_checkTimer) m_checkTimer->start();
}

void StageSync::sendCheck() {
    if (!m_options.host || !m_changedSinceCheck) return;
    m_changedSinceCheck = false;
    m_options.sendOrdered(QStringLiteral("check"), {{QStringLiteral("digest"), layerDigest(m_confirmed)}}, {});
}

void StageSync::onCheck(const QJsonObject& body) {
    if (m_options.host) return;
    m_changedSinceCheck = false;
    if (layerDigest(m_confirmed) == body.value(QLatin1String("digest")).toString()) return;
    USDCC_LOG_WARNING(kLog, "This stage's live layer differs from the host's; asking for it again");
    askForSnapshot();
    Q_EMIT mismatched();
}

// ---------------------------------------------------------------------------------
// The live layer

void StageSync::applyToLive(const LayerOps& ops) {
    UndoManager::changeUnrecorded(m_options.live,
                                  [&](SdfLayerStateDelegateBase& delegate) { applyOps(delegate, m_options.live, ops); });
}

void StageSync::rebuild() {
    UndoManager::changeUnrecorded(m_options.live, [&](SdfLayerStateDelegateBase& delegate) {
        SdfChangeBlock block;
        m_options.live->TransferContent(m_confirmed);
        for (const Pending& pending : m_pending) applyOps(delegate, m_options.live, pending.ops);
        applyOps(delegate, m_options.live, m_unsent);
    });
}

}  // namespace usdcc::usd::share
