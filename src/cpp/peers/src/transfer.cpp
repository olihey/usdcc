#include "usdcc/peers/transfer.h"

#include "usdcc/peers/channel.h"
#include "usdcc/peers/room.h"

#include "log.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <vector>

namespace usdcc::peers {

namespace {

const QString kEmptySha256 = QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
constexpr qint64 kMaxJsonInteger = (qint64(1) << 53);

bool isSha256(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(text).hasMatch();
}

bool isTransferId(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return pattern.match(text).hasMatch();
}

bool isLink(const QFileInfo& info) {
    return info.isSymLink() || info.isJunction();
}

// Whether `base`/`relative` can be written without passing through a link: none of
// the parts that are already there is one. `relative` is safe (isSafeTransferPath()).
bool noLinksUnder(const QString& base, const QString& relative, bool checkBase = true) {
    if (checkBase && isLink(QFileInfo(base))) {
        return false;
    }
    QString path = base;
    for (const QString& part : relative.split(QLatin1Char('/'))) {
        path += QLatin1Char('/') + part;
        const QFileInfo info(path);
        if (isLink(info)) {
            return false;
        }
        if (!info.exists()) {
            return true;  // Nothing below it is there either
        }
    }
    return true;
}

std::optional<QString> hashFile(const QString& path, qint64 limit = -1) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 left = limit < 0 ? file.size() : limit;
    QByteArray buffer;
    while (left > 0) {
        buffer = file.read(std::min<qint64>(left, 1024 * 1024));
        if (buffer.isEmpty()) {
            return std::nullopt;
        }
        hash.addData(buffer);
        left -= buffer.size();
    }
    return QString::fromLatin1(hash.result().toHex());
}

// The pieces of a manifest for one piece size: piece i is in the file whose first
// piece is the last one at or before i.
struct Layout {
    qint64 chunk = Transfer::kMaxChunkBytes;
    std::vector<qint64> firstChunk;  // Per file
    std::vector<qint64> firstByte;   // Per file
    qint64 count = 0;
    qint64 total = 0;

    void build(const TransferManifest& manifest, qint64 chunkBytes) {
        chunk = chunkBytes;
        firstChunk.clear();
        firstByte.clear();
        count = 0;
        total = 0;
        for (const TransferFile& file : manifest.files) {
            firstChunk.push_back(count);
            firstByte.push_back(total);
            count += (file.size + chunk - 1) / chunk;
            total += file.size;
        }
    }
    struct Piece {
        int file = -1;
        qint64 offset = 0;
        qint64 length = 0;
    };
    Piece locate(qint64 index, const TransferManifest& manifest) const {
        Piece piece;
        if (index < 0 || index >= count) {
            return piece;
        }
        const auto it = std::upper_bound(firstChunk.begin(), firstChunk.end(), index);
        piece.file = int(it - firstChunk.begin()) - 1;
        piece.offset = (index - firstChunk[piece.file]) * chunk;
        piece.length = std::min(chunk, manifest.files[piece.file].size - piece.offset);
        return piece;
    }
    qint64 bytesAt(qint64 index, const TransferManifest& manifest) const {
        if (index >= count) {
            return total;
        }
        const Piece piece = locate(index, manifest);
        return piece.file < 0 ? 0 : firstByte[piece.file] + piece.offset;
    }
};

// The pieces of every transfer of one room share one budget (Options::messagesPerSecond),
// so that together they stay under the room's rate limit (PEERS.md §7.4).
struct Bucket {
    double tokens = 0;
    QElapsedTimer refill;
};
QMutex bucketsMutex;
QHash<const Room*, Bucket> buckets;

// True if a piece may go now; otherwise how long until one may.
bool takeToken(Room* room, double rate, int* waitMs) {
    rate = std::max(1.0, rate);
    const double burst = std::max(1.0, rate / 10);
    QMutexLocker lock(&bucketsMutex);
    auto it = buckets.find(room);
    if (it == buckets.end()) {
        it = buckets.insert(room, Bucket{burst, {}});
        it->refill.start();
        QObject::connect(room, &QObject::destroyed, [key = static_cast<const Room*>(room)] {
            QMutexLocker inner(&bucketsMutex);
            buckets.remove(key);
        });
    }
    it->tokens = std::min(burst, it->tokens + it->refill.restart() * rate / 1000.0);
    if (it->tokens >= 1) {
        it->tokens -= 1;
        return true;
    }
    *waitMs = std::max(1, int(std::ceil((1 - it->tokens) * 1000.0 / rate)));
    return false;
}

QString displayName(Room* room, const PeerId& id) {
    const Peer peer = room ? room->peer(id) : Peer();
    return peer.name.isEmpty() ? id : peer.name;
}

}  // namespace

// ---------------------------------------------------------------------------------
// Manifests

qint64 TransferManifest::totalBytes() const {
    qint64 total = 0;
    for (const TransferFile& file : files) {
        total += file.size;
    }
    return total;
}

QJsonArray TransferManifest::toJson() const {
    QJsonArray array;
    for (const TransferFile& file : files) {
        array.append(QJsonObject{{QStringLiteral("path"), file.path},
                                 {QStringLiteral("size"), QJsonValue(file.size)},
                                 {QStringLiteral("sha256"), file.sha256}});
    }
    return array;
}

QString TransferManifest::hash() const {
    const QByteArray json = QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex());
}

std::optional<TransferManifest> TransferManifest::fromJson(const QJsonArray& files, QString* error) {
    const auto fail = [error](const QString& why) -> std::optional<TransferManifest> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    TransferManifest manifest;
    QSet<QString> paths;    // Lower case: Windows wouldn't tell them apart
    QSet<QString> folders;
    for (const QJsonValue& value : files) {
        const QJsonObject object = value.toObject();
        TransferFile file;
        file.path = object.value(QLatin1String("path")).toString();
        QString why;
        if (!isSafeTransferPath(file.path, &why)) {
            return fail(QStringLiteral("\"%1\": %2").arg(file.path, why));
        }
        const QJsonValue size = object.value(QLatin1String("size"));
        file.size = size.isDouble() ? size.toInteger(-1) : -1;
        if (file.size < 0 || file.size > kMaxJsonInteger) {
            return fail(QStringLiteral("\"%1\": not a size").arg(file.path));
        }
        file.sha256 = object.value(QLatin1String("sha256")).toString();
        if (!isSha256(file.sha256)) {
            return fail(QStringLiteral("\"%1\": not a sha256").arg(file.path));
        }
        if (file.size == 0 && file.sha256 != kEmptySha256) {
            return fail(QStringLiteral("\"%1\": an empty file with another file's sha256").arg(file.path));
        }
        const QString lower = file.path.toLower();
        if (paths.contains(lower)) {
            return fail(QStringLiteral("\"%1\" is in the manifest twice").arg(file.path));
        }
        paths.insert(lower);
        for (qsizetype slash = lower.indexOf(QLatin1Char('/')); slash > 0;
             slash = lower.indexOf(QLatin1Char('/'), slash + 1)) {
            folders.insert(lower.left(slash));
        }
        manifest.files.append(file);
    }
    for (const QString& path : std::as_const(paths)) {
        if (folders.contains(path)) {
            return fail(QStringLiteral("\"%1\" is both a file and a folder").arg(path));
        }
    }
    return manifest;
}

std::optional<TransferManifest> TransferManifest::fromFiles(const QString& root, const QStringList& files,
                                                            QString* error) {
    const auto fail = [error](const QString& why) -> std::optional<TransferManifest> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    const QString base = QDir(root).absolutePath();
    TransferManifest manifest;
    for (const QString& path : files) {
        QString why;
        if (!isSafeTransferPath(path, &why)) {
            return fail(QStringLiteral("\"%1\": %2").arg(path, why));
        }
        if (!noLinksUnder(base, path, false)) {
            return fail(QStringLiteral("\"%1\" is or passes through a symbolic link").arg(path));
        }
        const QFileInfo info(base + QLatin1Char('/') + path);
        if (!info.isFile()) {
            return fail(QStringLiteral("\"%1\" isn't a file in %2").arg(path, base));
        }
        TransferFile file;
        file.path = path;
        file.source = info.absoluteFilePath();
        file.size = info.size();
        const std::optional<QString> sha = hashFile(file.source);
        if (!sha) {
            return fail(QStringLiteral("Couldn't read %1").arg(file.source));
        }
        file.sha256 = *sha;
        manifest.files.append(file);
    }
    // The same checks as a receiver's (duplicates, a file that is also a folder).
    QString why;
    std::optional<TransferManifest> checked = fromJson(manifest.toJson(), &why);
    if (!checked) {
        return fail(why);
    }
    return manifest;
}

std::optional<TransferManifest> TransferManifest::fromDirectory(const QString& root, QString* error) {
    const QDir base(root);
    if (!base.exists()) {
        if (error) {
            *error = QStringLiteral("%1 isn't a folder").arg(root);
        }
        return std::nullopt;
    }
    QStringList files;
    QDirIterator it(base.absolutePath(), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        const QString relative = base.relativeFilePath(info.absoluteFilePath());
        if (isLink(info)) {
            if (error) {
                *error = QStringLiteral("\"%1\" is a symbolic link").arg(relative);
            }
            return std::nullopt;
        }
        if (info.isFile()) {
            files.append(relative);
        }
    }
    files.sort();
    return fromFiles(root, files, error);
}

bool isSafeTransferPath(const QString& path, QString* why) {
    const auto no = [why](const QString& reason) {
        if (why) {
            *why = reason;
        }
        return false;
    };
    if (path.isEmpty()) {
        return no(QStringLiteral("an empty path"));
    }
    if (path.size() > 1024) {
        return no(QStringLiteral("a path over 1024 characters"));
    }
    if (path.startsWith(QLatin1Char('/')) || QDir::isAbsolutePath(path)) {
        return no(QStringLiteral("an absolute path"));
    }
    if (path.contains(QLatin1Char('\\'))) {
        return no(QStringLiteral("a path with \\ (use /)"));
    }
    if (path.contains(QLatin1Char(':'))) {
        return no(QStringLiteral("a path with a drive letter or a ':'"));
    }
    for (const QChar c : path) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) {
            return no(QStringLiteral("a path with a control character"));
        }
        if (QStringLiteral("<>\"|?*").contains(c)) {
            return no(QStringLiteral("a path with '%1'").arg(c));
        }
    }
    static const QRegularExpression reserved(QStringLiteral("^(con|prn|aux|nul|com[0-9]|lpt[0-9])$"),
                                             QRegularExpression::CaseInsensitiveOption);
    for (const QString& part : path.split(QLatin1Char('/'))) {
        if (part.isEmpty()) {
            return no(QStringLiteral("a path with an empty part"));
        }
        if (part == u"." || part == u"..") {
            return no(QStringLiteral("a path with '%1'").arg(part));
        }
        if (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))) {
            return no(QStringLiteral("a part that ends in a dot or a space"));
        }
        if (reserved.match(part.section(QLatin1Char('.'), 0, 0)).hasMatch()) {
            return no(QStringLiteral("'%1', a name Windows keeps for itself").arg(part));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------
// A transfer

struct Transfer::Impl {
    explicit Impl(Transfer* transfer) : q(transfer) {}

    Transfer* q;
    bool incoming = false;
    QPointer<Room> room;
    QPointer<Channel> channel;
    QString channelName;
    PeerId peer;
    QString id;
    TransferManifest manifest;
    QString hash;
    State state = State::Offered;
    QString error;
    Layout layout;
    QElapsedTimer heard;
    QTimer* watch = nullptr;  // Stalls and giving up
    int giveUpMs = 120000;

    // --- Sending ---
    Options options;
    qint64 next = 0;   // The next piece to send
    qint64 acked = 0;  // Every piece before this one arrived
    qint64 sent = 0;
    QFile in;
    int inFile = -1;
    QTimer* pumpTimer = nullptr;
    QElapsedTimer offeredAt;

    // --- Receiving ---
    QPointer<TransferInbox> inbox;
    QString partDir;
    QString finalDir;
    QString verifiedPath;  // The files of the part already checked, one index a line
    qint64 expected = 0;
    QFile out;
    int outFile = -1;
    QCryptographicHash hasher{QCryptographicHash::Sha256};
    bool resyncAsked = false;
    QElapsedTimer resyncAt;
    QTimer* ackTimer = nullptr;
    qint64 lastAck = -1;
    qint64 lastAccept = -1;  // The last `accept`, and when: one is enough for a break
    QElapsedTimer acceptedAt;

    bool ended() const { return state == State::Done || state == State::Failed || state == State::Cancelled; }
    qint64 bytesDone() const {
        if (state == State::Done) {
            return layout.total;
        }
        return layout.bytesAt(incoming ? expected : acked, manifest);
    }

    void attach();
    void post(const QString& type, QJsonObject body = {}, const QByteArray& bytes = {});
    void setState(State next);
    void end(State how, const QString& reason);
    void fail(const QString& reason, const QString& tellType);
    void onRoomResumed();
    void onTick();

    // Sending
    void offer();
    void onSenderMessage(const Message& message);
    void pump();
    void schedulePump(int ms);
    std::optional<QByteArray> read(const Layout::Piece& piece);

    // Receiving
    bool start();  // false: refused, and said why
    void onReceiverMessage(const Message& message);
    void onReoffer();
    void onChunk(const Message& message);
    bool openOut(int file, qint64 offset);
    void markVerified(int file);
    void sendAck();
    void sendAccept(bool evenIfJustSent);
    void finalize();
    void discardPart();
};

void Transfer::Impl::attach() {
    heard.start();
    watch = new QTimer(q);
    watch->setInterval(std::clamp(incoming ? 1000 : options.stallMs / 4, 25, 1000));
    QObject::connect(watch, &QTimer::timeout, q, [this] { onTick(); });
    watch->start();
    QObject::connect(room, &Room::peerLeft, q, [this](const Peer& left, const QString&) {
        if (left.id == peer && !ended()) {
            fail(QStringLiteral("%1 left the room").arg(left.name.isEmpty() ? left.id : left.name), {});
        }
    });
    QObject::connect(room, &Room::closed, q, [this](const QString& reason) {
        if (!ended()) {
            fail(reason.isEmpty() ? QStringLiteral("The room closed") : reason, {});
        }
    });
    QObject::connect(room, &Room::resumed, q, [this](bool) { onRoomResumed(); });
    // The room going away quietly (deleted): so does this, without a word.
    QObject::connect(channel, &QObject::destroyed, q, [this] {
        if (!ended()) {
            state = State::Failed;
            error = QStringLiteral("The room closed");
        }
        if (watch) {
            watch->stop();
        }
    });
}

void Transfer::Impl::post(const QString& type, QJsonObject body, const QByteArray& bytes) {
    if (!channel || !room || room->state() == Room::State::Closed) {
        return;
    }
    body.insert(QStringLiteral("id"), id);
    channel->send(type, body, bytes, peer);
}

void Transfer::Impl::setState(State next) {
    if (state == next) {
        return;
    }
    state = next;
    Q_EMIT q->stateChanged(next);
}

void Transfer::Impl::end(State how, const QString& reason) {
    if (ended()) {
        return;
    }
    error = reason;
    if (watch) {
        watch->stop();
    }
    if (pumpTimer) {
        pumpTimer->stop();
    }
    if (ackTimer) {
        ackTimer->stop();
    }
    in.close();
    inFile = -1;
    out.close();
    outFile = -1;
    if (how != State::Done) {
        qCInfo(lcPeers).noquote() << (incoming ? "Receiving" : "Sending") << id << "ended:" << reason;
    }
    setState(how);
    if (how == State::Done) {
        Q_EMIT q->finished();
    } else {
        Q_EMIT q->failed(reason);
    }
}

// Ends it as failed, telling the other end with `tellType` (`refuse` or `cancel`) unless empty.
void Transfer::Impl::fail(const QString& reason, const QString& tellType) {
    if (ended()) {
        return;
    }
    if (!tellType.isEmpty()) {
        post(tellType, {{QStringLiteral("reason"), reason}});
    }
    end(State::Failed, reason);
}

void Transfer::Impl::onRoomResumed() {
    // This side lost the host and is back: whatever was on its way may be lost.
    if (ended()) {
        return;
    }
    if (incoming) {
        resyncAsked = false;
        sendAccept(true);
    } else {
        offer();
    }
}

void Transfer::Impl::onTick() {
    if (ended()) {
        watch->stop();
        return;
    }
    if (heard.elapsed() > giveUpMs) {
        fail(incoming ? QStringLiteral("Nothing more came from %1").arg(displayName(room, peer))
                      : QStringLiteral("No answer from %1").arg(displayName(room, peer)),
             QStringLiteral("cancel"));
        return;
    }
    if (!incoming && heard.elapsed() > options.stallMs && offeredAt.elapsed() > options.stallMs) {
        // Lost on the way, or the receiver is back from a break it didn't notice: asking
        // again tells where it is.
        offer();
    }
}

// --- Sending ---

void Transfer::Impl::offer() {
    offeredAt.restart();
    post(QStringLiteral("offer"),
         {{QStringLiteral("files"), manifest.toJson()}, {QStringLiteral("chunk"), layout.chunk}});
}

void Transfer::Impl::onSenderMessage(const Message& message) {
    if (message.from != peer || message.body.value(QLatin1String("id")).toString() != id || ended()) {
        return;
    }
    heard.restart();
    const QString& type = message.type;
    if (type == u"accept") {
        const qint64 at = message.body.value(QLatin1String("next")).toInteger(-1);
        if (at < 0 || at > layout.count) {
            fail(QStringLiteral("%1 asked for a piece there isn't").arg(displayName(room, peer)),
                 QStringLiteral("cancel"));
            return;
        }
        setState(State::Sending);
        next = acked = at;
        in.close();
        inFile = -1;
        if (at > 0) {
            Q_EMIT q->resumed(layout.bytesAt(at, manifest));
        }
        Q_EMIT q->progress(bytesDone(), layout.total);
        pump();
    } else if (type == u"ack") {
        const qint64 at = message.body.value(QLatin1String("next")).toInteger(-1);
        if (at > acked && at <= next) {
            acked = at;
            Q_EMIT q->progress(bytesDone(), layout.total);
            pump();
        }
    } else if (type == u"have" || type == u"done") {
        acked = layout.count;
        end(State::Done, {});
        Q_EMIT q->progress(layout.total, layout.total);
    } else if (type == u"refuse") {
        end(State::Failed, message.body.value(QLatin1String("reason")).toString(QStringLiteral("Refused")));
    } else if (type == u"cancel") {
        end(State::Cancelled, message.body.value(QLatin1String("reason"))
                                  .toString(QStringLiteral("%1 cancelled it").arg(displayName(room, peer))));
    }
}

void Transfer::Impl::schedulePump(int ms) {
    if (!pumpTimer) {
        pumpTimer = new QTimer(q);
        pumpTimer->setSingleShot(true);
        QObject::connect(pumpTimer, &QTimer::timeout, q, [this] { pump(); });
    }
    if (!pumpTimer->isActive()) {
        pumpTimer->start(ms);
    }
}

std::optional<QByteArray> Transfer::Impl::read(const Layout::Piece& piece) {
    const TransferFile& file = manifest.files[piece.file];
    if (inFile != piece.file) {
        in.close();
        in.setFileName(file.source);
        if (!in.open(QIODevice::ReadOnly)) {
            return std::nullopt;
        }
        inFile = piece.file;
        if (in.size() != file.size) {
            return std::nullopt;
        }
    }
    if (in.pos() != piece.offset && !in.seek(piece.offset)) {
        return std::nullopt;
    }
    QByteArray bytes = in.read(piece.length);
    if (bytes.size() != piece.length) {
        return std::nullopt;
    }
    return bytes;
}

void Transfer::Impl::pump() {
    while (state == State::Sending && next < layout.count && room) {
        const Layout::Piece piece = layout.locate(next, manifest);
        const qint64 inFlight = layout.bytesAt(next, manifest) - layout.bytesAt(acked, manifest);
        if (inFlight > 0 && inFlight + piece.length > options.windowBytes) {
            return;  // An ack goes on
        }
        if (room->queuedBytes(peer) > options.queueBytes) {
            schedulePump(20);
            return;
        }
        int waitMs = 0;
        if (!takeToken(room, options.messagesPerSecond, &waitMs)) {
            schedulePump(waitMs);
            return;
        }
        const std::optional<QByteArray> bytes = read(piece);
        if (!bytes) {
            fail(QStringLiteral("Couldn't read %1, or it changed").arg(manifest.files[piece.file].source),
                 QStringLiteral("cancel"));
            return;
        }
        post(QStringLiteral("chunk"),
             {{QStringLiteral("i"), next}, {QStringLiteral("f"), piece.file}, {QStringLiteral("o"), piece.offset}},
             *bytes);
        sent += bytes->size();
        ++next;
    }
}

// --- Receiving ---

bool Transfer::Impl::start() {
    QDir().mkpath(partDir);
    if (isLink(QFileInfo(partDir))) {
        fail(QStringLiteral("The inbox's folder for it is a link"), QStringLiteral("refuse"));
        return false;
    }
    // Resuming: what a part from before already has.
    QSet<int> verified;
    {
        QFile list(verifiedPath);
        if (list.open(QIODevice::ReadOnly)) {
            for (const QByteArray& line : list.readAll().split('\n')) {
                bool ok = false;
                const int index = line.trimmed().toInt(&ok);
                if (ok) {
                    verified.insert(index);
                }
            }
        }
    }
    expected = layout.count;
    for (int f = 0; f < manifest.files.size(); ++f) {
        const TransferFile& file = manifest.files[f];
        if (!noLinksUnder(partDir, file.path)) {
            fail(QStringLiteral("\"%1\" would pass through a link").arg(file.path), QStringLiteral("refuse"));
            return false;
        }
        const QString target = partDir + QLatin1Char('/') + file.path;
        const QFileInfo info(target);
        if (file.size == 0) {
            QDir().mkpath(info.absolutePath());
            QFile empty(target);
            if (!empty.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                fail(QStringLiteral("Couldn't write \"%1\"").arg(file.path), QStringLiteral("refuse"));
                return false;
            }
            continue;
        }
        if (expected < layout.count) {
            continue;  // Past where it resumes: written again
        }
        const qint64 have = info.isFile() ? info.size() : 0;
        if (have == file.size) {
            if (verified.contains(f)) {
                continue;
            }
            if (hashFile(target) == file.sha256) {
                markVerified(f);
                continue;
            }
            QFile::remove(target);
            expected = layout.firstChunk[f];
            continue;
        }
        const qint64 offset = have < file.size ? (have / layout.chunk) * layout.chunk : 0;
        expected = layout.firstChunk[f] + offset / layout.chunk;
    }
    if (expected > 0) {
        qCInfo(lcPeers).noquote() << "Receiving" << id << "resumes at piece" << expected << "of" << layout.count;
    }
    setState(State::Sending);
    if (expected == layout.count) {
        finalize();
        return true;
    }
    sendAccept(true);
    Q_EMIT q->progress(bytesDone(), layout.total);
    return true;
}

void Transfer::Impl::onReceiverMessage(const Message& message) {
    if (message.from != peer) {
        return;
    }
    heard.restart();
    if (message.type == u"chunk") {
        onChunk(message);
    } else if (message.type == u"cancel") {
        if (!ended()) {
            discardPart();
            end(State::Cancelled, message.body.value(QLatin1String("reason"))
                                      .toString(QStringLiteral("%1 cancelled it").arg(displayName(room, peer))));
        }
    }
}

void Transfer::Impl::onReoffer() {
    heard.restart();
    switch (state) {
    case State::Offered:
    case State::Sending:
        // A sender that heard nothing for a while asks again. Answered already (a
        // break this side noticed first), the pieces from there are on their way.
        resyncAsked = false;
        sendAccept(false);
        break;
    case State::Done:
        post(QStringLiteral("done"));
        break;
    case State::Failed:
        post(QStringLiteral("refuse"), {{QStringLiteral("reason"), error}});
        break;
    case State::Cancelled:
        post(QStringLiteral("cancel"), {{QStringLiteral("reason"), error}});
        break;
    }
}

bool Transfer::Impl::openOut(int file, qint64 offset) {
    if (outFile == file && out.isOpen() && out.pos() == offset) {
        return true;
    }
    out.close();
    outFile = -1;
    const TransferFile& entry = manifest.files[file];
    if (!noLinksUnder(partDir, entry.path)) {
        return false;
    }
    const QString target = partDir + QLatin1Char('/') + entry.path;
    QDir().mkpath(QFileInfo(target).absolutePath());
    out.setFileName(target);
    hasher.reset();
    if (offset == 0) {
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
    } else {
        // Resuming inside the file: what is there counts towards its hash.
        if (!out.open(QIODevice::ReadWrite) || out.size() < offset) {
            return false;
        }
        qint64 left = offset;
        while (left > 0) {
            const QByteArray buffer = out.read(std::min<qint64>(left, 1024 * 1024));
            if (buffer.isEmpty()) {
                return false;
            }
            hasher.addData(buffer);
            left -= buffer.size();
        }
        if (!out.resize(offset) || !out.seek(offset)) {
            return false;
        }
    }
    outFile = file;
    return true;
}

void Transfer::Impl::onChunk(const Message& message) {
    if (state != State::Sending) {
        return;
    }
    const qint64 index = message.body.value(QLatin1String("i")).toInteger(-1);
    if (index < expected) {
        return;  // Sent again after a resume: already here
    }
    if (index > expected) {
        // One went missing (a broken connection): ask for it, once a second at most.
        if (!resyncAsked || resyncAt.elapsed() > 1000) {
            resyncAsked = true;
            resyncAt.restart();
            sendAccept(true);
        }
        return;
    }
    resyncAsked = false;
    const Layout::Piece piece = layout.locate(index, manifest);
    if (piece.file < 0 || message.body.value(QLatin1String("f")).toInteger(-1) != piece.file ||
        message.body.value(QLatin1String("o")).toInteger(-1) != piece.offset || message.bytes.size() != piece.length) {
        fail(QStringLiteral("Piece %1 doesn't fit the manifest").arg(index), QStringLiteral("refuse"));
        return;
    }
    const TransferFile& file = manifest.files[piece.file];
    if (!openOut(piece.file, piece.offset) || out.write(message.bytes) != message.bytes.size()) {
        fail(QStringLiteral("Couldn't write \"%1\": %2").arg(file.path, out.errorString()), QStringLiteral("refuse"));
        return;
    }
    hasher.addData(message.bytes);
    ++expected;
    if (piece.offset + piece.length == file.size) {
        out.close();
        outFile = -1;
        if (QString::fromLatin1(hasher.result().toHex()) != file.sha256) {
            QFile::remove(partDir + QLatin1Char('/') + file.path);
            fail(QStringLiteral("\"%1\" doesn't match its sha256").arg(file.path), QStringLiteral("refuse"));
            return;
        }
        markVerified(piece.file);
    }
    Q_EMIT q->progress(bytesDone(), layout.total);
    if (expected == layout.count) {
        finalize();
        return;
    }
    if (!ackTimer->isActive()) {
        ackTimer->start();
    }
}

void Transfer::Impl::markVerified(int file) {
    QFile list(verifiedPath);
    if (list.open(QIODevice::Append)) {
        list.write(QByteArray::number(file) + '\n');
    }
}

void Transfer::Impl::sendAck() {
    if (state == State::Sending && expected != lastAck) {
        lastAck = expected;
        post(QStringLiteral("ack"), {{QStringLiteral("next"), expected}});
    }
}

void Transfer::Impl::sendAccept(bool evenIfJustSent) {
    if (!evenIfJustSent && lastAccept == expected && acceptedAt.isValid() && acceptedAt.elapsed() < 2000) {
        return;
    }
    lastAccept = expected;
    acceptedAt.restart();
    post(QStringLiteral("accept"), {{QStringLiteral("next"), expected}});
}

void Transfer::Impl::finalize() {
    out.close();
    outFile = -1;
    for (const TransferFile& file : std::as_const(manifest.files)) {
        if (QFileInfo(partDir + QLatin1Char('/') + file.path).size() != file.size) {
            fail(QStringLiteral("\"%1\" isn't all there").arg(file.path), QStringLiteral("refuse"));
            return;
        }
    }
    if (QFileInfo::exists(finalDir)) {
        QDir(partDir).removeRecursively();  // Another transfer of it got there first
    } else if (!QDir().rename(partDir, finalDir)) {
        fail(QStringLiteral("Couldn't move the bundle into %1").arg(finalDir), QStringLiteral("refuse"));
        return;
    }
    QFile::remove(verifiedPath);
    post(QStringLiteral("done"));
    end(State::Done, {});
    if (inbox) {
        Q_EMIT inbox->received(q);
    }
}

void Transfer::Impl::discardPart() {
    out.close();
    outFile = -1;
    QDir(partDir).removeRecursively();
    QFile::remove(verifiedPath);
}

// --- The public face ---

Transfer::Transfer(QObject* parent) : QObject(parent), d(std::make_unique<Impl>(this)) {
    static const int registered = qRegisterMetaType<usdcc::peers::Transfer::State>();
    Q_UNUSED(registered);
}

Transfer::~Transfer() {
    if (!d->ended() && d->room && d->room->state() == Room::State::Open) {
        d->post(QStringLiteral("cancel"), {{QStringLiteral("reason"), d->incoming ? QStringLiteral("The receiver stopped")
                                                                                   : QStringLiteral("The sender stopped")}});
    }
}

Transfer* Transfer::send(Room* room, const QString& channelName, const PeerId& to, const TransferManifest& manifest,
                         const Options& options, QObject* parent, QString* error) {
    const auto fail = [error](const QString& why) -> Transfer* {
        if (error) {
            *error = why;
        }
        return nullptr;
    };
    if (!room || room->state() != Room::State::Open) {
        return fail(QStringLiteral("The room isn't open"));
    }
    if (!isValidChannelName(channelName)) {
        return fail(QStringLiteral("\"%1\" isn't a channel name").arg(channelName));
    }
    if (to.isEmpty() || to == room->me().id || !room->peer(to).isValid()) {
        return fail(QStringLiteral("Nobody in the room has the id \"%1\"").arg(to));
    }
    QString why;
    if (!TransferManifest::fromJson(manifest.toJson(), &why)) {
        return fail(why);
    }
    for (const TransferFile& file : manifest.files) {
        const QFileInfo info(file.source);
        if (!info.isFile() || isLink(info) || info.size() != file.size) {
            return fail(QStringLiteral("\"%1\" isn't at %2 as the manifest says").arg(file.path, file.source));
        }
    }
    Channel* channel = room->findChannel(channelName);
    if (!channel) {
        channel = room->channel(channelName, Delivery::Direct);
    }

    auto* transfer = new Transfer(parent);
    Impl& d = *transfer->d;
    d.options = options;
    d.options.chunkBytes = std::clamp(options.chunkBytes, kMinChunkBytes, kMaxChunkBytes);
    d.options.windowBytes = std::max(options.windowBytes, d.options.chunkBytes);
    d.giveUpMs = options.giveUpMs;
    d.room = room;
    d.channel = channel;
    d.channelName = channelName;
    d.peer = to;
    d.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    d.manifest = manifest;
    d.hash = manifest.hash();
    d.layout.build(manifest, d.options.chunkBytes);
    d.attach();
    QObject::connect(channel, &Channel::received, transfer, [&d](const Message& m) { d.onSenderMessage(m); });
    QObject::connect(channel, &Channel::refused, transfer, [&d](const QString& type, const QString& reason) {
        // The host turned the offer or a piece down. The refusal doesn't say which
        // transfer's, so it is this one's only while it waits for its first answer, or
        // when its receiver is gone.
        if (d.ended() || (type != u"offer" && type != u"chunk")) {
            return;
        }
        if ((type == u"offer" && d.state == State::Offered) || !d.room->peer(d.peer).isValid()) {
            d.fail(QStringLiteral("The host refused it: %1").arg(reason), {});
        }
    });
    qCInfo(lcPeers).noquote() << "Offering" << d.id << "to" << to << ":" << manifest.files.size() << "files,"
                              << d.layout.total << "bytes";
    d.offer();
    return transfer;
}

QString Transfer::id() const { return d->id; }
bool Transfer::isIncoming() const { return d->incoming; }
Transfer::State Transfer::state() const { return d->state; }
Room* Transfer::room() const { return d->room; }
QString Transfer::channel() const { return d->channelName; }
PeerId Transfer::peer() const { return d->peer; }
const TransferManifest& Transfer::manifest() const { return d->manifest; }
QString Transfer::bundleHash() const { return d->hash; }
qint64 Transfer::bytesDone() const { return d->bytesDone(); }
qint64 Transfer::bytesTotal() const { return d->layout.total; }
qint64 Transfer::bytesSent() const { return d->sent; }
QString Transfer::error() const { return d->error; }
QString Transfer::directory() const { return d->finalDir; }

void Transfer::cancel(const QString& reason) {
    if (d->ended()) {
        return;
    }
    const QString why = reason.isEmpty() ? QStringLiteral("Cancelled") : reason;
    d->post(QStringLiteral("cancel"), {{QStringLiteral("reason"), why}});
    if (d->incoming) {
        d->discardPart();
    }
    d->end(State::Cancelled, why);
}

// ---------------------------------------------------------------------------------
// The inbox

TransferInbox::TransferInbox(Room* room, const QString& channel, Options options, QObject* parent)
    : QObject(parent), m_room(room), m_options(std::move(options)) {
    m_directory = m_options.directory.isEmpty()
                      ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                            QStringLiteral("/peers/inbox")
                      : m_options.directory;
    m_directory = QDir(m_directory).absolutePath();
    QDir().mkpath(m_directory);
    if (!room) {
        return;
    }
    m_channel = room->findChannel(channel);
    if (!m_channel) {
        m_channel = room->channel(channel, Delivery::Direct);
    }
    if (!m_channel) {
        qCWarning(lcPeers).noquote() << "A transfer inbox on" << channel << ", which isn't a channel name";
        return;
    }
    connect(m_channel, &Channel::received, this, &TransferInbox::onMessage);
}

TransferInbox::~TransferInbox() = default;

QString TransferInbox::directory() const {
    return m_directory;
}

QString TransferInbox::bundleDirectory(const QString& bundleHash) const {
    return m_directory + QLatin1Char('/') + bundleHash;
}

bool TransferInbox::hasBundle(const QString& bundleHash) const {
    if (!isSha256(bundleHash)) {
        return false;
    }
    const QFileInfo info(bundleDirectory(bundleHash));
    return info.isDir() && !isLink(info);
}

QList<Transfer*> TransferInbox::transfers() const {
    QList<Transfer*> result;
    for (const QPointer<Transfer>& transfer : m_transfers) {
        if (transfer) {
            result.append(transfer);
        }
    }
    return result;
}

void TransferInbox::refuse(const PeerId& to, const QString& id, const QString& reason, const Peer& from) {
    qCInfo(lcPeers).noquote() << "Refused transfer" << id << "from" << to << ":" << reason;
    m_channel->send(QStringLiteral("refuse"), {{QStringLiteral("id"), id}, {QStringLiteral("reason"), reason}}, {}, to);
    Q_EMIT refused(from, id, reason);
}

void TransferInbox::onMessage(const Message& message) {
    if (message.type == u"offer") {
        onOffer(message);
        return;
    }
    if (message.type != u"chunk" && message.type != u"cancel") {
        return;  // The answers to this side's own transfers
    }
    const QPointer<Transfer> transfer = m_transfers.value(message.body.value(QLatin1String("id")).toString());
    if (transfer && transfer->d->incoming) {
        transfer->d->onReceiverMessage(message);
    }
}

void TransferInbox::onOffer(const Message& message) {
    if (!m_room) {
        return;
    }
    const Peer from = m_room->peer(message.from);
    const QString id = message.body.value(QLatin1String("id")).toString();
    if (!from.isValid() || !isTransferId(id)) {
        return;
    }
    if (from.kind == Peer::Kind::Guest) {
        refuse(from.id, id, QStringLiteral("Guests can't send files"), from);
        return;
    }
    if (const QPointer<Transfer> existing = m_transfers.value(id)) {
        if (existing->peer() == from.id) {
            existing->d->onReoffer();  // Again: where it is
        } else {
            refuse(from.id, id, QStringLiteral("Another transfer has this id"), from);
        }
        return;
    }
    QString why;
    const QJsonArray files = message.body.value(QLatin1String("files")).toArray();
    if (files.size() > m_options.maxFiles) {
        refuse(from.id, id, QStringLiteral("More than %1 files").arg(m_options.maxFiles), from);
        return;
    }
    std::optional<TransferManifest> manifest = TransferManifest::fromJson(files, &why);
    if (!manifest) {
        refuse(from.id, id, why, from);
        return;
    }
    const qint64 total = manifest->totalBytes();
    if (total > m_options.maxTransferBytes) {
        refuse(from.id, id,
               QStringLiteral("%1 bytes, over this inbox's limit of %2").arg(total).arg(m_options.maxTransferBytes),
               from);
        return;
    }
    const qint64 chunk = message.body.value(QLatin1String("chunk")).toInteger(-1);
    if (chunk < Transfer::kMinChunkBytes || chunk > Transfer::kMaxChunkBytes) {
        refuse(from.id, id, QStringLiteral("Pieces of %1 bytes").arg(chunk), from);
        return;
    }

    auto* transfer = new Transfer(this);
    Transfer::Impl& d = *transfer->d;
    d.incoming = true;
    d.inbox = this;
    d.room = m_room;
    d.channel = m_channel;
    d.channelName = m_channel->name();
    d.peer = from.id;
    d.id = id;
    d.manifest = *manifest;
    d.hash = manifest->hash();
    d.giveUpMs = m_options.giveUpMs;
    d.layout.build(d.manifest, chunk);
    d.finalDir = bundleDirectory(d.hash);
    d.partDir = d.finalDir + QStringLiteral(".part");
    d.verifiedPath = d.finalDir + QStringLiteral(".verified");
    m_transfers.insert(id, transfer);

    if (hasBundle(d.hash)) {
        // Already here: nothing to send.
        qCInfo(lcPeers).noquote() << "Already have bundle" << d.hash << "from" << from.id;
        d.heard.start();
        d.post(QStringLiteral("have"));
        d.end(Transfer::State::Done, {});
        Q_EMIT received(transfer);
        return;
    }
    for (const QPointer<Transfer>& other : std::as_const(m_transfers)) {
        if (other && other != transfer && !other->d->ended() && other->d->hash == d.hash) {
            m_transfers.remove(id);
            delete transfer;
            refuse(from.id, id, QStringLiteral("This bundle is already on its way"), from);
            return;
        }
    }
    const QStorageInfo storage(m_directory);
    if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < total) {
        m_transfers.remove(id);
        delete transfer;
        refuse(from.id, id, QStringLiteral("Not enough disk space"), from);
        return;
    }
    if (m_options.accept && !m_options.accept(from, *manifest)) {
        m_transfers.remove(id);
        delete transfer;
        refuse(from.id, id, QStringLiteral("Declined"), from);
        return;
    }

    d.attach();
    d.ackTimer = new QTimer(transfer);
    d.ackTimer->setSingleShot(true);
    d.ackTimer->setInterval(std::max(0, m_options.ackIntervalMs));
    connect(d.ackTimer, &QTimer::timeout, transfer, [&d] { d.sendAck(); });
    qCInfo(lcPeers).noquote() << "Receiving" << id << "from" << from.id << ":" << manifest->files.size() << "files,"
                              << total << "bytes, bundle" << d.hash;
    if (!d.start()) {
        Q_EMIT refused(from, id, d.error);
        return;
    }
    if (d.state == Transfer::State::Sending) {
        Q_EMIT started(transfer);
    }
}

}  // namespace usdcc::peers
