// usdcc_node serve (docs/RENDER_NODE.md §4.2): a render node in render queue rooms, rendering
// the tasks artists' instances hand out, for trusted fingerprints only, until stopped. It
// speaks FINAL_RENDER.md §7.2 as an app instance with Render for peers on does: the
// RenderNetwork is the app's, with nobody to ask.

#include "node.h"
#include "serve_support.h"

#include "startup.h"
#include "usdcc/core/log.h"
#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/usd/render/final_renderer.h"
#include "usdcc/usd/render/render_network.h"
#include "usdcc/usd/render/render_scheduler.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileSystemWatcher>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>

#include <memory>

namespace usdcc::node {

namespace {
using usdcc::peers::Room;
using usdcc::peers::RoomType;

// How long after a room is lost its link is joined again.
constexpr int kRejoinMs = 5000;
}  // namespace

int runServe(const QCommandLineParser& parser) {
    const QStringList links = parser.values(QStringLiteral("join"));
    const bool joinPublic = parser.isSet(QStringLiteral("public"));
    if (links.isEmpty() && !joinPublic) {
        printError(QStringLiteral("serve: which rooms? --join <link> or --public (--help)."));
        return kExitUsage;
    }

    // Whom it renders for.
    TrustList trust;
    trust.setFromArguments(parser.values(QStringLiteral("trust")));
    const QString trustFile = parser.value(QStringLiteral("trust-file"));
    QFileSystemWatcher trustWatcher;
    const auto readTrustFile = [&trust, &trustWatcher, trustFile]() {
        QFile file(trustFile);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            USDCC_LOG_WARNING("usdcc.node", "Can't read the trust file {}", trustFile.toStdString());
            trust.setFileText({});
        } else {
            trust.setFileText(QString::fromUtf8(file.readAll()));
        }
        // An editor that replaces the file takes it off the watch list.
        if (!trustWatcher.files().contains(trustFile)) trustWatcher.addPath(trustFile);
    };
    if (!trustFile.isEmpty()) {
        if (!QFile::exists(trustFile)) {
            printError(QStringLiteral("serve: no trust file %1.").arg(trustFile));
            return kExitUsage;
        }
        readTrustFile();
        QObject::connect(&trustWatcher, &QFileSystemWatcher::fileChanged, &trustWatcher, [&readTrustFile, trustFile]() {
            readTrustFile();
            USDCC_LOG_INFO("usdcc.node", "Read the trust file {} again", trustFile.toStdString());
        });
    }
    if (trust.isEmpty()) {
        USDCC_LOG_WARNING("usdcc.node", "Nobody is trusted (--trust, --trust-file): every job will be declined");
    }

    // What it offers: every renderer that renders here, or those --renderers names.
    if (parser.isSet(QStringLiteral("renderers"))) {
        const QStringList wanted = parser.value(QStringLiteral("renderers")).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& id : wanted) {
            if (!usdcc::render::FinalRenderers::instance().find(id.trimmed().toStdString())) {
                printError(QStringLiteral("serve: no renderer '%1' (usdcc_node renderers lists them).").arg(id.trimmed()));
                return kExitUsage;
            }
        }
        std::vector<std::string> others;
        for (const auto& renderer : usdcc::render::FinalRenderers::instance().all()) {
            if (!wanted.contains(QString::fromStdString(renderer->id()))) others.push_back(renderer->id());
        }
        for (const std::string& id : others) usdcc::render::FinalRenderers::instance().remove(id);
    }

    // Who it is: --key-file's key, else the profile's; the name peers see.
    usdcc::peers::Peers::Options peersOptions = usdcc::app::peersOptions(joinPublic);
    if (parser.isSet(QStringLiteral("key-file"))) {
        QString error;
        const usdcc::peers::Identity identity = loadKeyFile(parser.value(QStringLiteral("key-file")), &error);
        if (!identity.isValid()) {
            printError(QStringLiteral("serve: %1.").arg(error));
            return kExitUsage;
        }
        peersOptions.profile.identity = identity;
        peersOptions.identity = [identity]() { return identity; };
    }
    peersOptions.profile.name = parser.isSet(QStringLiteral("name")) ? parser.value(QStringLiteral("name"))
                                                                     : QSysInfo::machineHostName();
    const usdcc::peers::Identity identity =
        peersOptions.profile.identity.isValid() ? peersOptions.profile.identity : peersOptions.identity();
    peersOptions.profile.identity = identity;
    peersOptions.identity = [identity]() { return identity; };
    if (!identity.isValid()) {
        printError(QStringLiteral("serve: this profile has no key; give one with --key-file."));
        return kExitFailed;
    }
    // For the artists' trust lists, if they keep one.
    printLine(QStringLiteral("usdcc_node serve as \"%1\", fingerprint %2 (%3)")
                  .arg(peersOptions.profile.name, usdcc::peers::shortFingerprint(identity.fingerprint()), identity.fingerprint()));
    usdcc::peers::Peers peers(std::move(peersOptions));

    // The app's network, as a node that's always on and asks nobody.
    usdcc::render::RenderScheduler scheduler;
    usdcc::render::RenderNetworkOptions networkOptions;
    networkOptions.folder = parser.isSet(QStringLiteral("inbox"))
                                ? QDir(parser.value(QStringLiteral("inbox"))).absolutePath()
                                : QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                      .filePath(QStringLiteral("render-network"));
    USDCC_LOG_INFO("usdcc.node", "Scenes from peers are kept in {}", QDir::toNativeSeparators(networkOptions.folder).toStdString());
    networkOptions.scheduler = &scheduler;
    // Renders for peers in every room it joins.
    networkOptions.isNode = []() { return true; };
    networkOptions.trusted = [&trust](const usdcc::peers::Peer& peer) { return trust.trusts(peer.fingerprint); };
    // No ask: a job from anyone else is declined.
    usdcc::render::RenderNetwork network(std::move(networkOptions));
    QObject::connect(&peers, &usdcc::peers::Peers::roomAdded, &network, [&network](Room* room) { network.addRoom(room); });

    // The rooms: each link joined at start and again once it's lost.
    bool stopping = false;
    QHash<Room*, QString> linkOf;
    const auto join = [&peers, &linkOf](const QString& link) -> QString {
        QString error;
        Room* room = peers.joinLink(link, &error, RoomType::RenderQueue);
        if (!room) return error.isEmpty() ? QStringLiteral("couldn't join") : error;
        linkOf.insert(room, link);
        QObject::connect(room, &Room::stateChanged, room, [room](Room::State state) {
            if (state == Room::State::Open) printLine(QStringLiteral("joined %1").arg(room->name()));
        });
        return {};
    };
    for (const QString& link : links) {
        if (const QString error = join(link); !error.isEmpty()) {
            printError(QStringLiteral("serve: --join %1: %2.").arg(link.left(40), error));
            return kExitUsage;
        }
    }
    QObject::connect(&peers, &usdcc::peers::Peers::roomRemoved, &network, [&](Room* room) {
        network.removeRoom(room);
        const QString link = linkOf.take(room);
        if (stopping) return;
        printLine(QStringLiteral("left %1").arg(room->name()));
        if (!link.isEmpty()) {
            QTimer::singleShot(kRejoinMs, &network, [&join, &stopping, link]() {
                if (stopping) return;
                if (const QString error = join(link); !error.isEmpty()) {
                    USDCC_LOG_WARNING("usdcc.node", "Couldn't join {} again: {}", link.left(40).toStdString(), error.toStdString());
                }
            });
        }
    });
    // Public rooms, as they're found.
    QSet<QString> joined;
    if (joinPublic) {
        const auto joinFound = [&peers, &joined, &stopping]() {
            if (stopping) return;
            for (const usdcc::peers::FoundRoom& found : peers.foundRooms(RoomType::RenderQueue)) {
                if (!isPublicRenderRoom(found) || joined.contains(found.roomId())) continue;
                joined.insert(found.roomId());
                if (Room* room = peers.join(found)) {
                    QObject::connect(room, &Room::closed, room, [&joined, id = found.roomId()](const QString&) { joined.remove(id); });
                }
            }
        };
        QObject::connect(&peers, &usdcc::peers::Peers::foundRoomsChanged, &network, joinFound);
        peers.setLooking(true);
        joinFound();
    }

    // Ctrl+C or the console closing: leaving the rooms tells the submitters, which put this
    // node's tasks back in their queues (FINAL_RENDER.md §7.1); then exit.
    QEventLoop loop;
    installStopHandler([&]() {
        stopping = true;
        printLine(QStringLiteral("stopping"));
        for (Room* room : peers.rooms()) room->close(QStringLiteral("The render node is stopping"));
        QTimer::singleShot(500, &loop, &QEventLoop::quit);
    });
    loop.exec();
    return kExitDone;
}

}  // namespace usdcc::node
