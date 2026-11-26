// usdcc_peers transfers (docs/PEERS.md §7.3, PEERS_PROTOCOL.md §9): a bundle of files
// sent in pieces to one member and checked against its manifest, resuming after the
// connection is cut, the inbox's cache, and what the inbox refuses. Some tests speak
// the protocol by hand from the host, to send what a real sender wouldn't. Runs in
// usdcc_peers_tests.
#include "peers_test_support.h"

#include "usdcc/peers/transfer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QTemporaryDir>

using namespace usdcc::peers;
using namespace usdcc::peers::test;

namespace {

QByteArray randomBytes(qint64 size, quint32 seed) {
    QRandomGenerator random(seed);
    QByteArray bytes(size, Qt::Uninitialized);
    for (qint64 i = 0; i < size; ++i) {
        bytes[i] = char(random.generate() & 0xff);
    }
    return bytes;
}

QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void writeFile(const QString& root, const QString& path, const QByteArray& bytes) {
    const QString full = root + "/" + path;
    QDir().mkpath(QFileInfo(full).absolutePath());
    QFile file(full);
    CHECK(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "couldn't write %s", qPrintable(full));
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

TransferManifest manifestOf(const QString& root) {
    QString error;
    std::optional<TransferManifest> manifest = TransferManifest::fromDirectory(root, &error);
    CHECK(manifest, "no manifest for %s: %s", qPrintable(root), qPrintable(error));
    return manifest.value_or(TransferManifest());
}

TransferInbox::Options inboxIn(const QTemporaryDir& dir) {
    TransferInbox::Options options;
    options.directory = dir.path() + "/inbox";
    return options;
}

// What a transfer said.
struct Watch {
    QObject context;
    bool finished = false;
    bool failed = false;
    bool resumed = false;
    QString reason;
    int progressCount = 0;
    qint64 lastDone = -1;

    explicit Watch(Transfer* transfer) {
        if (!transfer) {
            return;
        }
        QObject::connect(transfer, &Transfer::finished, &context, [this] { finished = true; });
        QObject::connect(transfer, &Transfer::failed, &context, [this](const QString& r) {
            failed = true;
            reason = r;
        });
        QObject::connect(transfer, &Transfer::resumed, &context, [this](qint64) { resumed = true; });
        QObject::connect(transfer, &Transfer::progress, &context, [this](qint64 done, qint64) {
            ++progressCount;
            lastDone = done;
        });
    }
    bool ended() const { return finished || failed; }
};

// The receiving side's signals.
struct InboxWatch {
    QObject context;
    QList<Transfer*> started;
    QList<Transfer*> received;
    QStringList refused;

    explicit InboxWatch(TransferInbox* inbox) {
        QObject::connect(inbox, &TransferInbox::started, &context, [this](Transfer* t) { started.append(t); });
        QObject::connect(inbox, &TransferInbox::received, &context, [this](Transfer* t) { received.append(t); });
        QObject::connect(inbox, &TransferInbox::refused, &context,
                         [this](const Peer&, const QString&, const QString& reason) { refused.append(reason); });
    }
};

Transfer::Options quick() {
    Transfer::Options options;
    options.chunkBytes = 64 * 1024;
    options.stallMs = 500;
    return options;
}

}  // namespace

USDCC_TEST(peers_transfer_paths) {
    for (const char* safe : {"a.txt", "shot/010/render.exr", "Ünïcode/ñ.usd", ".hidden", "a..b"}) {
        CHECK(isSafeTransferPath(safe), "'%s' is refused", safe);
    }
    for (const char* unsafe : {"", "../evil", "a/../../evil", "./a", "/etc/passwd", "C:/Windows/x", "c:x", "a\\b",
                               "\\\\server\\share", "a//b", "dir/", "con", "aux.txt", "dir/nul", "a.", "a ", "a\nb",
                               "what?", "file.txt:stream"}) {
        CHECK(!isSafeTransferPath(unsafe), "'%s' is taken", unsafe);
    }
    // A manifest checks its paths, and what Windows wouldn't tell apart.
    const QString sha = sha256("x");
    const auto entry = [&](const QString& path) {
        return QJsonObject{{"path", path}, {"size", 1}, {"sha256", sha}};
    };
    CHECK(TransferManifest::fromJson(QJsonArray{entry("a/b.txt"), entry("c.txt")}), "a good manifest is refused");
    CHECK(!TransferManifest::fromJson(QJsonArray{entry("A.txt"), entry("a.txt")}), "a path twice is taken");
    CHECK(!TransferManifest::fromJson(QJsonArray{entry("a"), entry("a/b")}), "a file that is a folder is taken");
    CHECK(!TransferManifest::fromJson(QJsonArray{QJsonObject{{"path", "a"}, {"size", 1}, {"sha256", "nope"}}}),
          "a bad sha256 is taken");
    CHECK(!TransferManifest::fromJson(QJsonArray{QJsonObject{{"path", "a"}, {"size", -1}, {"sha256", sha}}}),
          "a negative size is taken");
    // The bundle's name is its manifest's hash, so the same files are the same bundle.
    TransferManifest one = *TransferManifest::fromJson(QJsonArray{entry("a"), entry("b")});
    TransferManifest two = *TransferManifest::fromJson(QJsonArray{entry("a"), entry("b")});
    CHECK(one.hash() == two.hash() && one.hash().size() == 64, "one bundle has two names");
    two.files[1].path = "c";
    CHECK(one.hash() != two.hash(), "two bundles have one name");
}

USDCC_TEST(peers_transfer_files) {
    QTemporaryDir dir;
    const QString source = dir.path() + "/source";
    const QByteArray layer = "#usda 1.0\n(\n    defaultPrim = \"shot\"\n)\n";
    const QByteArray texture = randomBytes(200 * 1024 + 17, 1);
    writeFile(source, "shot.usda", layer);
    writeFile(source, "textures/wood/albedo.png", texture);
    writeFile(source, "empty.txt", {});

    auto host = hostRoom();
    auto sender = joinRoom(*host, "Sender");
    auto node = joinRoom(*host, "Render Node");
    auto* inbox = new TransferInbox(node.get(), "transfer", inboxIn(dir), node.get());
    InboxWatch inboxWatch(inbox);

    const TransferManifest manifest = manifestOf(source);
    CHECK(manifest.files.size() == 3 && manifest.totalBytes() == layer.size() + texture.size(),
          "the manifest has %lld files", qlonglong(manifest.files.size()));
    QString error;
    // Member to member, through the host.
    Transfer* transfer = Transfer::send(sender.get(), "transfer", node->me().id, manifest, quick(), sender.get(), &error);
    CHECK(transfer, "couldn't send: %s", qPrintable(error));
    Watch watch(transfer);
    CHECK(waitUntil([&] { return watch.ended() && inboxWatch.received.size() == 1; }), "the bundle didn't arrive");
    CHECK(watch.finished && transfer->state() == Transfer::State::Done, "the sender says: %s", qPrintable(watch.reason));
    CHECK(watch.lastDone == manifest.totalBytes() && watch.progressCount > 1, "progress ended at %lld",
          qlonglong(watch.lastDone));

    Transfer* incoming = inboxWatch.received.first();
    CHECK(incoming->isIncoming() && incoming->peer() == sender->me().id && incoming->state() == Transfer::State::Done,
          "the inbox's transfer is wrong");
    const QString bundle = incoming->directory();
    CHECK(bundle == inbox->bundleDirectory(manifest.hash()) && inbox->hasBundle(manifest.hash()),
          "the bundle isn't at its hash: %s", qPrintable(bundle));
    CHECK(readFile(bundle + "/shot.usda") == layer && readFile(bundle + "/textures/wood/albedo.png") == texture &&
              QFileInfo(bundle + "/empty.txt").isFile() && QFileInfo(bundle + "/empty.txt").size() == 0,
          "the files arrived different");
    CHECK(!QFileInfo::exists(bundle + ".part") && !QFileInfo::exists(bundle + ".verified"),
          "the part was left behind");

    // A sender that doesn't say what it sends right is stopped before it starts.
    TransferManifest bad = manifest;
    bad.files[0].path = "../escape.usda";
    CHECK(!Transfer::send(sender.get(), "transfer", node->me().id, bad, {}, nullptr, &error) &&
              error.contains("escape"),
          "an unsafe path was sent: %s", qPrintable(error));
    CHECK(!Transfer::send(sender.get(), "transfer", "nobody", manifest, {}, nullptr, &error),
          "a transfer to nobody started");
}

USDCC_TEST(peers_transfer_large_file) {
    QTemporaryDir dir;
    const QString source = dir.path() + "/source";
    const QByteArray big = randomBytes(5 * 1024 * 1024 + 4321, 2);
    writeFile(source, "frame.exr", big);

    auto host = hostRoom();
    auto node = joinRoom(*host, "Render Node");
    auto* inbox = new TransferInbox(node.get(), "transfer", inboxIn(dir), node.get());
    InboxWatch inboxWatch(inbox);

    // The host to a member, in pieces of 1 MB: more than one frame's worth and under the
    // room's limit on one.
    QString error;
    Transfer* transfer = Transfer::send(host.get(), "transfer", node->me().id, manifestOf(source), {}, host.get(), &error);
    CHECK(transfer, "couldn't send: %s", qPrintable(error));
    Watch watch(transfer);
    CHECK(waitUntil([&] { return watch.ended() && inboxWatch.received.size() == 1; }, 20000), "the file didn't arrive");
    CHECK(watch.finished, "the sender says: %s", qPrintable(watch.reason));
    CHECK(transfer->bytesSent() == big.size(), "%lld bytes went for %lld", qlonglong(transfer->bytesSent()),
          qlonglong(big.size()));
    CHECK(watch.progressCount >= 2, "only %d progress signals", watch.progressCount);
    CHECK(readFile(inboxWatch.received.first()->directory() + "/frame.exr") == big, "the file arrived different");
    CHECK(node->state() == Room::State::Open && host->peers().size() == 2, "someone was dropped on the way");
}

USDCC_TEST(peers_transfer_resumes_after_cut) {
    QTemporaryDir dir;
    const QString source = dir.path() + "/source";
    const QByteArray big = randomBytes(3 * 1024 * 1024, 3);
    writeFile(source, "bundle/scene.usdc", big);

    auto host = hostRoom();
    Proxy proxy(host->port());
    auto node = joinThrough(proxy.address(), "Render Node", host->secret());
    CHECK(node->state() == Room::State::Open, "the node didn't join through the proxy");
    auto* inbox = new TransferInbox(node.get(), "transfer", inboxIn(dir), node.get());
    InboxWatch inboxWatch(inbox);

    Transfer::Options options = quick();
    options.windowBytes = 256 * 1024;  // Slow enough to cut in the middle
    options.messagesPerSecond = 40;
    QString error;
    Transfer* transfer =
        Transfer::send(host.get(), "transfer", node->me().id, manifestOf(source), options, host.get(), &error);
    CHECK(transfer, "couldn't send: %s", qPrintable(error));
    Watch watch(transfer);
    CHECK(waitUntil([&] { return transfer->bytesDone() > big.size() / 2; }, 20000), "nothing arrived before the cut");

    proxy.cut();
    CHECK(waitUntil([&] { return node->state() == Room::State::Reconnecting; }), "the node didn't lose the host");
    const qint64 before = inboxWatch.started.isEmpty() ? 0 : inboxWatch.started.first()->bytesDone();
    CHECK(before > 0 && before < big.size(), "the node had %lld bytes at the cut", qlonglong(before));
    proxy.resume();

    CHECK(waitUntil([&] { return watch.ended() && inboxWatch.received.size() == 1; }, 30000),
          "the transfer didn't finish after the cut");
    CHECK(watch.finished, "the sender says: %s", qPrintable(watch.reason));
    CHECK(watch.resumed, "it started again from the beginning");
    // What arrived before the cut wasn't sent again: only the pieces that were on their
    // way (at most a window, here a sixth of what had arrived).
    CHECK(transfer->bytesSent() < big.size() + before, "%lld bytes went for %lld, %lld of them there before the cut",
          qlonglong(transfer->bytesSent()), qlonglong(big.size()), qlonglong(before));
    CHECK(inboxWatch.started.size() == 1, "the inbox started %lld times", qlonglong(inboxWatch.started.size()));
    CHECK(readFile(inboxWatch.received.first()->directory() + "/bundle/scene.usdc") == big, "the file arrived different");
}

USDCC_TEST(peers_transfer_corrupt_chunk_refused) {
    QTemporaryDir dir;
    auto host = hostRoom();
    auto node = joinRoom(*host, "Render Node");
    auto* inbox = new TransferInbox(node.get(), "transfer", inboxIn(dir), node.get());
    InboxWatch inboxWatch(inbox);

    // The host speaks for a sender, by hand.
    Channel* channel = host->channel("transfer", Delivery::Direct);
    Inbox replies;
    replies.listen(channel);
    const PeerId to = node->me().id;
    const QByteArray real = randomBytes(100000, 4);
    const QJsonArray files{QJsonObject{{"path", "frame.exr"}, {"size", real.size()}, {"sha256", sha256(real)}}};
    const auto reply = [&](const QString& id, const QString& type) -> const Message* {
        for (const Message& m : replies.messages) {
            if (m.type == type && m.body.value("id").toString() == id) {
                return &m;
            }
        }
        return nullptr;
    };

    // A piece whose bytes aren't the file's: the file doesn't match its hash.
    channel->send("offer", {{"id", "corrupt"}, {"files", files}, {"chunk", 65536}}, {}, to);
    CHECK(waitUntil([&] { return reply("corrupt", "accept"); }), "the offer wasn't accepted");
    CHECK(reply("corrupt", "accept")->body.value("next").toInteger() == 0, "it starts somewhere else");
    QByteArray tampered = real.mid(65536);
    tampered[100] = char(tampered[100] ^ 0x55);
    channel->send("chunk", {{"id", "corrupt"}, {"i", 0}, {"f", 0}, {"o", 0}}, real.left(65536), to);
    channel->send("chunk", {{"id", "corrupt"}, {"i", 1}, {"f", 0}, {"o", 65536}}, tampered, to);
    CHECK(waitUntil([&] { return reply("corrupt", "refuse"); }), "a corrupt piece was taken");
    CHECK(reply("corrupt", "refuse")->body.value("reason").toString().contains("sha256"), "refused for: %s",
          qPrintable(reply("corrupt", "refuse")->body.value("reason").toString()));
    CHECK(!reply("corrupt", "done") && inboxWatch.received.isEmpty(), "a corrupt bundle arrived");
    const QString bundle = inbox->bundleDirectory(TransferManifest::fromJson(files)->hash());
    CHECK(!QFileInfo::exists(bundle) && !QFileInfo::exists(bundle + ".part/frame.exr"),
          "the corrupt file is in the inbox");
    CHECK(inboxWatch.started.size() == 1 && inboxWatch.started.first()->state() == Transfer::State::Failed,
          "the inbox's transfer didn't fail");

    // A piece of the wrong size is refused at once.
    channel->send("offer", {{"id", "short"}, {"files", files}, {"chunk", 65536}}, {}, to);
    CHECK(waitUntil([&] { return reply("short", "accept"); }), "the second offer wasn't accepted");
    channel->send("chunk", {{"id", "short"}, {"i", 0}, {"f", 0}, {"o", 0}}, real.left(1000), to);
    CHECK(waitUntil([&] { return reply("short", "refuse"); }), "a short piece was taken");

    // The real bytes go through (the bundle was never marked as had).
    channel->send("offer", {{"id", "good"}, {"files", files}, {"chunk", 65536}}, {}, to);
    CHECK(waitUntil([&] { return reply("good", "accept"); }), "the third offer wasn't accepted");
    channel->send("chunk", {{"id", "good"}, {"i", 0}, {"f", 0}, {"o", 0}}, real.left(65536), to);
    channel->send("chunk", {{"id", "good"}, {"i", 1}, {"f", 0}, {"o", 65536}}, real.mid(65536), to);
    CHECK(waitUntil([&] { return reply("good", "done"); }), "the real bytes weren't taken");
    CHECK(readFile(bundle + "/frame.exr") == real, "the file arrived different");
}

USDCC_TEST(peers_transfer_cache_hit) {
    QTemporaryDir dir;
    const QString source = dir.path() + "/source";
    writeFile(source, "scene.usda", "#usda 1.0\n");
    writeFile(source, "textures/a.png", randomBytes(150000, 5));

    auto host = hostRoom();
    auto node = joinRoom(*host, "Render Node");
    auto* inbox = new TransferInbox(node.get(), "transfer", inboxIn(dir), node.get());
    InboxWatch inboxWatch(inbox);
    const TransferManifest manifest = manifestOf(source);

    Transfer* first = Transfer::send(host.get(), "transfer", node->me().id, manifest, quick(), host.get());
    Watch firstWatch(first);
    CHECK(waitUntil([&] { return firstWatch.ended(); }) && firstWatch.finished, "the first transfer failed: %s",
          qPrintable(firstWatch.reason));
    CHECK(first->bytesSent() == manifest.totalBytes(), "the first transfer sent %lld bytes",
          qlonglong(first->bytesSent()));

    // The same bundle again: the inbox has it, and nothing is sent.
    Transfer* second = Transfer::send(host.get(), "transfer", node->me().id, manifest, quick(), host.get());
    Watch secondWatch(second);
    CHECK(waitUntil([&] { return secondWatch.ended() && inboxWatch.received.size() == 2; }),
          "the second transfer didn't end");
    CHECK(secondWatch.finished && second->bytesSent() == 0, "the bundle was sent again: %lld bytes",
          qlonglong(second->bytesSent()));
    CHECK(second->bytesDone() == manifest.totalBytes(), "the second transfer isn't complete");
    CHECK(inboxWatch.started.size() == 1, "the inbox received it twice");
    CHECK(inboxWatch.received[1]->directory() == inboxWatch.received[0]->directory(), "two places for one bundle");
}

USDCC_TEST(peers_transfer_unsafe_paths_refused) {
    QTemporaryDir dir;
    auto host = hostRoom();
    auto node = joinRoom(*host, "Render Node");
    TransferInbox::Options options = inboxIn(dir);
    options.maxTransferBytes = 1024 * 1024;
    auto* inbox = new TransferInbox(node.get(), "transfer", options, node.get());
    InboxWatch inboxWatch(inbox);

    Channel* channel = host->channel("transfer", Delivery::Direct);
    Inbox replies;
    replies.listen(channel);
    const QByteArray bytes = "pwned";
    int n = 0;
    QStringList ids;
    for (const char* path : {"../evil.txt", "a/../../evil.txt", "/evil.txt", "C:/evil.txt", "C:evil.txt",
                             "..\\evil.txt", "a/./b.txt", "a//b.txt"}) {
        const QString id = QStringLiteral("unsafe-%1").arg(++n);
        ids.append(id);
        const QJsonArray files{QJsonObject{{"path", path}, {"size", bytes.size()}, {"sha256", sha256(bytes)}}};
        channel->send("offer", {{"id", id}, {"files", files}, {"chunk", 65536}}, {}, node->me().id);
        // As if it had been accepted: a piece follows at once.
        channel->send("chunk", {{"id", id}, {"i", 0}, {"f", 0}, {"o", 0}}, bytes, node->me().id);
    }
    // Over the inbox's limit.
    const QJsonArray huge{QJsonObject{{"path", "big.bin"}, {"size", 2 * 1024 * 1024}, {"sha256", sha256("x")}}};
    channel->send("offer", {{"id", "too-big"}, {"files", huge}, {"chunk", 65536}}, {}, node->me().id);
    ids.append("too-big");

    const auto refusedAll = [&] {
        int refused = 0;
        for (const Message& m : replies.messages) {
            refused += m.type == u"refuse" ? 1 : 0;
        }
        return refused == ids.size();
    };
    CHECK(waitUntil(refusedAll), "only %lld replies to %lld offers", qlonglong(replies.messages.size()),
          qlonglong(ids.size()));
    for (const Message& m : replies.messages) {
        CHECK(m.type == u"refuse", "an unsafe offer got '%s'", qPrintable(m.type));
    }
    CHECK(inboxWatch.refused.size() == ids.size() && inboxWatch.started.isEmpty(), "the inbox took an unsafe offer");
    CHECK(!QFileInfo::exists(dir.path() + "/evil.txt") && !QFileInfo::exists(dir.path() + "/inbox/evil.txt") &&
              !QFileInfo::exists(QDir::rootPath() + "evil.txt"),
          "a file was written outside the inbox");
    QDir inboxDir(inbox->directory());
    CHECK(inboxDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(), "the inbox has: %s",
          qPrintable(inboxDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).join(", ")));

    // A source that is a symbolic link isn't sent (where links can be made here).
    const QString source = dir.path() + "/source";
    writeFile(source, "real.txt", "real");
    if (QFile::link(source + "/real.txt", source + "/link.txt") && QFileInfo(source + "/link.txt").isSymbolicLink()) {
        QString error;
        CHECK(!TransferManifest::fromDirectory(source, &error) && error.contains("link"),
              "a symbolic link went into a manifest");
    }
}

USDCC_TEST(peers_transfer_cancel) {
    QTemporaryDir dir;
    const QString source = dir.path() + "/source";
    writeFile(source, "big.bin", randomBytes(2 * 1024 * 1024, 6));

    auto host = hostRoom();
    auto node = joinRoom(*host, "Render Node");
    auto* inbox = new TransferInbox(node.get(), "transfer", inboxIn(dir), node.get());
    InboxWatch inboxWatch(inbox);
    const TransferManifest manifest = manifestOf(source);
    Transfer::Options slow = quick();
    slow.messagesPerSecond = 20;

    // The receiver cancels: the sender hears, and what arrived is thrown away.
    Transfer* first = Transfer::send(host.get(), "transfer", node->me().id, manifest, slow, host.get());
    Watch firstWatch(first);
    CHECK(waitUntil([&] { return !inboxWatch.started.isEmpty() && inboxWatch.started.first()->bytesDone() > 0; }),
          "nothing arrived");
    Transfer* incoming = inboxWatch.started.first();
    Watch incomingWatch(incoming);
    incoming->cancel("Not now");
    CHECK(incoming->state() == Transfer::State::Cancelled && incomingWatch.failed, "the receiver didn't cancel");
    CHECK(waitUntil([&] { return firstWatch.ended(); }), "the sender didn't hear of it");
    CHECK(first->state() == Transfer::State::Cancelled && firstWatch.reason == "Not now", "the sender says %s: %s",
          first->state() == Transfer::State::Failed ? "failed" : "something else", qPrintable(firstWatch.reason));
    const QString bundle = inbox->bundleDirectory(manifest.hash());
    CHECK(!QFileInfo::exists(bundle + ".part") && !QFileInfo::exists(bundle), "the cancelled bundle is kept");
    const qint64 sentAtCancel = first->bytesSent();

    // The sender cancels: the receiver hears.
    Transfer* second = Transfer::send(host.get(), "transfer", node->me().id, manifest, slow, host.get());
    Watch secondWatch(second);
    CHECK(waitUntil([&] { return inboxWatch.started.size() == 2; }), "the second offer wasn't accepted");
    Watch receivingWatch(inboxWatch.started[1]);
    second->cancel();
    CHECK(second->state() == Transfer::State::Cancelled, "the sender didn't cancel");
    CHECK(waitUntil([&] { return receivingWatch.ended(); }), "the receiver didn't hear of it");
    CHECK(inboxWatch.started[1]->state() == Transfer::State::Cancelled, "the receiver says: %s",
          qPrintable(receivingWatch.reason));
    CHECK(inboxWatch.received.isEmpty() && !QFileInfo::exists(bundle), "a cancelled bundle arrived");

    // Nothing more goes once cancelled.
    QTest::qWait(300);
    CHECK(first->bytesSent() == sentAtCancel, "pieces went after the cancel");
}

// The receiver is asked (TransferInboxOptions::ask), and answers after the sender would
// otherwise have offered again: "wait" keeps it waiting. A no fails it with "Declined".
USDCC_TEST(peers_transfer_asks_first) {
    QTemporaryDir dir;
    const QString source = dir.path() + "/source";
    writeFile(source, "session.json", "{\"panels\": []}");
    auto host = hostRoom();
    auto sender = joinRoom(*host, "Sender");
    auto receiver = joinRoom(*host, "Receiver");
    TransferInbox::Options options = inboxIn(dir);
    QList<std::function<void(bool)>> questions;
    QStringList askedAbout;
    options.ask = [&](const Peer& from, const TransferManifest& manifest, std::function<void(bool)> answer) {
        askedAbout.append(from.name + ": " + manifest.files.first().path);
        questions.append(std::move(answer));
    };
    auto* inbox = new TransferInbox(receiver.get(), "transfer", options, receiver.get());
    InboxWatch inboxWatch(inbox);

    Transfer::Options quickStall = quick();
    quickStall.stallMs = 300;  // It offers again often, and hears "wait"
    quickStall.giveUpMs = 1500;
    QString error;
    Transfer* yes = Transfer::send(sender.get(), "transfer", receiver->me().id, manifestOf(source), quickStall,
                                   sender.get(), &error);
    Watch yesWatch(yes);
    CHECK(waitUntil([&] { return questions.size() == 1; }), "the receiver wasn't asked");
    CHECK(askedAbout == QStringList{"Sender: session.json"} && inbox->isAsking(yes->id()), "asked about %s",
          qPrintable(askedAbout.join(", ")));
    QTest::qWait(2500);  // Longer than the sender would wait without hearing "wait"
    CHECK(!yesWatch.ended() && yes->state() == Transfer::State::Offered, "the sender stopped waiting: %s",
          qPrintable(yesWatch.reason));
    questions.first()(true);
    CHECK(waitUntil([&] { return yesWatch.finished && inboxWatch.received.size() == 1; }), "it didn't arrive after yes");

    writeFile(source, "other.txt", "no thanks");
    Transfer* no = Transfer::send(sender.get(), "transfer", receiver->me().id, manifestOf(source), quickStall,
                                  sender.get(), &error);
    Watch noWatch(no);
    CHECK(waitUntil([&] { return questions.size() == 2; }), "the receiver wasn't asked again");
    questions.last()(false);
    CHECK(waitUntil([&] { return noWatch.failed; }) && noWatch.reason == "Declined", "a no ended it with \"%s\"",
          qPrintable(noWatch.reason));
    CHECK(inboxWatch.received.size() == 1, "a declined bundle arrived");
}
