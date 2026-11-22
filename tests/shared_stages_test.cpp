// Shared stages in the app (docs/SHARED_STAGE.md): two instances' controllers in one
// process, in a real room on 127.0.0.1. Anna shares a stage, Ben opens it, edits go
// both ways, a prim from the file below can't be renamed, Ben may only watch, and Anna
// stops sharing with a merge. Runs in usdcc_ui_tests.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/peers/peers.h"
#include "usdcc/peers/room.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/share/live_layers.h"
#include "usdcc/usd/share/presence.h"
#include "usdcc/usd/share/shared_stages.h"
#include "usdcc/usd/share/stage_sync.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace PXR_NS;
using namespace usdcc::peers;
using usdcc::ui::PeersController;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::share::SharedStageInfo;
using usdcc::usd::share::SharedStages;

namespace {

struct Instance {
    std::unique_ptr<PeersController> peers;
    StageManager stages;
    UndoManager undo;
    std::unique_ptr<SharedStages> shared;
    QStringList notices;

    explicit Instance(const QString& name, const QString& folder) {
        Peers::Options options;
        options.profile.name = name;
        options.profile.color = QStringLiteral("#61afef");
        options.roomsFolder = folder;
        options.network = std::make_unique<FakeNetworkDiscovery>(QStringLiteral("shared stage test"));
        peers = std::make_unique<PeersController>(std::make_unique<Peers>(std::move(options)), nullptr,
                                                  PeersController::Hooks{});
        SharedStages::Hooks hooks;
        hooks.ask = [](const QString&, const QString&, const QStringList&) { return 0; };
        hooks.notify = [this](const QString& title, const QString&, const auto&) { notices << title; };
        shared = std::make_unique<SharedStages>(peers.get(), stages, undo, std::move(hooks));
    }
    ~Instance() {
        shared.reset();
        for (const StageRefPtr& stage : std::vector<StageRefPtr>(stages.stages())) stages.closeStage(stage);
    }
};

}  // namespace

USDCC_TEST(shared_stage_two_instances) {
    TestSettings settings;
    PeersController::defineSettings();
    QTemporaryDir folder;
    QTemporaryDir files;
    const QString path = files.filePath(QStringLiteral("shot.usda"));
    {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly), "can't write the stage");
        file.write("#usda 1.0\n(\n    subLayers = [@./sub.usda@]\n)\n"
                   "def Xform \"World\"\n{\n    def Cube \"FromFile\"\n    {\n    }\n}\n");
    }
    {
        QFile sub(files.filePath(QStringLiteral("sub.usda")));
        CHECK(sub.open(QIODevice::WriteOnly), "can't write the sublayer");
        sub.write("#usda 1.0\ndef Scope \"Sub\"\n{\n}\n");
    }

    Instance anna(QStringLiteral("Anna"), folder.path());
    Instance ben(QStringLiteral("Ben"), folder.path());
    QString error;
    PeersController::HostRequest request;
    request.name = QStringLiteral("Shot");
    request.admission = Admission::Invited;
    request.onNetwork = false;
    Room* annaRoom = anna.peers->host(request, &error);
    CHECK(annaRoom, "couldn't host: %s", qPrintable(error));
    if (!annaRoom) return;
    Room* benRoom = ben.peers->joinLinkOrAddress(annaRoom->link(), {}, &error);
    CHECK(benRoom, "couldn't join: %s", qPrintable(error));
    if (!benRoom) return;
    CHECK(QTest::qWaitFor([&] { return benRoom->state() == Room::State::Open; }), "Ben didn't get in");

    // Anna shares her stage.
    const StageRefPtr annaStage = anna.stages.openStage(path.toStdString());
    CHECK(anna.shared->share(annaRoom, annaStage, &error), "couldn't share: %s", qPrintable(error));
    CHECK(annaStage->editTargetLayer() == usdcc::usd::share::liveLayerOf(annaStage->usdStage()),
          "the live layer is Anna's edit target");
    CHECK(QTest::qWaitFor([&] { return ben.shared->shared(benRoom).size() == 1; }), "Ben doesn't see the share");
    if (ben.shared->shared(benRoom).isEmpty()) return;
    const SharedStageInfo info = ben.shared->shared(benRoom).first();
    CHECK(info.name == QStringLiteral("shot.usda") && info.layers.size() == 2, "the share says %s with %d layers",
          qPrintable(info.name), int(info.layers.size()));

    // Anna edits before Ben opens it; Ben gets it in the snapshot.
    annaStage->usdStage()->DefinePrim(SdfPath("/World/Early"), TfToken("Sphere"));
    QTest::qWait(50);
    CHECK(ben.shared->open(benRoom, info.id, &error), "Ben couldn't open it: %s", qPrintable(error));
    const StageRefPtr benStage = ben.shared->stageOf(benRoom, info.id);
    if (!benStage) return;
    CHECK(QTest::qWaitFor([&] { return bool(benStage->usdStage()->GetPrimAtPath(SdfPath("/World/Early"))); }),
          "Ben doesn't have Anna's early edit");

    // Edits both ways.
    annaStage->usdStage()->DefinePrim(SdfPath("/World/FromAnna"), TfToken("Cone"));
    benStage->usdStage()->GetPrimAtPath(SdfPath("/World/FromFile")).CreateAttribute(TfToken("size"),
                                                                                   SdfValueTypeNames->Double).Set(3.0);
    CHECK(QTest::qWaitFor([&] {
              return benStage->usdStage()->GetPrimAtPath(SdfPath("/World/FromAnna")) &&
                     annaStage->usdStage()->GetPrimAtPath(SdfPath("/World/FromFile")).GetAttribute(TfToken("size")).HasAuthoredValue();
          }),
          "the edits didn't cross");
    std::string annaLive, benLive;
    CHECK(QTest::qWaitFor([&] {
              usdcc::usd::share::liveLayerOf(annaStage->usdStage())->ExportToString(&annaLive);
              usdcc::usd::share::liveLayerOf(benStage->usdStage())->ExportToString(&benLive);
              return annaLive == benLive;
          }),
          "the live layers differ:\n%s\nvs\n%s", annaLive.c_str(), benLive.c_str());
    CHECK(QFile(path).size() > 0 && !SdfLayer::Find(path.toStdString())->IsDirty(), "the file below changed");

    // Presence (§8): Ben sees what Anna selects and where she looks, and can follow her.
    {
        using usdcc::usd::share::PeerView;
        using usdcc::usd::share::PresenceHub;
        PresenceHub& hub = PresenceHub::instance();
        anna.stages.setSelectedPaths(annaStage, {SdfPath("/World/FromFile")});
        PeerView view;
        view.cameraToWorld.SetTranslate(GfVec3d(1, 2, 30));
        view.distance = 30;
        hub.reportView(get_pointer(annaStage->usdStage()), view);
        annaStage->setTime(12);
        const UsdStage* benUsd = get_pointer(benStage->usdStage());
        CHECK(QTest::qWaitFor([&] { return !hub.colorsSelecting(benUsd, SdfPath("/World/FromFile")).empty(); }),
              "Ben doesn't see Anna's selection");
        CHECK(QTest::qWaitFor([&] { return !hub.peers(benUsd).empty() && hub.peers(benUsd).front().view == view; }),
              "Ben doesn't see where Anna looks");
        hub.follow(benUsd, annaRoom->me().id);
        CHECK(hub.followedView(benUsd) == view, "Ben doesn't follow Anna's view");
        CHECK(QTest::qWaitFor([&] { return benStage->time() == 12; }), "Ben's frame isn't Anna's (%g)",
              benStage->time());
        CHECK(!usdcc::usd::share::buildPresenceOverlay(benStage->usdStage(), hub.peers(benUsd), UsdTimeCode(12))
                   .lines.empty(),
              "the viewport would draw nothing for Anna");
        hub.stopFollowing(benUsd);
    }

    // Phase C: Carl asks Anna for the stage's files, and opens it from what arrives.
    {
        Instance carl(QStringLiteral("Carl"), folder.path());
        Room* carlRoom = carl.peers->joinLinkOrAddress(annaRoom->link(), {}, &error);
        CHECK(carlRoom && QTest::qWaitFor([&] { return carlRoom->state() == Room::State::Open; }), "Carl didn't get in");
        if (carlRoom) {
            CHECK(QTest::qWaitFor([&] { return carl.shared->shared(carlRoom).size() == 1; }), "Carl doesn't see the share");
            CHECK(carl.shared->requestFiles(carlRoom, info.id, &error), "Carl couldn't ask: %s", qPrintable(error));
            CHECK(QTest::qWaitFor([&] { return carl.shared->isOpen(carlRoom, info.id); }, 20000),
                  "Carl's stage didn't open from the files");
            const StageRefPtr carlStage = carl.shared->stageOf(carlRoom, info.id);
            if (carlStage) {
                const std::string root = carlStage->usdStage()->GetRootLayer()->GetRealPath();
                CHECK(QString::fromStdString(root) != path, "Carl opened Anna's own file");
                CHECK(carlStage->usdStage()->GetPrimAtPath(SdfPath("/Sub")), "the sublayer didn't come with it");
                CHECK(QTest::qWaitFor([&] { return bool(carlStage->usdStage()->GetPrimAtPath(SdfPath("/World/FromAnna"))); }),
                      "Carl doesn't have the shared edits");
            }
            carlRoom->close();
            QTest::qWait(100);
        }
    }

    // A prim from the file can't be renamed while shared; one made in the live layer can.
    CHECK(!usdcc::usd::prim_actions::rename(ben.undo, benStage, SdfPath("/World/FromFile"), "Renamed", ben.stages.stages()),
          "a prim from the file was renamed");
    CHECK(usdcc::usd::prim_actions::rename(ben.undo, benStage, SdfPath("/World/FromAnna"), "Cone2", ben.stages.stages()),
          "a prim made in the live layer wasn't renamed");
    CHECK(QTest::qWaitFor([&] { return bool(annaStage->usdStage()->GetPrimAtPath(SdfPath("/World/Cone2"))); }),
          "Anna doesn't see the rename");

    // Ben may only watch: his edit is taken back.
    anna.shared->setMayEdit(annaRoom, info.id, benRoom->me().id, false);
    CHECK(QTest::qWaitFor([&] { return !ben.shared->mayEdit(benRoom, info.id, benRoom->me().id); }),
          "Ben doesn't hear he may only watch");
    benStage->usdStage()->DefinePrim(SdfPath("/World/Sneaky"));
    CHECK(QTest::qWaitFor([&] { return !benStage->usdStage()->GetPrimAtPath(SdfPath("/World/Sneaky")); }),
          "Ben's edit stayed");
    CHECK(!annaStage->usdStage()->GetPrimAtPath(SdfPath("/World/Sneaky")), "Anna got the watcher's edit");

    // Anna stops sharing and merges: the edits are in her root layer, unsaved; Ben keeps a copy.
    CHECK(anna.shared->stop(annaRoom, info.id, SharedStages::Ending::MergeIntoRoot, &error), "couldn't stop: %s",
          qPrintable(error));
    const SdfLayerHandle root = annaStage->usdStage()->GetRootLayer();
    CHECK(root->GetPrimAtPath(SdfPath("/World/Cone2")) && root->IsDirty(), "the merge isn't in the root layer");
    CHECK(annaStage->editTargetLayer() == root, "Anna's edit target is back");
    CHECK(!usdcc::usd::share::liveLayerOf(annaStage->usdStage()), "Anna's stage is still shared");
    CHECK(QTest::qWaitFor([&] { return !ben.shared->isOpen(benRoom, info.id); }), "Ben's session didn't end");
    CHECK(benStage->usdStage()->GetPrimAtPath(SdfPath("/World/Cone2")), "Ben lost his copy");
    CHECK(!ben.notices.isEmpty(), "Ben wasn't told");
    anna.undo.stack(annaStage)->undo();
    CHECK(!root->GetPrimAtPath(SdfPath("/World/Cone2")), "the merge can't be undone");
    benRoom->close();
    annaRoom->close();
}
