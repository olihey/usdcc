// usdcc_peers, phase 1b (docs/PEERS.md §14): identities, the encrypted handshake and
// the room key, reconnecting, shared state, and who may join. A member that should be
// watched or cut off joins through a small TCP proxy in the test, which keeps every
// byte that passes and can drop the connection. Runs in usdcc_peers_tests.
#include "peers_test_support.h"

#include "usdcc/peers/identity.h"
#include "usdcc/peers/state.h"

#include <QJsonArray>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>

using namespace usdcc::peers;
using namespace usdcc::peers::test;

namespace {

// Passes bytes between members and a host, keeping a copy, and can cut them apart.
class Proxy : public QObject {
public:
    explicit Proxy(quint16 target) : m_target(target) {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* in = m_server.nextPendingConnection()) {
                auto* out = new QTcpSocket(this);
                in->setParent(this);
                m_pairs.append(std::make_pair(in, out));
                connect(in, &QTcpSocket::readyRead, this, [this, in, out] {
                    const QByteArray data = in->readAll();
                    captured += data;
                    out->write(data);
                });
                connect(out, &QTcpSocket::readyRead, this, [this, in, out] {
                    const QByteArray data = out->readAll();
                    captured += data;
                    in->write(data);
                });
                connect(in, &QTcpSocket::disconnected, out, &QTcpSocket::disconnectFromHost);
                connect(out, &QTcpSocket::disconnected, in, &QTcpSocket::disconnectFromHost);
                out->connectToHost(QHostAddress::LocalHost, m_target);
            }
        });
        resume();
    }
    QString address() const { return QStringLiteral("127.0.0.1:%1").arg(m_port); }
    // Drops every connection, and takes none until resume().
    void cut() {
        m_server.close();
        for (const auto& [in, out] : m_pairs) {
            in->abort();
            out->abort();
        }
        m_pairs.clear();
    }
    void resume() {
        m_server.listen(QHostAddress::LocalHost, m_port);
        m_port = m_server.serverPort();
    }

    QByteArray captured;

private:
    quint16 m_target;
    quint16 m_port = 0;
    QTcpServer m_server;
    QList<std::pair<QTcpSocket*, QTcpSocket*>> m_pairs;
};

std::unique_ptr<Room> joinThrough(const QString& address, const QString& name, const QString& secret,
                                  QString* closedReason = nullptr) {
    JoinOptions options;
    options.profile.name = name;
    options.secret = secret;
    std::unique_ptr<Room> room(Room::join(address, options));
    if (closedReason) {
        QObject::connect(room.get(), &Room::closed, room.get(), [closedReason](const QString& r) { *closedReason = r; });
    }
    waitUntil([&] { return room->state() != Room::State::Connecting; });
    return room;
}

// Joins without the secret, as a member that found the room does.
std::unique_ptr<Room> joinFound(Room& host, const QString& name, QString* closedReason, const QString& password = {},
                                const Identity& identity = {}) {
    JoinOptions options;
    options.profile.name = name;
    options.profile.identity = identity;
    options.password = password;
    options.expectedFingerprint = host.me().fingerprint;
    std::unique_ptr<Room> room(Room::join(address(host), options));
    QObject::connect(room.get(), &Room::closed, room.get(), [closedReason](const QString& r) { *closedReason = r; });
    return room;
}

}  // namespace

USDCC_TEST(peers_identity_and_links) {
    const Identity a = Identity::generate();
    const Identity b = Identity::generate();
    CHECK(a.isValid() && b.isValid() && a.fingerprint() != b.fingerprint(), "two identities are one");
    CHECK(a.fingerprint().size() == 32, "a fingerprint is %lld characters", qlonglong(a.fingerprint().size()));
    const QByteArray signature = a.sign("hello");
    CHECK(Identity::verify(a.publicKey(), "hello", signature), "a signature doesn't verify");
    CHECK(!Identity::verify(b.publicKey(), "hello", signature) && !Identity::verify(a.publicKey(), "hellO", signature),
          "a signature verifies for the wrong key or message");
    const Identity again = Identity::fromSecretKey(a.secretKey());
    CHECK(again.isValid() && again.fingerprint() == a.fingerprint(), "a stored key doesn't come back");
    QByteArray broken = a.secretKey();
    broken[40] = char(broken[40] ^ 1);
    CHECK(!Identity::fromSecretKey(broken).isValid() && !Identity::fromSecretKey("short").isValid(),
          "a damaged key is taken");

    const QString link = makeJoinLink({"192.168.1.20:47800", a.fingerprint(), "s3cr+t/="});
    const std::optional<JoinLink> parsed = parseJoinLink(link);
    CHECK(parsed && parsed->address == "192.168.1.20:47800" && parsed->fingerprint == a.fingerprint() &&
              parsed->secret == "s3cr+t/=",
          "%s didn't come back", qPrintable(link));
    CHECK(!parseJoinLink("http://example.com") && !parseJoinLink("usdcc://join/nohost#k=a"),
          "something else is a link");
}

USDCC_TEST(peers_nothing_readable_on_the_wire) {
    HostOptions options;
    options.profile.name = "Hosting Hilda";
    auto host = hostRoom(options);
    host->channel("chat");
    host->channel("view", Delivery::LatestOnly);
    StateMap* hostNotes = host->state("notes");
    Proxy proxy(host->port());
    auto anna = joinThrough(proxy.address(), "Anna Proxyname", host->secret());
    CHECK(anna->state() == Room::State::Open, "Anna didn't join through the proxy");
    Inbox annaChat, annaView;
    annaChat.listen(anna->channel("chat"));
    annaView.listen(anna->channel("view", Delivery::LatestOnly));
    Inbox hostChat;
    hostChat.listen(host->findChannel("chat"));

    anna->channel("chat")->send("say", {{"text", "the pineapple plan"}});
    host->channel("chat")->send("say", {{"text", "mango reply"}});
    host->findChannel("view")->send("frame", {}, QByteArray("PICTUREBYTES"));
    hostNotes->set("shot_12", "approved-by-hilda");
    StateMap* annaNotes = anna->state("notes");
    CHECK(waitUntil([&] {
              return hostChat.messages.size() == 2 && annaChat.messages.size() == 2 && annaView.messages.size() == 1 &&
                     annaNotes->value("shot_12") == QJsonValue("approved-by-hilda");
          }),
          "the messages didn't arrive");
    CHECK(proxy.captured.contains("challenge") && proxy.captured.size() > 1000, "the proxy saw nothing");
    for (const QByteArray& readable : {QByteArray("pineapple"), QByteArray("mango"), QByteArray("Proxyname"),
                                       QByteArray("Hilda"), QByteArray("PICTUREBYTES"), QByteArray("approved"),
                                       QByteArray("\"chat\""), QByteArray("\"view\""), host->secret().toUtf8()}) {
        CHECK(!proxy.captured.contains(readable), "'%s' is readable on the wire", readable.constData());
    }
    CHECK(anna->hostPeer().fingerprint == host->me().fingerprint && anna->hostPeer().name == "Hosting Hilda",
          "Anna doesn't know who the host is");
}

USDCC_TEST(peers_wrong_key_is_refused) {
    auto host = hostRoom();
    JoinOptions options;
    options.secret = host->secret();
    options.expectedFingerprint = Identity::generate().fingerprint();  // Not the host's
    QString reason;
    std::unique_ptr<Room> room(Room::join(address(*host), options));
    QObject::connect(room.get(), &Room::closed, room.get(), [&](const QString& r) { reason = r; });
    CHECK(waitUntil([&] { return room->state() == Room::State::Closed; }), "joined a host with another key");
    CHECK(reason.contains("Wrong key"), "refused, but for: %s", qPrintable(reason));
    CHECK(host->peers().size() == 1, "the host let it in anyway");

    // The right key goes in.
    options.expectedFingerprint = host->me().fingerprint;
    std::unique_ptr<Room> right(Room::join(address(*host), options));
    CHECK(waitUntil([&] { return right->state() == Room::State::Open; }), "the right key was refused");
}

USDCC_TEST(peers_reconnect_catches_up) {
    auto host = hostRoom();
    Channel* chat = host->channel("chat");
    Proxy proxy(host->port());
    auto anna = joinThrough(proxy.address(), "Anna", host->secret());
    auto ben = joinRoom(*host, "Ben");
    Inbox annaChat, benChat;
    annaChat.listen(anna->channel("chat"));
    benChat.listen(ben->channel("chat"));
    bool resumed = false;
    bool caughtUp = false;
    QObject::connect(anna.get(), &Room::resumed, anna.get(), [&](bool c) {
        resumed = true;
        caughtUp = c;
    });
    int hostHeardLeaving = 0;
    QObject::connect(host.get(), &Room::peerLeft, host.get(), [&] { ++hostHeardLeaving; });

    chat->send("say", {{"n", 1}});
    CHECK(waitUntil([&] { return annaChat.messages.size() == 1; }), "the first message didn't arrive");
    const PeerId annaId = anna->me().id;

    proxy.cut();
    CHECK(waitUntil([&] { return anna->state() == Room::State::Reconnecting; }), "Anna doesn't know she lost the host");
    for (int n = 2; n <= 4; ++n) {
        chat->send("say", {{"n", n}});
    }
    CHECK(waitUntil([&] { return benChat.messages.size() == 4; }), "Ben didn't get the messages");
    CHECK(host->peer(annaId).isValid() && hostHeardLeaving == 0, "Anna was taken out of the room while away");

    proxy.resume();
    CHECK(waitUntil([&] { return resumed; }, 10000), "Anna didn't come back");
    CHECK(caughtUp, "Anna came back too late to catch up");
    CHECK(waitUntil([&] { return annaChat.messages.size() == 4; }), "Anna has %lld messages", qlonglong(annaChat.messages.size()));
    bool inOrder = true;
    for (qsizetype i = 0; i < annaChat.messages.size(); ++i) {
        inOrder = inOrder && annaChat.messages[i].body.value("n").toInt() == i + 1;
    }
    CHECK(inOrder, "Anna's messages came out of order or twice");
    CHECK(anna->me().id == annaId && anna->state() == Room::State::Open, "Anna came back as someone else");

    // Her side of the new connection works too.
    Inbox hostChat;
    hostChat.listen(chat);
    anna->channel("chat")->send("say", {{"n", 5}});
    CHECK(waitUntil([&] { return hostChat.messages.size() == 1 && benChat.messages.size() == 5; }),
          "Anna's message after coming back didn't arrive");
    CHECK(hostHeardLeaving == 0, "the host heard Anna leave");
}

USDCC_TEST(peers_away_too_long_leaves) {
    HostOptions options;
    options.limits.reconnectGraceMs = 300;
    options.limits.pingIntervalMs = 100;
    auto host = hostRoom(options);
    Proxy proxy(host->port());
    auto anna = joinThrough(proxy.address(), "Anna", host->secret());
    QString left;
    QObject::connect(host.get(), &Room::peerLeft, host.get(), [&](const Peer&, const QString& reason) { left = reason; });
    proxy.cut();
    CHECK(waitUntil([&] { return !left.isEmpty(); }, 3000), "a member away too long stays");
    // Back later, it joins again: afresh.
    bool resumed = false;
    bool caughtUp = true;
    QObject::connect(anna.get(), &Room::resumed, anna.get(), [&](bool c) {
        resumed = true;
        caughtUp = c;
    });
    proxy.resume();
    CHECK(waitUntil([&] { return resumed; }, 10000) && !caughtUp, "Anna didn't join again afresh");
}

USDCC_TEST(peers_shared_state) {
    auto host = hostRoom();
    StateMap* hostMap = host->state("com.example.notes");
    auto a = joinRoom(*host, "A");
    auto b = joinRoom(*host, "B");
    StateMap* aMap = a->state("com.example.notes");
    StateMap* bMap = b->state("com.example.notes");

    // Both set one key at once: they end equal, the host's last wins.
    aMap->set("shot", "from A");
    bMap->set("shot", "from B");
    CHECK(aMap->value("shot") == QJsonValue("from A") && bMap->value("shot") == QJsonValue("from B"),
          "a change doesn't apply here at once");
    CHECK(waitUntil([&] {
              return hostMap->contains("shot") && aMap->value("shot") == hostMap->value("shot") &&
                     bMap->value("shot") == hostMap->value("shot");
          }),
          "the map didn't end equal: host %s, A %s, B %s", qPrintable(hostMap->value("shot").toString()),
          qPrintable(aMap->value("shot").toString()), qPrintable(bMap->value("shot").toString()));

    // Random changes from three sides, some at the same time.
    QRandomGenerator random(12);
    StateMap* maps[] = {hostMap, aMap, bMap};
    for (int i = 0; i < 300; ++i) {
        StateMap* map = maps[random.bounded(3)];
        const QString key = QStringLiteral("k%1").arg(random.bounded(5));
        if (random.bounded(4) == 0) {
            map->remove(key);
        } else {
            map->set(key, QJsonObject{{"i", i}});
        }
        if (random.bounded(10) == 0) {
            QCoreApplication::processEvents();
        }
    }
    QTest::qWait(300);
    CHECK(waitUntil([&] { return aMap->items() == hostMap->items() && bMap->items() == hostMap->items(); }),
          "the maps differ after random changes");

    // A late joiner has the map.
    auto c = joinRoom(*host, "C");
    CHECK(waitUntil([&] { return c->state("com.example.notes")->items() == hostMap->items(); }),
          "the late joiner doesn't have the map");

    // The host may say who writes.
    QStringList refused;
    QObject::connect(aMap, &StateMap::refused, aMap, [&](const QString& key, const QString&) { refused.append(key); });
    hostMap->setSenders(Senders::Host);
    aMap->set("locked", 1);
    CHECK(waitUntil([&] { return !refused.isEmpty(); }), "a change the host doesn't allow went through");
    CHECK(!aMap->contains("locked") && !hostMap->contains("locked"), "the refused change stayed");
    hostMap->setSenders(Senders::Everyone);
    aMap->set("big", QString(300 * 1024, 'x'));
    CHECK(waitUntil([&] { return refused.contains("big"); }), "a value over 256 KB was taken");

    // Who changed what, as the others hear it.
    QString by;
    QObject::connect(bMap, &StateMap::changed, bMap, [&](const QString& key, const QJsonValue&, const Peer& peer) {
        if (key == "who") {
            by = peer.name;
        }
    });
    aMap->set("who", true);
    CHECK(waitUntil([&] { return by == "A"; }), "B heard the change came from '%s'", qPrintable(by));
}

USDCC_TEST(peers_peer_state_and_snapshots) {
    auto host = hostRoom();
    Channel* chat = host->channel("chat");
    QJsonArray history;
    chat->setSnapshotProvider([&history] { return Channel::Snapshot{{{"history", history}}, {}}; });
    QObject::connect(chat, &Channel::received, chat, [&history](const Message& m) { history.append(m.body); });
    auto a = joinRoom(*host, "A");
    auto b = joinRoom(*host, "B");
    PeerState* aPresence = a->peerState("presence");
    PeerState* bPresence = b->peerState("presence");
    aPresence->set(QJsonObject{{"stage", "shot_12.usda"}});
    CHECK(aPresence->value(a->me().id) == QJsonValue(QJsonObject{{"stage", "shot_12.usda"}}), "one's own presence isn't set at once");
    CHECK(waitUntil([&] { return bPresence->value(a->me().id).toObject().value("stage") == "shot_12.usda"; }),
          "B doesn't see A's presence");
    CHECK(waitUntil([&] { return host->peerState("presence")->value(a->me().id).isObject(); }),
          "the host doesn't see A's presence");
    a->channel("chat")->send("say", {{"text", "first"}});
    a->channel("chat")->send("say", {{"text", "second"}});
    CHECK(waitUntil([&] { return history.size() == 2; }), "the host's chat didn't get them");

    // A late joiner gets the presence there is, and the chat's history as its snapshot.
    auto c = joinRoom(*host, "C");
    Inbox cChat;
    cChat.listen(c->channel("chat"));
    PeerState* cPresence = c->peerState("presence");
    CHECK(waitUntil([&] { return cPresence->value(a->me().id).isObject(); }), "the late joiner doesn't see A's presence");
    // The snapshot comes with the welcome: a member that opens the channel before then gets it.
    JoinOptions dOptions;
    dOptions.profile.name = "D";
    dOptions.secret = host->secret();
    std::unique_ptr<Room> d(Room::join(address(*host), dOptions));
    Inbox dChat;
    dChat.listen(d->channel("chat"));
    CHECK(waitUntil([&] { return !dChat.messages.isEmpty(); }), "no snapshot on the chat channel");
    if (!dChat.messages.isEmpty()) {
        const Message& first = dChat.messages.first();
        CHECK(first.type == "snapshot" && first.body.value("history").toArray().size() == 2 && first.seq == 0,
              "the snapshot is a %s with %lld lines", qPrintable(first.type),
              qlonglong(first.body.value("history").toArray().size()));
    }

    // Gone with the peer.
    bool cleared = false;
    QObject::connect(bPresence, &PeerState::changed, bPresence, [&](const Peer& peer, const QJsonValue& value) {
        if (peer.name == "A" && value.isUndefined()) {
            cleared = true;
        }
    });
    a->close();
    CHECK(waitUntil([&] { return cleared && bPresence->value(a->me().id).isUndefined(); }), "A's presence outlived A");
}

USDCC_TEST(peers_admission) {
    // Ask: the host decides.
    {
        HostOptions options;
        options.admission = Admission::Ask;
        auto host = hostRoom(options);
        QList<JoinRequest*> requests;
        QObject::connect(host.get(), &Room::joinRequested, host.get(), [&](JoinRequest* r) { requests.append(r); });
        QString annaReason, benReason;
        auto anna = joinFound(*host, "Anna", &annaReason);
        CHECK(waitUntil([&] { return requests.size() == 1; }), "the host wasn't asked");
        CHECK(anna->state() == Room::State::Connecting && requests.value(0)->peer().name == "Anna",
              "Anna is in before the host answered");
        requests.takeFirst()->allow();
        CHECK(waitUntil([&] { return anna->state() == Room::State::Open; }), "allowed, but Anna isn't in");

        const Identity benKey = Identity::generate();
        auto ben = joinFound(*host, "Ben", &benReason, {}, benKey);
        CHECK(waitUntil([&] { return requests.size() == 1; }), "the host wasn't asked about Ben");
        requests.takeFirst()->deny();
        CHECK(waitUntil([&] { return ben->state() == Room::State::Closed; }) && benReason.contains("said no"),
              "denied, but Ben: %s", qPrintable(benReason));

        // Always allow Ben: not asked again.
        host->trust(benKey.fingerprint());
        auto benAgain = joinFound(*host, "Ben", &benReason, {}, benKey);
        CHECK(waitUntil([&] { return benAgain->state() == Room::State::Open; }) && requests.isEmpty(),
              "a trusted peer was asked about, or kept out");

        // The secret goes in without asking.
        auto carl = joinRoom(*host, "Carl");
        CHECK(carl->state() == Room::State::Open && requests.isEmpty(), "a member with the link was asked about");

        // Gone while asking: the request is cancelled.
        QString danReason;
        auto dan = joinFound(*host, "Dan", &danReason);
        CHECK(waitUntil([&] { return requests.size() == 1; }), "the host wasn't asked about Dan");
        bool cancelled = false;
        QObject::connect(requests.first(), &JoinRequest::cancelled, host.get(), [&] { cancelled = true; });
        dan.reset();
        CHECK(waitUntil([&] { return cancelled; }), "the request outlived Dan");
    }
    // Open: a public room, anyone who finds it.
    {
        HostOptions options;
        options.admission = Admission::Open;
        auto host = hostRoom(options);
        int asked = 0;
        QObject::connect(host.get(), &Room::joinRequested, host.get(), [&] { ++asked; });
        QString reason;
        auto anna = joinFound(*host, "Anna", &reason);
        CHECK(waitUntil([&] { return anna->state() == Room::State::Open; }) && asked == 0,
              "a public room kept Anna out or asked: %s", qPrintable(reason));
    }
    // Invited: only with the link.
    {
        auto host = hostRoom();
        QString reason;
        auto anna = joinFound(*host, "Anna", &reason);
        CHECK(waitUntil([&] { return anna->state() == Room::State::Closed; }) && reason.contains("invited"),
              "an invited room let in someone without the link: %s", qPrintable(reason));
    }
    // A password, for those without the link.
    {
        HostOptions options;
        options.admission = Admission::Open;
        options.password = "swordfish";
        auto host = hostRoom(options);
        QString wrong, right;
        auto anna = joinFound(*host, "Anna", &wrong, "tuna");
        CHECK(waitUntil([&] { return anna->state() == Room::State::Closed; }) && wrong.contains("password"),
              "a wrong password went in: %s", qPrintable(wrong));
        auto ben = joinFound(*host, "Ben", &right, "swordfish");
        CHECK(waitUntil([&] { return ben->state() == Room::State::Open; }), "the right password was refused: %s",
              qPrintable(right));
    }
}

USDCC_TEST(peers_room_key_changes_when_someone_leaves) {
    auto host = hostRoom();
    Channel* chat = host->channel("chat");
    auto a = joinRoom(*host, "A");
    auto b = joinRoom(*host, "B");
    Inbox aChat;
    aChat.listen(a->channel("chat"));
    chat->send("say", {{"n", 1}});
    CHECK(waitUntil([&] { return aChat.messages.size() == 1; }), "the first message didn't arrive");
    host->remove(b->me().id);
    CHECK(waitUntil([&] { return b->state() == Room::State::Closed; }), "B is still in");
    // A can still read what comes under the new key.
    chat->send("say", {{"n", 2}});
    chat->send("say", {{"n", 3}});
    CHECK(waitUntil([&] { return aChat.messages.size() == 3; }), "A can't read the room after the key changed");
}
