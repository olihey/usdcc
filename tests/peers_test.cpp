// usdcc_peers (docs/PEERS.md, phase 1a): rooms and channels over 127.0.0.1, with a
// host and its members in one process. A member that has to fall behind lives on a
// thread of its own, whose event loop the test blocks: its socket then stops reading,
// the operating system's buffers fill, and the host's queue for it grows. Guests that
// are pages are raw QWebSockets that send an Origin, as a browser does. Runs in
// usdcc_peers_tests.
#include "peers_test_support.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QWebSocket>

#include <functional>
#include <memory>

using namespace usdcc::peers;
using namespace usdcc::peers::test;

USDCC_TEST(peers_frame_round_trip) {
    Message message;
    message.channel = "com.example.notes";
    message.type = "add";
    message.from = "a1";
    message.to = "b2";
    message.seq = 7;
    message.body = {{"text", "hi"}};
    message.bytes = QByteArray("\x00\x01\xff", 3);
    const std::optional<Frame> frame = decodeFrame(encodeFrame(message, true));
    CHECK(frame && frame->latest, "a frame didn't come back");
    if (frame) {
        const Message& m = frame->message;
        CHECK(m.channel == message.channel && m.type == message.type && m.from == message.from &&
                  m.to == message.to && m.seq == 7 && m.body == message.body && m.bytes == message.bytes,
              "a field changed on the way: %s", encodeFrame(m).constData() + 4);
    }
    // The header's length, big-endian, then the header.
    const QByteArray plain = encodeFrame(message);
    const quint32 length = (quint8(plain[0]) << 24) | (quint8(plain[1]) << 16) | (quint8(plain[2]) << 8) | quint8(plain[3]);
    CHECK(int(length) == plain.size() - 4 - 3, "the length prefix is %u for %lld bytes", length, qlonglong(plain.size()));

    QString error;
    CHECK(!decodeFrame(QByteArray("\x00\x00", 2), &error), "two bytes are a frame");
    CHECK(!decodeFrame(QByteArray("\x00\x00\x00\x10{}", 6), &error), "a header longer than its frame is accepted");
    CHECK(!decodeTextFrame("[1]", &error), "an array is a header");
    CHECK(!decodeTextFrame(R"({"type":"say"})", &error), "a frame without a channel is accepted");
    CHECK(!decodeTextFrame(R"({"channel":"chat","body":[1]})", &error), "a body that isn't an object is accepted");
    const std::optional<Frame> text = decodeTextFrame(R"({"channel":"chat","type":"say","body":{"text":"hi"}})");
    CHECK(text && text->message.body.value("text").toString() == "hi" && text->message.bytes.isEmpty(),
          "a text frame didn't decode");
}

USDCC_TEST(peers_channel_names) {
    for (const char* name : {"chat", "presence", "view", "stage/12", "com.example.notes.pins", "a-b_c"}) {
        CHECK(isValidChannelName(name), "%s is refused", name);
    }
    for (const char* name : {"", "room", ".chat", "chat.", "a..b", "a//b", "/a", "has space", "ünï", "a?b"}) {
        CHECK(!isValidChannelName(QString::fromUtf8(name)), "'%s' is a channel name", name);
    }
    CHECK(!isValidChannelName(QString(129, 'a')), "a 129-character name is accepted");
    CHECK(isUsdccChannel("chat") && isUsdccChannel("stage/12"), "dotless names aren't usdcc's");
    CHECK(!isUsdccChannel("com.example.notes"), "a plugin's name is usdcc's");

    auto host = hostRoom();
    CHECK(host->channel("room") == nullptr && host->channel("bad name") == nullptr, "an invalid channel opened");
    Channel* chat = host->channel("chat");
    CHECK(chat && host->channel("chat") == chat && host->findChannel("chat") == chat, "a channel isn't opened once");
}

USDCC_TEST(peers_ordered_one_order_on_three_members) {
    auto host = hostRoom();
    Channel* hostChat = host->channel("chat");
    auto a = joinRoom(*host, "A");
    auto b = joinRoom(*host, "B");
    auto c = joinRoom(*host, "C");
    for (Room* member : {a.get(), b.get(), c.get()}) {
        CHECK(member->state() == Room::State::Open, "%s didn't join", qPrintable(member->me().name));
    }
    CHECK(host->peers().size() == 4, "the host has %lld in the room", qlonglong(host->peers().size()));
    CHECK(waitUntil([&] { return a->peers().size() == 4; }), "A sees %lld in the room", qlonglong(a->peers().size()));
    CHECK(a->hostPeer().kind == Peer::Kind::Host && a->me().name == "A" && !a->me().id.isEmpty(),
          "A doesn't know who it and the host are");

    Inbox hostInbox, aInbox, bInbox, cInbox;
    hostInbox.listen(hostChat);
    Channel* aChat = a->channel("chat");
    Channel* bChat = b->channel("chat");
    Channel* cChat = c->channel("chat");
    aInbox.listen(aChat);
    bInbox.listen(bChat);
    cInbox.listen(cChat);

    // Four senders at once, each sending as fast as it can.
    constexpr int kEach = 50;
    for (int i = 0; i < kEach; ++i) {
        hostChat->send("say", {{"by", "host"}, {"n", i}});
        aChat->send("say", {{"by", "A"}, {"n", i}});
        bChat->send("say", {{"by", "B"}, {"n", i}});
        cChat->send("say", {{"by", "C"}, {"n", i}});
        if (i % 7 == 0) {
            QCoreApplication::processEvents();
        }
    }
    const auto all = [&](const Inbox& inbox) { return inbox.messages.size() == 4 * kEach; };
    CHECK(waitUntil([&] { return all(hostInbox) && all(aInbox) && all(bInbox) && all(cInbox); }),
          "not everything arrived: host %lld, A %lld, B %lld, C %lld", qlonglong(hostInbox.messages.size()),
          qlonglong(aInbox.messages.size()), qlonglong(bInbox.messages.size()), qlonglong(cInbox.messages.size()));

    const auto order = [](const Inbox& inbox) {
        QStringList result;
        for (const Message& m : inbox.messages) {
            result.append(QStringLiteral("%1:%2:%3").arg(m.seq).arg(m.body.value("by").toString()).arg(m.body.value("n").toInt()));
        }
        return result;
    };
    const QStringList hostOrder = order(hostInbox);
    CHECK(order(aInbox) == hostOrder && order(bInbox) == hostOrder && order(cInbox) == hostOrder,
          "the members saw different orders");
    // Numbered by the host, one after another; each sender's own in the order it sent them.
    qint64 lastSeq = 0;
    QMap<QString, int> lastOf;
    bool numbered = true;
    bool keptOwnOrder = true;
    for (const Message& m : bInbox.messages) {
        numbered = numbered && m.seq > lastSeq;
        lastSeq = m.seq;
        const QString by = m.body.value("by").toString();
        keptOwnOrder = keptOwnOrder && m.body.value("n").toInt() == lastOf.value(by, -1) + 1;
        lastOf[by] = m.body.value("n").toInt();
    }
    CHECK(numbered, "seq doesn't grow with each message");
    CHECK(keptOwnOrder, "a sender's messages arrived out of the order it sent them");
    // `from` is the host's word: B's messages say B, whoever they arrive at.
    bool fromSet = true;
    for (const Message& m : cInbox.messages) {
        const QString by = m.body.value("by").toString();
        const PeerId expected = by == "host" ? host->me().id : idOf(*host, by);
        fromSet = fromSet && m.from == expected;
    }
    CHECK(fromSet, "a message's sender isn't the one the host knows");
}

USDCC_TEST(peers_latest_only_replaces_one_still_waiting) {
    auto host = hostRoom();
    Channel* view = host->channel("view", Delivery::LatestOnly);
    view->setSenders(Senders::Host);
    auto a = joinRoom(*host, "A");
    Inbox aInbox;
    aInbox.listen(a->channel("view", Delivery::LatestOnly));
    ThreadedMember b(*host, "B", "view", Delivery::LatestOnly);
    CHECK(b.opened, "B didn't join: %s", qPrintable(b.closedReason));
    const PeerId bId = idOf(*host, "B");

    b.block();
    const QByteArray picture(64 * 1024, 'x');
    int sent = 0;
    // Until a frame still waits for B once the socket has had its chance to write: the
    // operating system's buffers are full. One can wait for a moment only because Qt
    // hasn't written the ones before it yet, with the system's buffers far from full (on
    // Linux, loopback's hold megabytes).
    while (sent < 5000) {
        view->send("frame", {{"n", sent++}}, picture);
        if (!waitUntil([&] { return host->queuedBytes(bId) == 0; }, 200)) {
            break;
        }
    }
    CHECK(host->queuedBytes(bId) > 0, "nothing waited for B after %d frames", sent);
    const int firstWaiting = sent - 1;
    // Each of these replaces the one waiting.
    for (int i = 0; i < 100; ++i) {
        view->send("frame", {{"n", sent++}}, picture);
        QCoreApplication::processEvents();
    }
    CHECK(host->queuedBytes(bId) <= picture.size() + 1024, "frames pile up for B: %lld bytes waiting",
          qlonglong(host->queuedBytes(bId)));
    CHECK(host->peer(bId).isValid(), "B was dropped, but a latest-only channel shouldn't make it too slow");

    b.unblock();
    CHECK(waitUntil([&] { return !b.messages.isEmpty() && b.messages.last().body.value("n").toInt() == sent - 1; }, 10000),
          "B never got the last frame (%lld arrived)", qlonglong(b.messages.size()));
    bool increasing = true;
    bool pictures = true;
    for (qsizetype i = 0; i < b.messages.size(); ++i) {
        const int n = b.messages[i].body.value("n").toInt();
        increasing = increasing && (i == 0 || n > b.messages[i - 1].body.value("n").toInt());
        pictures = pictures && b.messages[i].bytes == picture;
    }
    CHECK(increasing, "an older frame arrived after a newer one");
    CHECK(pictures, "a frame lost its picture");
    // Frames already handed to the socket before B stopped reading all arrive; of the
    // hundred sent while one waited, nearly all were replaced.
    CHECK(b.messages.size() <= firstWaiting + 10, "B got %lld of %d frames; %d were sent before one had to wait",
          qlonglong(b.messages.size()), sent, firstWaiting);
    CHECK(waitUntil([&] { return !aInbox.messages.isEmpty() && aInbox.messages.last().body.value("n").toInt() == sent - 1; }),
          "A never got the last frame");
    CHECK(aInbox.messages.first().seq == 0, "a latest-only message has a seq");
    // Nobody but the host may send here.
    a->channel("view")->send("frame", {{"n", -1}});
    CHECK(waitUntil([&] { return !aInbox.refusals.isEmpty(); }), "A's frame on the host's channel wasn't refused");
}

USDCC_TEST(peers_slow_member_is_dropped) {
    HostOptions options;
    options.limits.maxQueuedBytes = 4 * 1024 * 1024;
    auto host = hostRoom(options);
    Channel* log = host->channel("log");
    auto a = joinRoom(*host, "A");
    Inbox aInbox;
    aInbox.listen(a->channel("log"));
    ThreadedMember b(*host, "B", "log", Delivery::Ordered);
    CHECK(b.opened, "B didn't join: %s", qPrintable(b.closedReason));
    const PeerId bId = idOf(*host, "B");
    QString bLeft;
    QObject::connect(host.get(), &Room::peerLeft, host.get(), [&](const Peer& peer, const QString& reason) {
        if (peer.id == bId) {
            bLeft = reason;
        }
    });
    QString aLeft;
    QObject::connect(a.get(), &Room::peerLeft, a.get(), [&](const Peer& peer, const QString& reason) {
        if (peer.id == bId) {
            aLeft = reason;
        }
    });

    b.block();
    const QByteArray chunk(128 * 1024, 'y');
    int sent = 0;
    while (bLeft.isEmpty() && sent < 4000) {
        log->send("chunk", {{"n", sent++}}, chunk);
        QCoreApplication::processEvents();
        // A keeps up, so only B falls behind.
        waitUntil([&] { return aInbox.messages.size() >= sent - 8; });
    }
    CHECK(bLeft.contains("slow"), "B wasn't dropped after %d messages (%s)", sent, qPrintable(bLeft));
    CHECK(!host->peer(bId).isValid(), "B is still in the room");
    CHECK(waitUntil([&] { return aInbox.messages.size() == sent && !aLeft.isEmpty(); }),
          "A got %lld of %d, and heard B leave: %s", qlonglong(aInbox.messages.size()), sent, qPrintable(aLeft));
    bool inOrder = true;
    for (qsizetype i = 0; i < aInbox.messages.size(); ++i) {
        inOrder = inOrder && aInbox.messages[i].body.value("n").toInt() == i;
    }
    CHECK(inOrder, "A's messages came out of order");
    // A dropped member rejoins, and gets the room's state afresh (PEERS.md §7.4).
    b.unblock();
    CHECK(waitUntil([&] { return b.resumed; }, 10000), "B didn't rejoin after being dropped: %s", qPrintable(b.closedReason));
    CHECK(!b.resumedCaughtUp, "B was caught up, but it had been dropped");
}

USDCC_TEST(peers_wrong_secret_is_refused) {
    auto host = hostRoom();
    int joined = 0;
    QObject::connect(host.get(), &Room::peerJoined, host.get(), [&] { ++joined; });
    QString reason;
    auto mallory = joinRoom(*host, "Mallory", "not-the-secret", &reason);
    CHECK(mallory->state() == Room::State::Closed, "a wrong secret got in");
    CHECK(reason.contains("Wrong secret"), "refused, but for: %s", qPrintable(reason));
    CHECK(joined == 0 && host->peers().size() == 1, "the host counts a refused member");

    // Another protocol is refused with both versions named.
    RawMember future;
    CHECK(future.hello(*host, host->secret(), "Future", 99), "no answer to a hello of protocol 99");
    const Message* denied = future.find("room", "denied");
    CHECK(denied, "protocol 99 got in");
    if (denied) {
        const QString text = denied->body.value("reason").toString();
        CHECK(text.contains("protocol 1") && text.contains("protocol 99"), "the refusal doesn't name both: %s", qPrintable(text));
    }
    CHECK(waitUntil([&] { return future.disconnected; }), "a refused member is still connected");

    // Anything before hello is refused too.
    RawMember rude;
    rude.open(*host);
    CHECK(waitUntil([&] { return rude.connected; }), "couldn't connect");
    rude.sendText("chat", "say", {{"text", "hi"}});
    CHECK(waitUntil([&] { return rude.find("room", "denied") && rude.disconnected; }), "a message before hello was let through");
}

USDCC_TEST(peers_guests) {
    HostOptions options;
    options.guests = true;
    auto host = hostRoom(options);
    Channel* chat = host->channel("chat");
    chat->setOpenToGuests(true);
    Channel* notes = host->channel("notes");
    Inbox hostChat, hostNotes;
    hostChat.listen(chat);
    hostNotes.listen(notes);
    auto peer = joinRoom(*host, "Peer");
    Inbox peerChat, peerNotes;
    peerChat.listen(peer->channel("chat"));
    peerNotes.listen(peer->channel("notes"));

    // A page on the room's own address, as a browser opens it.
    const QString ownOrigin = QStringLiteral("http://127.0.0.1:%1").arg(host->port());
    RawMember guest(ownOrigin);
    CHECK(guest.hello(*host, host->guestSecret(), "Watcher"), "no answer to the guest's hello");
    const Message* welcome = guest.find("room", "welcome");
    CHECK(welcome, "the guest wasn't welcomed");
    if (!welcome) {
        return;
    }
    const PeerId guestId = welcome->body.value("id").toString();
    CHECK(host->peer(guestId).kind == Peer::Kind::Guest, "the page joined as something other than a guest");
    QStringList offered;
    for (const QJsonValue& value : welcome->body.value("channels").toArray()) {
        offered.append(value.toObject().value("name").toString());
    }
    CHECK(offered == QStringList{"chat"}, "the guest is told of: %s", qPrintable(offered.join(", ")));

    // A channel not opened to guests: refused, and nobody gets it.
    guest.sendText("notes", "add", {{"text", "sneaky"}});
    CHECK(waitUntil([&] { return guest.find("room", "refused"); }), "the guest's note wasn't refused");
    if (const Message* refused = guest.find("room", "refused")) {
        CHECK(refused->body.value("channel").toString() == "notes", "refused the wrong channel");
    }
    // Its own channel works.
    guest.sendText("chat", "say", {{"text", "hello"}});
    CHECK(waitUntil([&] { return !hostChat.messages.isEmpty() && !peerChat.messages.isEmpty(); }), "the guest's chat didn't arrive");
    CHECK(hostChat.messages.value(0).from == guestId, "the guest's chat isn't from the guest");
    CHECK(hostNotes.messages.isEmpty() && peerNotes.messages.isEmpty(), "the guest's note arrived");

    // Nothing on a closed channel reaches the guest; the open one does.
    notes->send("add", {{"text", "for peers"}});
    chat->send("say", {{"text", "for all"}});
    CHECK(waitUntil([&] { return guest.count("chat") == 2 && peerNotes.messages.size() == 1; }),
          "the host's messages didn't arrive");
    CHECK(guest.count("notes") == 0, "the guest got a note");

    // A page can't use the peers' secret, and another site's page can't connect at all.
    RawMember page(ownOrigin);
    page.hello(*host, host->secret(), "Page");
    CHECK(page.find("room", "denied") && !page.find("room", "welcome"), "a page joined with the peers' secret");
    RawMember evil(QStringLiteral("http://evil.example"));
    evil.open(*host);
    CHECK(waitUntil([&] { return evil.connected || evil.disconnected; }) && !evil.connected,
          "another site's page connected");
    RawMember tunnel(QStringLiteral("https://watch.example.net"));
    tunnel.open(*host);
    CHECK(waitUntil([&] { return tunnel.connected || tunnel.disconnected; }) && !tunnel.connected,
          "an origin the host didn't allow connected");

    // A program with the guests' secret is a guest as well.
    auto program = joinRoom(*host, "Program", host->guestSecret());
    CHECK(program->state() == Room::State::Open && program->me().kind == Peer::Kind::Guest, "the program isn't a guest");
    Inbox programNotes;
    programNotes.listen(program->channel("notes"));
    program->channel("notes")->send("add");
    CHECK(waitUntil([&] { return !programNotes.refusals.isEmpty(); }), "Channel::refused wasn't emitted");
}

USDCC_TEST(peers_guests_off_and_allowed_origins) {
    auto closedRoom = hostRoom();  // Takes no guests
    RawMember page(QStringLiteral("http://127.0.0.1:%1").arg(closedRoom->port()));
    page.open(*closedRoom);
    CHECK(waitUntil([&] { return page.connected || page.disconnected; }) && !page.connected,
          "a page connected to a room without guests");
    CHECK(closedRoom->guestSecret().isEmpty() && closedRoom->guestLink().isEmpty(), "a room without guests has a guest link");

    HostOptions options;
    options.guests = true;
    options.allowedOrigins = {"https://watch.example.net"};
    auto room = hostRoom(options);
    RawMember tunnel(QStringLiteral("https://watch.example.net"));
    CHECK(tunnel.hello(*room, room->guestSecret(), "Far") && tunnel.find("room", "welcome"),
          "the allowed origin was refused");
    const QUrl link = room->guestLink();
    CHECK(link.scheme() == "http" && link.host() == "127.0.0.1" && link.port() == room->port() &&
              link.fragment() == "s=" + room->guestSecret(),
          "the guest link is %s", qPrintable(link.toString()));
}

USDCC_TEST(peers_page) {
    QNetworkAccessManager network;
    const auto get = [&](const Room& room, const QString& path, QByteArray* type = nullptr) -> std::pair<int, QByteArray> {
        QNetworkReply* reply = network.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(room.port()).arg(path))));
        waitUntil([&] { return reply->isFinished(); });
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (type) {
            *type = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        }
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        return {status, body};
    };

    HostOptions options;
    options.guests = true;
    options.page = "<p>Watching</p>";
    auto room = hostRoom(options);
    QByteArray type;
    auto [status, body] = get(*room, "/", &type);
    CHECK(status == 200 && body == "<p>Watching</p>" && type.startsWith("text/html"), "/ gave %d %s (%s)", status,
          body.constData(), type.constData());
    CHECK(get(*room, "/nothing").first == 404, "a missing page isn't 404");

    auto noGuests = hostRoom();
    CHECK(get(*noGuests, "/").first == 404, "a room without guests serves a page");

    options.page.clear();
    auto plain = hostRoom(options);
    CHECK(get(*plain, "/").second.contains("usdcc room"), "the built-in page isn't served");

    QTemporaryDir folder;
    const auto writeFile = [&](const QString& name, const QByteArray& content) {
        QFile file(QDir(folder.path()).filePath(name));
        CHECK(file.open(QIODevice::WriteOnly) && file.write(content) == content.size(), "couldn't write %s", qPrintable(name));
    };
    writeFile("index.html", "<script src=app.js></script>");
    writeFile("app.js", "let x = 1;");
    options.pageDirectory = folder.path();
    auto served = hostRoom(options);
    CHECK(get(*served, "/").second == "<script src=app.js></script>", "the folder's index.html isn't /");
    auto [scriptStatus, scriptBody] = get(*served, "/app.js", &type);
    CHECK(scriptStatus == 200 && scriptBody == "let x = 1;" && type.startsWith("text/javascript"), "app.js gave %d (%s)",
          scriptStatus, type.constData());
    CHECK(get(*served, "/%2e%2e/secret.txt").first == 404 && get(*served, "/sub/app.js").first == 404,
          "a path outside the folder is served");
}

USDCC_TEST(peers_who_may_send) {
    auto host = hostRoom();
    Channel* notify = host->channel("notify");
    notify->setSenders(Senders::Host);
    auto a = joinRoom(*host, "A");
    auto b = joinRoom(*host, "B");
    Inbox aNotify, bNotify;
    aNotify.listen(a->channel("notify"));
    bNotify.listen(b->channel("notify"));

    a->channel("notify")->send("toast", {{"text", "from A"}});
    CHECK(waitUntil([&] { return !aNotify.refusals.isEmpty(); }), "A's message on a host-only channel wasn't refused");
    notify->send("toast", {{"text", "from the host"}});
    CHECK(waitUntil([&] { return bNotify.messages.size() == 1; }), "the host's message didn't arrive");
    CHECK(bNotify.messages.value(0).body.value("text").toString() == "from the host", "B got A's message");

    notify->setNamedSenders({a->me().id});
    a->channel("notify")->send("toast", {{"text", "A, allowed"}});
    b->channel("notify")->send("toast", {{"text", "B, not"}});
    CHECK(waitUntil([&] { return bNotify.messages.size() == 2 && !bNotify.refusals.isEmpty(); }),
          "the named sender wasn't let through, or the other wasn't refused");
    CHECK(bNotify.messages.value(1).body.value("text").toString() == "A, allowed", "the wrong message got through");
    CHECK(notify->maySend(host->me()) && !notify->maySend(b->me()), "maySend disagrees");
}

USDCC_TEST(peers_direct) {
    auto host = hostRoom();
    Channel* nav = host->channel("nav", Delivery::Direct);
    host->channel("whisper");
    Inbox hostNav;
    hostNav.listen(nav);
    auto a = joinRoom(*host, "A");
    auto b = joinRoom(*host, "B");
    auto c = joinRoom(*host, "C");
    Inbox aNav, bNav, aWhisper, bWhisper, cWhisper;
    aNav.listen(a->channel("nav", Delivery::Direct));
    bNav.listen(b->channel("nav", Delivery::Direct));
    aWhisper.listen(a->channel("whisper"));
    bWhisper.listen(b->channel("whisper"));
    cWhisper.listen(c->channel("whisper"));

    // To nobody named, on a direct channel: the host.
    a->channel("nav")->send("orbit", {{"dx", 0.1}});
    CHECK(waitUntil([&] { return hostNav.messages.size() == 1; }), "the move didn't reach the host");
    CHECK(hostNav.messages.value(0).from == a->me().id, "the move isn't from A");

    // To one member.
    // Then a message to everyone from A: one sender's messages stay in the order sent,
    // so once B has the marker, the whisper has had its chance to arrive anywhere.
    a->channel("whisper")->send("psst", {}, {}, b->me().id);
    a->channel("whisper")->send("marker");
    CHECK(waitUntil([&] { return bWhisper.types().contains("marker") && cWhisper.types().contains("marker") &&
                                 aWhisper.types().contains("marker"); }),
          "the marker didn't arrive");
    CHECK(bWhisper.types() == QStringList({"psst", "marker"}), "B got: %s", qPrintable(bWhisper.types().join(", ")));
    CHECK(cWhisper.types() == QStringList({"marker"}) && aWhisper.types() == QStringList({"marker"}),
          "the whisper reached someone else");
    CHECK(bWhisper.messages.value(0).seq == 0 && bWhisper.messages.value(0).to == b->me().id, "a direct message has a seq");

    a->channel("whisper")->send("psst", {}, {}, "nobody");
    CHECK(waitUntil([&] { return !aWhisper.refusals.isEmpty(); }), "a message to nobody wasn't refused");
    CHECK(aNav.messages.isEmpty() && bNav.messages.isEmpty(), "a move reached another member");
}

USDCC_TEST(peers_limits) {
    {
        HostOptions options;
        options.limits.maxMembers = 2;
        auto host = hostRoom(options);
        auto a = joinRoom(*host, "A");
        auto b = joinRoom(*host, "B");
        QString reason;
        auto c = joinRoom(*host, "C", {}, &reason);
        CHECK(a->state() == Room::State::Open && b->state() == Room::State::Open, "the first two didn't join");
        CHECK(c->state() == Room::State::Closed && reason.contains("full"), "a third joined a room for two: %s", qPrintable(reason));
    }
    {
        HostOptions options;
        options.guests = true;
        options.limits.maxGuests = 1;
        auto host = hostRoom(options);
        auto first = joinRoom(*host, "G1", host->guestSecret());
        QString reason;
        auto second = joinRoom(*host, "G2", host->guestSecret(), &reason);
        CHECK(first->state() == Room::State::Open && second->state() == Room::State::Closed,
              "a second guest joined a room for one: %s", qPrintable(reason));
        auto peer = joinRoom(*host, "P");
        CHECK(peer->state() == Room::State::Open, "the guest limit kept out a peer");
    }
    {
        HostOptions options;
        options.limits.maxFrameBytes = 64 * 1024;
        auto host = hostRoom(options);
        QString left;
        QObject::connect(host.get(), &Room::peerLeft, host.get(), [&](const Peer&, const QString& reason) { left = reason; });
        auto a = joinRoom(*host, "A");
        a->channel("files")->send("big", {}, QByteArray(200 * 1024, 'z'));
        CHECK(waitUntil([&] { return !left.isEmpty(); }), "a frame over the limit was taken");
        CHECK(left.contains("size limit"), "dropped, but for: %s", qPrintable(left));
    }
    {
        HostOptions options;
        options.limits.messagesPerSecond = 20;
        options.limits.messageBurst = 40;
        auto host = hostRoom(options);
        QString left;
        QObject::connect(host.get(), &Room::peerLeft, host.get(), [&](const Peer&, const QString& reason) { left = reason; });
        auto a = joinRoom(*host, "A");
        Channel* chat = a->channel("chat");
        for (int i = 0; i < 200; ++i) {
            chat->send("say", {{"n", i}});
        }
        CHECK(waitUntil([&] { return !left.isEmpty(); }), "200 messages at once were taken");
        CHECK(left.contains("too many"), "dropped, but for: %s", qPrintable(left));
        CHECK(waitUntil([&] { return a->state() == Room::State::Closed; }), "the flooder doesn't know");
    }
    {
        HostOptions options;
        options.limits.helloTimeoutMs = 300;
        options.limits.pingIntervalMs = 100;
        options.limits.silenceTimeoutMs = 600;
        auto host = hostRoom(options);
        RawMember quiet;
        quiet.open(*host);
        CHECK(waitUntil([&] { return quiet.disconnected; }, 3000), "a connection without hello stays");

        // A member whose event loop stops answers no ping, and is gone after the silence.
        ThreadedMember b(*host, "B", "chat", Delivery::Ordered);
        CHECK(b.opened, "B didn't join");
        const PeerId bId = idOf(*host, "B");
        QString left;
        QObject::connect(host.get(), &Room::peerLeft, host.get(), [&](const Peer&, const QString& reason) { left = reason; });
        QTest::qWait(900);
        CHECK(host->peer(bId).isValid(), "an answering member was dropped");
        b.block();
        CHECK(waitUntil([&] { return !left.isEmpty(); }, 3000), "a silent member stays");
        CHECK(left.contains("Silent"), "dropped, but for: %s", qPrintable(left));
    }
}

USDCC_TEST(peers_remove_leave_and_close) {
    auto host = hostRoom();
    auto a = joinRoom(*host, "A");
    auto b = joinRoom(*host, "B");
    auto c = joinRoom(*host, "C");
    QString aClosed, bClosed, cClosed, bHeard;
    QObject::connect(a.get(), &Room::closed, a.get(), [&](const QString& r) { aClosed = r; });
    QObject::connect(b.get(), &Room::closed, b.get(), [&](const QString& r) { bClosed = r; });
    QObject::connect(c.get(), &Room::closed, c.get(), [&](const QString& r) { cClosed = r; });
    QObject::connect(b.get(), &Room::peerLeft, b.get(), [&](const Peer& p, const QString& r) { bHeard = p.name + ": " + r; });
    CHECK(waitUntil([&] { return b->peers().size() == 4; }), "B doesn't see everyone");

    host->remove(a->me().id, "Bye, A");
    CHECK(waitUntil([&] { return aClosed == "Bye, A" && bHeard == "A: Bye, A"; }), "A: %s; B heard: %s", qPrintable(aClosed),
          qPrintable(bHeard));
    CHECK(host->peers().size() == 3 && b->peers().size() == 3, "A is still counted");

    c->close();
    CHECK(cClosed == "Left the room", "C's own close says: %s", qPrintable(cClosed));
    CHECK(waitUntil([&] { return bHeard == "C: Left the room"; }), "B heard: %s", qPrintable(bHeard));

    QString hostClosed;
    QObject::connect(host.get(), &Room::closed, host.get(), [&](const QString& r) { hostClosed = r; });
    host->close("Done for today");
    CHECK(hostClosed == "Done for today" && host->state() == Room::State::Closed, "the host isn't closed");
    CHECK(waitUntil([&] { return bClosed == "Done for today"; }), "B: %s", qPrintable(bClosed));

    QString late;
    JoinOptions options;
    options.secret = host->secret();
    std::unique_ptr<Room> latecomer(Room::join(address(*host), options));
    QObject::connect(latecomer.get(), &Room::closed, latecomer.get(), [&](const QString& r) { late = r; });
    CHECK(waitUntil([&] { return latecomer->state() == Room::State::Closed; }), "joined a closed room");
    CHECK(!late.isEmpty(), "no reason given for a room that isn't there");
}
