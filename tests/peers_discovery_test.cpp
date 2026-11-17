// usdcc_peers, finding rooms (docs/PEERS.md §5): this machine's folder of room files,
// and the network through a fake that stands for it (multicast isn't dependable in CI,
// §15). The real mDNS runs only when USDCC_PEERS_MDNS_TEST=1, as it sends on the
// machine's network. Runs in usdcc_peers_tests.
#include "peers_test_support.h"

#include "usdcc/peers/peers.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace usdcc::peers;
using namespace usdcc::peers::test;

namespace {

std::unique_ptr<Peers> makePeers(const QString& name, const QString& folder, const QString& network = {}) {
    Peers::Options options;
    options.profile.name = name;
    options.roomsFolder = folder;
    if (!network.isEmpty()) {
        options.network = std::make_unique<FakeNetworkDiscovery>(network);
        options.announceLoopback = true;
    }
    return std::make_unique<Peers>(std::move(options));
}

const FoundRoom* findNamed(const QList<FoundRoom>& rooms, const QString& name) {
    for (const FoundRoom& room : rooms) {
        if (room.announcement.name == name) {
            return &room;
        }
    }
    return nullptr;
}

}  // namespace

USDCC_TEST(peers_rooms_on_this_machine) {
    QTemporaryDir folder;
    auto anna = makePeers("Anna", folder.path());
    auto ben = makePeers("Ben", folder.path());

    HostOptions options;
    options.name = "Shot 12";
    options.admission = Admission::Ask;
    Room* room = anna->host(options);
    CHECK(room, "Anna couldn't host");
    QList<JoinRequest*> requests;
    QObject::connect(room, &Room::joinRequested, room, [&](JoinRequest* r) { requests.append(r); });
    HostOptions invited;
    invited.name = "Private";
    anna->host(invited);

    CHECK(waitUntil([&] { return findNamed(ben->foundRooms(), "Shot 12"); }), "Ben doesn't find Anna's room");
    const QList<FoundRoom> found = ben->foundRooms();
    CHECK(!findNamed(found, "Private"), "an invited room is listed");
    CHECK(anna->foundRooms().isEmpty(), "Anna finds her own room");
    const FoundRoom* shot = findNamed(found, "Shot 12");
    if (!shot) {
        return;
    }
    CHECK(shot->where == FoundRoom::Where::ThisMachine && shot->announcement.hostName == "Anna" &&
              shot->announcement.admission == Admission::Ask && shot->announcement.fingerprint == room->me().fingerprint,
          "the room is listed wrong");

    Room* joined = ben->join(*shot);
    CHECK(waitUntil([&] { return requests.size() == 1; }), "Anna wasn't asked");
    if (!requests.isEmpty()) {
        requests.first()->allow();
    }
    CHECK(waitUntil([&] { return joined->state() == Room::State::Open; }), "Ben isn't in");
    CHECK(ben->rooms().size() == 1 && anna->rooms().size() == 2, "the managers count the rooms wrong");
    CHECK(ben->foundRooms().isEmpty(), "a room Ben is in is still listed as found");

    // Closed: the file goes, and so does the room from the list.
    room->close();
    CHECK(waitUntil([&] { return !QDir(folder.path()).entryList({"*.json"}, QDir::Files).size(); }),
          "the room's file stayed");
    CHECK(waitUntil([&] { return ben->rooms().isEmpty() && ben->foundRooms().isEmpty(); }), "Ben still has the room");

    // A file left behind by a program that ended: nothing answers its port.
    QFile stale(QDir(folder.path()).filePath("stale-abc.json"));
    CHECK(stale.open(QIODevice::WriteOnly), "couldn't write a stale file");
    stale.write(R"({"protocol":1,"fingerprint":"stale","instance":"abc","name":"Gone","host":"Ghost","port":9,"admission":"open"})");
    stale.close();
    ben->directory()->rescan();
    CHECK(waitUntil([&] { return !QFile::exists(stale.fileName()); }), "a stale room file stayed");
}

USDCC_TEST(peers_public_rooms_on_the_network) {
    QTemporaryDir annasMachine, bensMachine;
    auto anna = makePeers("Anna", annasMachine.path(), "studio");
    auto ben = makePeers("Ben", bensMachine.path(), "studio");
    auto other = makePeers("Other", QString(), "another network");

    HostOptions options;
    options.name = "Dailies";
    options.admission = Admission::Open;
    Room* room = anna->host(options);
    CHECK(room, "Anna couldn't host");
    int asked = 0;
    QObject::connect(room, &Room::joinRequested, room, [&] { ++asked; });

    // Nothing is asked of the network until someone looks.
    QTest::qWait(100);
    CHECK(ben->foundRooms().isEmpty(), "Ben found a room without looking");
    ben->setLooking(true);
    other->setLooking(true);
    CHECK(waitUntil([&] { return findNamed(ben->foundRooms(), "Dailies"); }), "Ben doesn't find the public room");
    QTest::qWait(50);
    CHECK(other->foundRooms().isEmpty(), "another network sees the room");
    const FoundRoom* dailies = findNamed(ben->foundRooms(), "Dailies");
    if (!dailies) {
        return;
    }
    CHECK(dailies->where == FoundRoom::Where::Network && dailies->announcement.admission == Admission::Open,
          "the room is listed wrong");

    // Public: in without asking.
    Room* joined = ben->join(*dailies);
    CHECK(waitUntil([&] { return joined->state() == Room::State::Open; }) && asked == 0,
          "Ben wasn't let straight in");
    Channel* annaChat = room->channel("chat");
    Inbox benChat;
    benChat.listen(joined->channel("chat"));
    annaChat->send("say", {{"text", "hello network"}});
    CHECK(waitUntil([&] { return benChat.messages.size() == 1; }), "the chat didn't arrive");

    // Several rooms at once: Ben hosts one of his own while in Anna's.
    HostOptions bens;
    bens.name = "Ben's review";
    bens.admission = Admission::Ask;
    Room* review = ben->host(bens);
    anna->setLooking(true);
    CHECK(review && waitUntil([&] { return findNamed(anna->foundRooms(), "Ben's review"); }),
          "Anna doesn't find Ben's room");
    CHECK(ben->rooms().size() == 2, "Ben isn't in two rooms");

    room->close();
    CHECK(waitUntil([&] { return !findNamed(ben->foundRooms(), "Dailies") && ben->rooms().size() == 1; }),
          "the closed room is still listed");
    ben->setLooking(false);
    CHECK(ben->foundRooms().isEmpty(), "rooms stay listed after looking stopped");
}

USDCC_TEST(peers_mdns) {
    if (qEnvironmentVariable("USDCC_PEERS_MDNS_TEST") != "1") {
        std::printf("  (skipped: set USDCC_PEERS_MDNS_TEST=1 to send mDNS on this machine's network)\n");
        return;
    }
    std::unique_ptr<NetworkDiscovery> host = createMdnsDiscovery();
    std::unique_ptr<NetworkDiscovery> looker = createMdnsDiscovery();
    Announcement announcement;
    announcement.fingerprint = Identity::generate().fingerprint();
    announcement.instance = randomId(6);
    announcement.name = QStringLiteral("mDNS test room ✓");
    announcement.hostName = "Tester";
    announcement.port = 47999;
    announcement.admission = Admission::Open;
    announcement.password = true;
    host->announce(announcement);
    CHECK(host->problem().isEmpty(), "announcing failed: %s", qPrintable(host->problem()));
    QList<FoundRoom> found;
    QObject::connect(looker.get(), &NetworkDiscovery::found, looker.get(), [&](const FoundRoom& room) {
        if (room.announcement.fingerprint == announcement.fingerprint) {
            found.append(room);
        }
    });
    looker->setLooking(true);
    CHECK(waitUntil([&] { return !found.isEmpty(); }, 10000), "the room wasn't found over mDNS");
    if (!found.isEmpty()) {
        const FoundRoom& room = found.last();
        CHECK(room.announcement.name == announcement.name && room.announcement.hostName == "Tester" &&
                  room.announcement.instance == announcement.instance && room.announcement.admission == Admission::Open &&
                  room.announcement.password && room.address.endsWith(":47999"),
              "found as %s at %s", qPrintable(room.announcement.name), qPrintable(room.address));
        std::printf("  found at %s\n", qPrintable(room.address));
    }
    bool lost = false;
    QObject::connect(looker.get(), &NetworkDiscovery::lost, looker.get(), [&](const QString& id) {
        lost = lost || id == announcement.hostId();
    });
    host->withdraw(announcement.instance);
    CHECK(waitUntil([&] { return lost; }, 15000), "the withdrawn room stayed found");
}
