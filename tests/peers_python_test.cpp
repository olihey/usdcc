// The app's usdcc_peers against the Python package's (docs/PEERS.md §9, §15): the two
// implementations of one protocol, kept equal by talking to each other. The fixture starts
// the Python hub on free ports, as the HTTP tests start their server; C++ members join it,
// get a notification posted over HTTP and see a map the hub set, and one comes back after a
// cut connection. Then a Python member joins a room the app hosts. Runs in usdcc_peers_tests,
// with build/peers-venv's Python (tests/CMakeLists.txt makes it).
#include "peers_test_support.h"

#include "usdcc/peers/identity.h"
#include "usdcc/peers/state.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

using namespace usdcc::peers;
using namespace usdcc::peers::test;

namespace {

QProcessEnvironment pythonEnvironment() {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONPATH"), QStringLiteral(USDCC_SOURCE_DIR "/src/usdcc_peers"));
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    return environment;
}

// The hub, started for one test and stopped at its end.
struct PythonHub {
    QTemporaryDir folder;
    QProcess process;
    QString link;
    QString fingerprint;
    int http = 0;
    QString token = QStringLiteral("hub-test-token");

    bool start() {
        QFile tokenFile(folder.filePath(QStringLiteral("hub.token")));
        if (!tokenFile.open(QIODevice::WriteOnly)) {
            return false;
        }
        tokenFile.write(token.toUtf8());
        tokenFile.close();
        process.setProcessEnvironment(pythonEnvironment());
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start(QStringLiteral(USDCC_PEERS_PYTHON),
                      {QStringLiteral("-m"), QStringLiteral("usdcc_peers.hub"), QStringLiteral("--name"),
                       QStringLiteral("Studio"), QStringLiteral("--port"), QStringLiteral("0"), QStringLiteral("--http"),
                       QStringLiteral("0"), QStringLiteral("--address"), QStringLiteral("127.0.0.1"),
                       QStringLiteral("--token-file"), tokenFile.fileName(), QStringLiteral("--data"),
                       folder.filePath(QStringLiteral("data")), QStringLiteral("--print-links"), QStringLiteral("--stop-on-eof"),
                       QStringLiteral("--log-level"), QStringLiteral("warning")});
        if (!waitUntil([this] { return process.canReadLine() || process.state() == QProcess::NotRunning; }, 20000) ||
            !process.canReadLine()) {
            std::printf("    the hub didn't start: %s\n", process.readAllStandardError().constData());
            return false;
        }
        const QJsonObject started = QJsonDocument::fromJson(process.readLine()).object();
        link = started.value("rooms").toArray().at(0).toObject().value("link").toString();
        fingerprint = started.value("fingerprint").toString();
        http = started.value("http").toInt();
        return !link.isEmpty() && http > 0;
    }

    // POST to the hub's HTTP side; the status code, or 0 if it didn't answer.
    int post(const QString& path, const QJsonObject& body, const QString& withToken = {}) {
        QNetworkAccessManager network;
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(http).arg(path)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Authorization", "Bearer " + (withToken.isEmpty() ? token : withToken).toUtf8());
        QNetworkReply* reply = network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        waitUntil([reply] { return reply->isFinished(); });
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        return status;
    }

    // Stops it as a program that runs it would: on Windows terminate() doesn't reach a
    // console program, so the hub stops when its standard input closes.
    void stop() { process.closeWriteChannel(); }

    ~PythonHub() {
        if (process.state() != QProcess::NotRunning) {
            stop();
            process.waitForFinished(5000);
        }
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(5000)) {
                process.kill();
                process.waitForFinished(2000);
            }
        }
    }
};

std::unique_ptr<Room> joinLink(const QString& text, const QString& name, const QString& viaAddress = {}) {
    const std::optional<JoinLink> link = parseJoinLink(text);
    CHECK(link.has_value(), "%s isn't a link", qPrintable(text));
    if (!link) {
        return {};
    }
    JoinOptions options;
    options.profile.name = name;
    options.secret = link->secret;
    options.expectedFingerprint = link->fingerprint;
    std::unique_ptr<Room> room(Room::join(viaAddress.isEmpty() ? link->address : viaAddress, options));
    waitUntil([&] { return room->state() != Room::State::Connecting; }, 10000);
    return room;
}

}  // namespace

USDCC_TEST(peers_python_hub_with_cpp_members) {
    PythonHub hub;
    if (!hub.start()) {
        CHECK(false, "the Python hub didn't start (is build/peers-venv there?)");
        return;
    }
    auto anna = joinLink(hub.link, "Anna");
    auto ben = joinLink(hub.link, "Ben");
    CHECK(anna && anna->state() == Room::State::Open && ben && ben->state() == Room::State::Open,
          "C++ members didn't join the Python hub");
    if (!anna || anna->state() != Room::State::Open || !ben || ben->state() != Room::State::Open) {
        return;
    }
    CHECK(anna->hostPeer().fingerprint == hub.fingerprint && anna->name() == "Studio",
          "the hub isn't the host: %s", qPrintable(anna->hostPeer().fingerprint));
    CHECK(waitUntil([&] { return anna->peers().size() == 3; }), "Anna sees %lld in the room",
          qlonglong(anna->peers().size()));

    // Chat between two C++ members, through the Python host's ordering and room key.
    Inbox benChat, annaNotify, benNotify;
    benChat.listen(ben->channel("chat"));
    annaNotify.listen(anna->channel("notify"));
    benNotify.listen(ben->channel("notify"));
    anna->channel("chat")->send("say", {{"text", "hello through Python"}});
    CHECK(waitUntil([&] { return benChat.messages.size() == 1; }), "Ben didn't hear Anna");
    if (!benChat.messages.isEmpty()) {
        const Message& said = benChat.messages.first();
        CHECK(said.from == anna->me().id && said.body.value("text") == "hello through Python" && said.seq > 0,
              "Ben heard %s from %s", qPrintable(said.body.value("text").toString()), qPrintable(said.from));
    }

    // A webhook's notification, to one person by name, and one to everyone.
    CHECK(hub.post("/notify", {{"title", "x"}}, "wrong") == 401, "the hub took a wrong token");
    CHECK(hub.post("/notify", {{"title", "Render finished"}, {"kind", "success"}, {"id", "farm/8812"}, {"to", "ben"}}) ==
              200,
          "the hub didn't take a notification");
    CHECK(hub.post("/notify", {{"title", "Lunch"}}) == 200, "the hub didn't take a notification to everyone");
    CHECK(waitUntil([&] { return benNotify.messages.size() == 2 && annaNotify.messages.size() == 1; }),
          "notifications: Ben has %lld, Anna %lld", qlonglong(benNotify.messages.size()),
          qlonglong(annaNotify.messages.size()));
    if (benNotify.messages.size() == 2) {
        const Message& first = benNotify.messages.first();
        CHECK(first.type == "notify" && first.body.value("title") == "Render finished" &&
                  first.body.value("id") == "farm/8812" && first.from == anna->hostPeer().id,
              "Ben's notification: %s", QJsonDocument(first.body).toJson(QJsonDocument::Compact).constData());
    }

    // A member may read notify but not send on it.
    anna->channel("notify")->send("notify", {{"title", "Spam"}, {"kind", "info"}});
    CHECK(waitUntil([&] { return !annaNotify.refusals.isEmpty(); }), "the hub let Anna send a notification");

    // A map the hub sets, seen by a member; and one a member sets, seen by the other.
    CHECK(hub.post("/state", {{"map", "farm.jobs"}, {"key", "8812"}, {"value", QJsonObject{{"progress", 1.0}}}}) == 200,
          "the hub didn't set a map");
    StateMap* jobs = ben->state("farm.jobs");
    anna->state("notes")->set("/World/hero", "too shiny");
    CHECK(waitUntil([&] {
              return jobs->value("8812").toObject().value("progress").toDouble() == 1.0 &&
                     ben->state("notes")->value("/World/hero") == "too shiny";
          }),
          "Ben's maps: %s", QJsonDocument(ben->state("notes")->items()).toJson(QJsonDocument::Compact).constData());

    // Through a proxy that cuts the connection: back, with what it missed.
    const std::optional<JoinLink> link = parseJoinLink(hub.link);
    Proxy proxy(quint16(link->address.section(':', -1).toUShort()));
    auto cara = joinLink(hub.link, "Cara", proxy.address());
    CHECK(cara && cara->state() == Room::State::Open, "Cara didn't join through the proxy");
    if (!cara || cara->state() != Room::State::Open) {
        return;
    }
    Inbox caraChat;
    caraChat.listen(cara->channel("chat"));
    bool caughtUp = false;
    bool resumed = false;
    QObject::connect(cara.get(), &Room::resumed, cara.get(), [&](bool c) {
        caughtUp = c;
        resumed = true;
    });
    proxy.cut();
    CHECK(waitUntil([&] { return cara->state() == Room::State::Reconnecting; }), "Cara didn't notice the cut");
    anna->channel("chat")->send("say", {{"text", "while Cara was away"}});
    waitUntil([&] { return benChat.messages.size() == 2; });
    proxy.resume();
    CHECK(waitUntil([&] { return resumed && caraChat.messages.size() == 1; }, 15000),
          "Cara: resumed %d, %lld messages", int(resumed), qlonglong(caraChat.messages.size()));
    CHECK(caughtUp, "Cara wasn't caught up");

    // The hub stopping closes the room for its members.
    QString closedReason;
    QObject::connect(anna.get(), &Room::closed, anna.get(), [&](const QString& reason) { closedReason = reason; });
    hub.stop();
    CHECK(waitUntil([&] { return !closedReason.isEmpty(); }, 10000) && closedReason == "The hub stopped",
          "Anna's room closed with \"%s\"", qPrintable(closedReason));
}

USDCC_TEST(peers_python_member_in_cpp_room) {
    HostOptions options;
    options.name = QStringLiteral("App Room");
    options.profile.name = QStringLiteral("Hosting Hilda");
    auto host = hostRoom(options);
    Inbox chat;
    chat.listen(host->channel("chat"));
    host->channel("notify")->setNamedSenders({});
    StateMap* jobs = host->state("farm.jobs");

    // A script, as a studio's tool would be: joins, chats, sets a map, tries to notify,
    // waits for the host's answer and prints what it saw.
    const QString script = QStringLiteral(R"(
import json, sys, threading, usdcc_peers
room = usdcc_peers.join(sys.argv[1], name="Render Farm", key_file=sys.argv[2], app="farm 1.0")
answers, refused = threading.Event(), []
room.channel("chat").on_message(lambda m: m.body.get("text") == "hello Python" and answers.set())
room.channel("notify").on_refused(lambda t, r: refused.append(r))
room.channel("chat").send("say", {"text": "hello from Python"})
room.state("farm.jobs").set("8812", {"progress": 0.5})
room.notify("Not mine to send")
answers.wait(10)
print(json.dumps({"answered": answers.is_set(), "refused": refused, "host": room.host.name,
                  "peers": sorted(p.name for p in room.peers())}), flush=True)
room.close()
)");
    QTemporaryDir folder;
    QProcess python;
    python.setProcessEnvironment(pythonEnvironment());
    python.start(QStringLiteral(USDCC_PEERS_PYTHON),
                 {QStringLiteral("-c"), script, host->link(), folder.filePath(QStringLiteral("farm.key"))});
    CHECK(waitUntil([&] { return chat.messages.size() == 1 || python.state() == QProcess::NotRunning; }, 20000) &&
              chat.messages.size() == 1,
          "the host didn't hear Python: %s", python.readAllStandardError().constData());
    if (chat.messages.isEmpty()) {
        return;
    }
    const Message said = chat.messages.first();
    const Peer farm = host->peer(said.from);
    CHECK(said.body.value("text") == "hello from Python" && farm.name == "Render Farm" && farm.app == "farm 1.0",
          "the host heard %s from %s", qPrintable(said.body.value("text").toString()), qPrintable(farm.name));
    CHECK(waitUntil([&] { return jobs->value("8812").toObject().value("progress").toDouble() == 0.5; }),
          "Python's change to the map didn't arrive");
    host->channel("chat")->send("say", {{"text", "hello Python"}});
    // Waited for with the event loop running: the host answers from it.
    CHECK(waitUntil([&] { return python.state() == QProcess::NotRunning; }, 15000) && python.exitCode() == 0,
          "Python ended with %d: %s", python.exitCode(),
          python.readAllStandardError().constData());
    const QJsonObject saw = QJsonDocument::fromJson(python.readAllStandardOutput().trimmed()).object();
    CHECK(saw.value("answered").toBool() && saw.value("host").toString() == "Hosting Hilda" &&
              saw.value("refused").toArray().size() == 1 &&
              saw.value("peers").toArray() == QJsonArray({"Hosting Hilda", "Render Farm"}),
          "Python saw %s", QJsonDocument(saw).toJson(QJsonDocument::Compact).constData());
    CHECK(waitUntil([&] { return host->peers().size() == 1; }), "Python didn't leave");
}
