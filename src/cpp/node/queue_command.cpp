// usdcc_node queue (docs/RENDER_NODE.md §4.4, §4.5): a render queue server. It hosts a
// render queue room or joins one, takes artists' jobs (RenderQueueServer), hands their tasks
// to the render nodes there through its own RenderNetwork, writes the frames, keeps what it
// has in --state so that it goes on after a restart, and serves a web page with --web.

#include "node.h"
#include "queue_web.h"
#include "serve_support.h"

#include "startup.h"
#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/render/render_network.h"
#include "usdcc/usd/render/render_queue.h"
#include "usdcc/usd/render/render_scheduler.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>

namespace usdcc::node {

namespace {
using usdcc::peers::Room;
using usdcc::peers::RoomType;
constexpr int kRejoinMs = 5000;

// --accept / --admin and their files.
bool readList(const QCommandLineParser& parser, const QString& option, const QString& fileOption, TrustList* list,
              QString* error) {
    list->setFromArguments(parser.values(option));
    if (fileOption.isEmpty() || !parser.isSet(fileOption)) return true;
    QFile file(parser.value(fileOption));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = QStringLiteral("--%1: can't read %2").arg(fileOption, file.fileName());
        return false;
    }
    list->setFileText(QString::fromUtf8(file.readAll()));
    return true;
}
}  // namespace

int runQueue(const QCommandLineParser& parser) {
    const bool hosting = parser.isSet(QStringLiteral("host-room"));
    const QStringList links = parser.values(QStringLiteral("join"));
    if (hosting == !links.isEmpty()) {
        printError(QStringLiteral("queue: --host-room <name> or --join <link>, one of them (--help)."));
        return kExitUsage;
    }

    // Whose jobs it takes, and who may change anyone's.
    TrustList accepted;
    TrustList admins;
    QString error;
    if (!readList(parser, QStringLiteral("accept"), QStringLiteral("accept-file"), &accepted, &error) ||
        !readList(parser, QStringLiteral("admin"), {}, &admins, &error)) {
        printError(QStringLiteral("queue: %1.").arg(error));
        return kExitUsage;
    }
    if (accepted.isEmpty()) USDCC_LOG_WARNING("usdcc.queue", "Nobody is accepted (--accept, --accept-file): every job will be refused");

    // Who it is.
    usdcc::peers::Peers::Options peersOptions = usdcc::app::peersOptions(parser.isSet(QStringLiteral("public")));
    usdcc::peers::Identity identity;
    if (parser.isSet(QStringLiteral("key-file"))) {
        identity = loadKeyFile(parser.value(QStringLiteral("key-file")), &error);
        if (!identity.isValid()) {
            printError(QStringLiteral("queue: %1.").arg(error));
            return kExitUsage;
        }
    } else {
        identity = peersOptions.identity ? peersOptions.identity() : usdcc::peers::Identity();
    }
    if (!identity.isValid()) {
        printError(QStringLiteral("queue: this profile has no key; give one with --key-file."));
        return kExitFailed;
    }
    const QString name = parser.isSet(QStringLiteral("name")) ? parser.value(QStringLiteral("name")) : QStringLiteral("Render queue");
    peersOptions.profile.identity = identity;
    peersOptions.profile.name = name;
    peersOptions.identity = [identity]() { return identity; };
    usdcc::peers::Peers peers(std::move(peersOptions));

    // Its queue: the nodes as workers, and this machine with --render.
    const QString stateFolder = parser.isSet(QStringLiteral("state"))
                                    ? QDir(parser.value(QStringLiteral("state"))).absolutePath()
                                    : QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("queue"));
    const QString outputRoot = parser.isSet(QStringLiteral("output-root")) ? QDir(parser.value(QStringLiteral("output-root"))).absolutePath()
                                                                          : QString();
    usdcc::render::RenderScheduler scheduler;
    if (parser.isSet(QStringLiteral("render"))) scheduler.addWorker(std::make_shared<usdcc::render::LocalWorker>());
    usdcc::render::RenderNetworkOptions networkOptions;
    networkOptions.folder = QDir(stateFolder).filePath(QStringLiteral("network"));
    networkOptions.scheduler = &scheduler;
    networkOptions.isNode = []() { return false; };
    usdcc::render::RenderNetwork network(std::move(networkOptions));

    usdcc::render::RenderQueueOptions queueOptions;
    queueOptions.name = name;
    queueOptions.stateFolder = stateFolder;
    queueOptions.outputRoot = outputRoot;
    queueOptions.scheduler = &scheduler;
    queueOptions.distribute = parser.isSet(QStringLiteral("render")) ? "both" : "peers";
    queueOptions.accepts = [&accepted](const usdcc::peers::Peer& peer) { return accepted.trusts(peer.fingerprint); };
    queueOptions.isAdmin = [&admins](const usdcc::peers::Peer& peer) { return admins.trusts(peer.fingerprint); };
    usdcc::render::RenderQueueServer server(std::move(queueOptions));
    // Before any room: what it had when it stopped.
    if (const int restored = server.restore()) printLine(QStringLiteral("restored %1 job(s)").arg(restored));
    QObject::connect(&server, &usdcc::render::RenderQueueServer::refused, &server, [](const QString& who, const QString& why) {
        printLine(QStringLiteral("refused a job from %1: %2").arg(who, why));
    });
    QSet<QString> announced;
    for (const usdcc::render::QueuedJobInfo& job : server.jobs()) announced.insert(job.id);
    QObject::connect(&server, &usdcc::render::RenderQueueServer::jobsChanged, &server, [&server, &announced]() {
        for (const usdcc::render::QueuedJobInfo& job : server.jobs()) {
            if (announced.contains(job.id)) continue;
            announced.insert(job.id);
            printLine(QStringLiteral("queued %1 (%2) from %3").arg(job.title, job.id, job.submitter));
        }
    });
    QObject::connect(&scheduler, &usdcc::render::RenderScheduler::jobEnded, &server, [&server](const QString& id) {
        if (const usdcc::render::RenderJobPtr job = server.job(id)) {
            printLine(QStringLiteral("%1 %2: %3 file(s)").arg(QString::fromStdString(job->title),
                                                             QLatin1String(usdcc::render::jobStatusName(job->status)))
                          .arg(job->files.size()));
        }
    });
    QObject::connect(&peers, &usdcc::peers::Peers::roomAdded, &server, [&network, &server](Room* room) {
        network.addRoom(room);
        server.addRoom(room);
    });
    QObject::connect(&peers, &usdcc::peers::Peers::roomRemoved, &server, [&network, &server](Room* room) {
        network.removeRoom(room);
        server.removeRoom(room);
    });

    printLine(QStringLiteral("usdcc_node queue \"%1\", fingerprint %2 (%3)")
                  .arg(name, usdcc::peers::shortFingerprint(identity.fingerprint()), identity.fingerprint()));
    printLine(QStringLiteral("state in %1; frames %2").arg(QDir::toNativeSeparators(stateFolder),
                                                          outputRoot.isEmpty() ? QStringLiteral("kept for the artists to fetch")
                                                                               : QStringLiteral("to ") + QDir::toNativeSeparators(outputRoot)));

    // The room: hosted, or joined (again after a lost connection).
    bool stopping = false;
    QHash<Room*, QString> linkOf;
    if (hosting) {
        usdcc::peers::HostOptions host;
        host.name = parser.value(QStringLiteral("host-room"));
        host.type = RoomType::RenderQueue;
        host.profile = peers.profile();
        host.profile.identity = identity;
        const bool local = parser.isSet(QStringLiteral("local"));
        host.address = local ? QHostAddress(QHostAddress::LocalHost) : QHostAddress(QHostAddress::Any);
        host.port = static_cast<quint16>(parser.value(QStringLiteral("port")).toUInt());
        if (parser.isSet(QStringLiteral("public"))) host.admission = usdcc::peers::Admission::Open;
        Room* room = peers.host(host, &error);
        if (!room) {
            printError(QStringLiteral("queue: couldn't host the room: %1.").arg(error));
            return kExitFailed;
        }
        // For the nodes and artists: their --join, and Peers > Join.
        printLine(QStringLiteral("link %1").arg(room->link(local ? QStringLiteral("127.0.0.1") : QString())));
    } else {
        const auto join = [&peers, &linkOf](const QString& link) -> QString {
            QString why;
            Room* room = peers.joinLink(link, &why, RoomType::RenderQueue);
            if (!room) return why.isEmpty() ? QStringLiteral("couldn't join") : why;
            linkOf.insert(room, link);
            return {};
        };
        for (const QString& link : links) {
            if (const QString why = join(link); !why.isEmpty()) {
                printError(QStringLiteral("queue: --join %1: %2.").arg(link.left(40), why));
                return kExitUsage;
            }
        }
        QObject::connect(&peers, &usdcc::peers::Peers::roomRemoved, &server, [&linkOf, &stopping, join, &server](Room* room) {
            const QString link = linkOf.take(room);
            if (stopping || link.isEmpty()) return;
            QTimer::singleShot(kRejoinMs, &server, [join, link, &stopping]() {
                if (stopping) return;
                if (const QString why = join(link); !why.isEmpty()) {
                    USDCC_LOG_WARNING("usdcc.queue", "Couldn't join {} again: {}", link.left(40).toStdString(), why.toStdString());
                }
            });
        });
    }

    // The web page (§4.5).
    std::unique_ptr<QueueWeb> web;
    if (parser.isSet(QStringLiteral("web"))) {
        QueueWebOptions webOptions;
        webOptions.name = name;
        webOptions.port = static_cast<quint16>(parser.value(QStringLiteral("web")).toUInt());
        if (parser.isSet(QStringLiteral("local"))) webOptions.address = QHostAddress(QHostAddress::LocalHost);
        webOptions.pageDirectory = QDir(usdcc::ui::programDataFolder()).filePath(QStringLiteral("node-web"));
        webOptions.accent = QString::fromStdString(
            usdcc::core::Settings::instance().get<std::string>("Appearance/accentColor", std::string("#3d8bfd")));
        if (parser.isSet(QStringLiteral("web-token-file"))) {
            const QString tokenFile = parser.value(QStringLiteral("web-token-file"));
            QFile file(tokenFile);
            if (file.exists()) {
                if (file.open(QIODevice::ReadOnly | QIODevice::Text)) webOptions.token = QString::fromUtf8(file.readAll()).trimmed();
            } else {
                webOptions.token = QueueWeb::makeToken();
                QSaveFile out(tokenFile);
                if (!out.open(QIODevice::WriteOnly) || out.write(webOptions.token.toUtf8()) < 0 || !out.commit()) {
                    printError(QStringLiteral("queue: can't write the token file %1.").arg(tokenFile));
                    return kExitUsage;
                }
                QFile::setPermissions(tokenFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                printLine(QStringLiteral("made the web page's token in %1").arg(QDir::toNativeSeparators(tokenFile)));
            }
            if (webOptions.token.isEmpty()) {
                printError(QStringLiteral("queue: the token file %1 is empty.").arg(tokenFile));
                return kExitUsage;
            }
        }
        web = std::make_unique<QueueWeb>(&server, &scheduler, &network, std::move(webOptions));
        if (!web->start(&error)) {
            printError(QStringLiteral("queue: couldn't serve the web page: %1.").arg(error));
            return kExitFailed;
        }
        printLine(QStringLiteral("web %1").arg(web->link(parser.isSet(QStringLiteral("local")) ? QStringLiteral("127.0.0.1") : QString())
                                                    .toString()));
    }

    // Ctrl+C: its state is saved as it goes; the rooms close, which tells the nodes and artists.
    QEventLoop loop;
    installStopHandler([&]() {
        stopping = true;
        printLine(QStringLiteral("stopping"));
        web.reset();
        for (Room* room : peers.rooms()) room->close(QStringLiteral("The render queue is stopping"));
        QTimer::singleShot(500, &loop, &QEventLoop::quit);
    });
    loop.exec();
    return kExitDone;
}

}  // namespace usdcc::node
