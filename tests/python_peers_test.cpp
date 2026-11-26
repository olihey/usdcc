// usdcc.peers (docs/PEERS.md §8.2): a script in the app hosts a room, and a member in C++
// (another usdcc, as far as the room can tell) joins it. Messages, maps, per-peer state and
// notifications both ways; a connection that is disconnected hears nothing more; and a
// closed room's calls raise. Runs in usdcc_python_tests.
#include "python_test_app.h"
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/peers/channel.h"
#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/peers/state.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/ui/peers_controller.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace usdcc::peers;
using usdcc::test::pythonTestApp;
using usdcc::ui::PeersController;

namespace {

std::unique_ptr<PeersController> controllerFor(const QString& name, const QString& folder) {
    Peers::Options options;
    options.profile.name = name;
    options.roomsFolder = folder;
    return std::make_unique<PeersController>(std::make_unique<Peers>(std::move(options)), nullptr,
                                             PeersController::Hooks{});
}

// In one namespace for the whole test: each call sees what the ones before made.
bool python(const std::string& code) {
    return pythonTestApp().python.runString(code, "<peers test>", usdcc::scripting::PythonRuntime::Namespace::Session);
}

// Waits, with the event loop running, until the Python expression is true.
bool pythonUntil(const std::string& expression, int milliseconds = 5000) {
    return QTest::qWaitFor([&] { return python("assert " + expression + "\n"); }, milliseconds);
}

}  // namespace

USDCC_TEST(python_peers_rooms_from_a_script) {
    auto& app = pythonTestApp();
    TestSettings settings;
    PeersController::defineSettings();
    QTemporaryDir folder;
    auto anna = controllerFor(QStringLiteral("Anna"), folder.path());
    auto ben = controllerFor(QStringLiteral("Ben"), folder.path());
    const usdcc::scripting::AppContextScope scope({&app.stages, &app.undo, &app.tools});
    usdcc::scripting::setPeersController(anna.get());

    CHECK(python("import usdcc.peers as peers\n"
                 "joined_events = []\n"
                 "peers.connect('room_joined', lambda room: joined_events.append(room.name))\n"
                 "room = peers.host('Notes', admission='invited', on_network=False)\n"
                 "assert room.is_host and room.link.startswith('usdcc://join/'), room.link\n"
                 "assert peers.me()['name'] == 'Anna', peers.me()\n"
                 "heard, notes, presence, came = [], [], [], []\n"
                 "chat = room.channel('notes.ping')\n"
                 "listening = chat.on_message(lambda m: heard.append((m.sender.name, m.type, m.body, m.data)))\n"
                 "room.state('notes.items').on_change(lambda key, value, by: notes.append((key, value, by.name)))\n"
                 "room.peer_state('notes.where').on_change(lambda peer, value: presence.append((peer.name, value)))\n"
                 "room.on('peer_joined', lambda peer: came.append(peer.name))\n"),
          "a script couldn't host a room");
    Room* annaRoom = anna->peers()->rooms().value(0);
    CHECK(annaRoom && pythonUntil("joined_events == ['Notes']"), "room_joined didn't come");
    if (!annaRoom) return;

    Room* benRoom = ben->joinLinkOrAddress(annaRoom->link(), {}, nullptr);
    CHECK(benRoom && QTest::qWaitFor([&] { return benRoom->state() == Room::State::Open; }, 5000), "Ben didn't join");
    if (!benRoom) return;
    CHECK(pythonUntil("came == ['Ben'] and [p.name for p in room.peers()] == ['Anna', 'Ben']"),
          "the script doesn't see Ben");

    // Ben, in C++, to the script.
    benRoom->channel("notes.ping")->send("added", {{"text", "New note on /World/hero"}}, QByteArray("\x01\x02", 2));
    benRoom->state("notes.items")->set("/World/hero", QJsonObject{{"text", "too shiny"}});
    benRoom->peerState("notes.where")->set("/World/hero");
    CHECK(pythonUntil("heard == [('Ben', 'added', {'text': 'New note on /World/hero'}, b'\\x01\\x02')]"),
          "the script didn't hear Ben");
    CHECK(pythonUntil("notes == [('/World/hero', {'text': 'too shiny'}, 'Ben')]"), "the script's map didn't change");
    CHECK(pythonUntil("presence == [('Ben', '/World/hero')]"), "the script didn't see Ben's per-peer state");
    CHECK(python("assert room.state('notes.items').get('/World/hero') == {'text': 'too shiny'}\n"
                 "assert room.peer_state('notes.where').values() == {room.find('ben')[0].id: '/World/hero'}\n"),
          "reading the maps from the script");

    // The script to Ben: a message, a map, a private message and a notification.
    QList<Message> benMessages;
    QObject context;
    QObject::connect(benRoom->channel("notes.ping"), &Channel::received, &context,
                     [&](const Message& m) { benMessages.append(m); });
    CHECK(python("chat.send('reply', {'n': 1})\n"
                 "room.state('notes.items').set('/World/lamp', [1, 'two', None])\n"
                 "room.state('notes.items').remove('/World/hero')\n"
                 "assert chat.send_private('secret', {'x': 1}, room.find('Ben')[0])\n"
                 "room.notify('Note added', 'On /World/lamp', kind='success', to='ben', id='notes/1',\n"
                 "            actions=[{'label': 'Select', 'select': ['/World/lamp']}])\n"),
          "the script couldn't send");
    CHECK(QTest::qWaitFor([&] { return benMessages.size() >= 3; }, 5000), "Ben has %lld messages",
          qlonglong(benMessages.size()));
    if (benMessages.size() >= 3) {
        // Ordered messages come back to their sender too: Ben's own "added" first.
        const Message& reply = benMessages[1];
        const Message& secret = benMessages[2];
        CHECK(reply.type == "reply" && reply.body.value("n").toInt() == 1 && reply.from == annaRoom->me().id,
              "Ben's reply: %s", qPrintable(reply.type));
        CHECK(secret.type == "secret" && secret.sealed, "the private message wasn't sealed");
    }
    StateMap* items = benRoom->state("notes.items");
    CHECK(QTest::qWaitFor([&] { return !items->contains("/World/hero") && items->value("/World/lamp").isArray(); }),
          "Ben's map: %s", QJsonDocument(items->items()).toJson(QJsonDocument::Compact).constData());
    CHECK(QTest::qWaitFor([&] { return ben->notifications(benRoom).size() == 1; }), "Ben has no notification");
    if (!ben->notifications(benRoom).isEmpty()) {
        const PeersController::Notification n = *ben->notifications(benRoom).first().notification;
        CHECK(n.title == "Note added" && n.id == "notes/1" && n.actions.size() == 1 &&
                  n.actions.first().paths == QStringList{"/World/lamp"},
              "Ben's notification: %s", qPrintable(n.title));
    }
    CHECK(!python("room.notify('x', actions=[{'label': 'Run', 'run': 'rm -rf /'}])\n"),
          "a notification with an action not on the list was sent");
    CHECK(!python("room.notify('x', to='nobody')\n"), "a notification to nobody didn't raise");

    // Disconnected: nothing more arrives on it.
    python("listening.disconnect()\n");
    benRoom->channel("notes.ping")->send("after", {});
    QTest::qWait(300);
    CHECK(python("assert 'after' not in [h[1] for h in heard], heard\n"), "a disconnected callback still ran");

    // Closed: the room goes, and its calls raise.
    CHECK(python("left = []\npeers.connect('room_left', lambda r: left.append(r))\nroom.close()\n"), "close()");
    CHECK(pythonUntil("len(left) == 1 and peers.rooms() == []"), "room_left didn't come");
    CHECK(!python("room.channel('notes.ping').send('x')\n"), "a closed room's send didn't raise");
    usdcc::scripting::setPeersController(nullptr);
    CHECK(!python("peers.rooms()\n"), "usdcc.peers without the app didn't raise");
}
