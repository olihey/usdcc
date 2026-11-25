// Panels don't wait for servers (docs/REMOTE_ASSETS.md §10): against a WebDAV server inside
// this program that takes three seconds to answer, the arcs, the dependencies, the layers, an
// asset field and a USDA link are there at once, as resolving, and right when it has answered.
// Runs in usdcc_ui_tests (offscreen), which registers the WebDAV plugin at start.
#include "test_harness.h"

#include "assets_test_server.h"

#include "usdcc/assets/config.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/disk_cache.h"
#include "usdcc/assets/lookup.h"
#include "usdcc/assets/no_wait.h"
#include "usdcc/assets/transport.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/asset_answers.h"
#include "usdcc/usd/composition_arcs.h"
#include "usdcc/usd/dependencies.h"
#include "usdcc/usd/layer_stack.h"
#include "usdcc/usd/properties/property_editor.h"

#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

namespace assets = usdcc::assets;
namespace fs = std::filesystem;
using namespace usdcc::usd;
using usdcc::test::FolderServer;
using usdcc::test::ServerRequest;
using usdcc::test::TestServer;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

constexpr int kDelayMs = 3000;
constexpr int kAtOnceMs = 1000;

struct SlowServer {
    QTemporaryDir dir;
    FolderServer folder;
    TestServer server;

    SlowServer() : server([this](const ServerRequest& request) { return serveFolder(folder, request); }) {
        folder.root = fs::path(dir.filePath(QStringLiteral("prod")).toStdWString());
        // A folder each: a listing answers for everything in it, and composing lists look.usda's.
        fs::create_directories(folder.root / "tex");
        fs::create_directories(folder.root / "props");
        std::ofstream(folder.root / "props/chair.usda") << "#usda 1.0\n(\n    defaultPrim = \"Chair\"\n)\n\ndef Xform \"Chair\"\n{\n}\n";
        std::ofstream(folder.root / "tex/wood.png") << "png";
        std::ofstream(folder.root / "look.usda") << "#usda 1.0\n";
        folder.delayMs = kDelayMs;
        assets::DiskCache::setSharedRoot(fs::path(dir.filePath(QStringLiteral("cache")).toStdWString()));
        assets::Endpoint endpoint;
        endpoint.id = "prod";
        endpoint.scheme = "webdav";
        endpoint.url = server.url();
        assets::Config config;
        config.environment = "prod";
        config.endpoints["prod"] = endpoint;
        config.environments["prod"]["webdav"]["studio"] = {"prod"};
        assets::setDefaultConfig(config);
        assets::forgetLookups();
    }
    ~SlowServer() {
        folder.delayMs = 0;
        assets::stopAsking();
        assets::useDefaultConfigFile();
        assets::forgetLookups();
    }
};

const Dependency* dependency(const std::vector<Dependency>& all, const std::string& path) {
    const auto found = std::find_if(all.begin(), all.end(), [&](const Dependency& each) { return each.resolvedPath == path; });
    return found == all.end() ? nullptr : &*found;
}

template <class Condition>
bool waitUntil(Condition condition, int ms = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

}  // namespace

USDCC_TEST(asset_answers_panels_dont_wait) {
    assets::loadProtocolPlugins();
    if (!assets::transportFor("webdav")) return;
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    SlowServer slow;
    // Anything a test before this one left behind.
    AssetAnswers& answers = AssetAnswers::instance();
    assets::setAnswerListener(nullptr);

    const SdfLayerRefPtr root = SdfLayer::CreateAnonymous("root.usda");
    root->ImportFromString(
        "#usda 1.0\n(\n    subLayers = [@webdav://studio/look.usda@]\n)\n\n"
        "def Xform \"A\" (\n    prepend payload = @webdav://studio/props/chair.usda@\n)\n{\n"
        "    asset map = @webdav://studio/tex/wood.png@\n}\n");
    const ArResolverContext context{assets::Context(assets::defaultConfig(), "prod")};
    const SdfLayerRefPtr session = SdfLayer::CreateAnonymous("session.usda");
    UsdStagePopulationMask mask;
    mask.Add(SdfPath("/A"));
    // The payload stays unloaded and nobody reads the texture: nobody has an answer about them.
    root->SetSubLayerPaths({});
    const UsdStageRefPtr stage = UsdStage::OpenMasked(root, session, context, mask, UsdStage::LoadNone);
    CHECK(bool(stage), "the stage opens");
    if (!stage) return;
    stage->MuteLayer("webdav://studio/look.usda");
    root->SetSubLayerPaths({"webdav://studio/look.usda"});

    int changes = 0;
    QObject receiver;
    QObject::connect(&answers, &AssetAnswers::changed, &receiver, [&changes]() { ++changes; });
    // As the app has it: the listener AssetAnswers made is set again.
    assets::setAnswerListener([&answers]() { QMetaObject::invokeMethod(&answers, &AssetAnswers::changed, Qt::QueuedConnection); });

    QElapsedTimer timer;
    timer.start();
    const std::vector<ArcInfo> arcs = compositionArcs(stage->GetPrimAtPath(SdfPath("/A")));
    CHECK(arcs.size() == 1 && arcs[0].resolving && !arcs[0].missing, "the payload is resolving, not missing");
    const std::vector<Dependency> files = stageDependencies(stage, true);
    const Dependency* wood = dependency(files, "webdav://studio/tex/wood.png");
    const Dependency* chair = dependency(files, "webdav://studio/props/chair.usda");
    CHECK(wood && wood->resolving && !wood->missing, "so is the texture, in the dependencies");
    CHECK(chair && chair->resolving && !chair->missing, "and the payload there");
    const std::vector<LayerEntry> tree = layerTree(stage);
    // Composing resolved it, muted or not: there's nothing left to wait for.
    CHECK(tree.size() == 2 && tree[1].children.size() == 1 && tree[1].children[0].muted && !tree[1].children[0].missing,
          "the muted sublayer isn't missing");

    properties::EditorHints hints;
    hints.anchorLayer = root;
    hints.resolverContext = context;
    const std::unique_ptr<properties::PropertyEditor> field(
        properties::createPropertyEditor(SdfValueTypeNames->Asset, hints));
    field->setValue(VtValue(SdfAssetPath("webdav://studio/tex/wood.png")));
    auto* sub = qobject_cast<QLabel*>(field->subLineWidget());
    CHECK(sub && sub->text() == QStringLiteral("Resolving..."), "the asset field says so");
    CHECK(timer.elapsed() < kAtOnceMs, "and none of it waited: %lld ms", static_cast<long long>(timer.elapsed()));

    CHECK(waitUntil([&]() { return sub && sub->text().contains(QStringLiteral("prod")); }),
          "the field follows, with the server: [%s]", sub ? sub->text().toUtf8().constData() : "");
    CHECK(changes > 0, "the panels are told");
    const auto settled = [&]() {
        const std::vector<Dependency> now = stageDependencies(stage, true);
        return std::none_of(now.begin(), now.end(), [](const Dependency& each) { return each.resolving; });
    };
    CHECK(waitUntil(settled), "everything is answered in the end");
    timer.restart();
    const std::vector<Dependency> after = stageDependencies(stage, true);
    wood = dependency(after, "webdav://studio/tex/wood.png");
    chair = dependency(after, "webdav://studio/props/chair.usda");
    CHECK(wood && !wood->missing && wood->origin == "prod", "the texture is found, from prod");
    CHECK(chair && !chair->missing && chair->origin == "prod", "and the payload");
    const std::vector<ArcInfo> arcsAfter = compositionArcs(stage->GetPrimAtPath(SdfPath("/A")));
    CHECK(arcsAfter.size() == 1 && arcsAfter[0].resolvedPath == "webdav://studio/props/chair.usda" && arcsAfter[0].origin == "prod",
          "the arc has the path and the server");

    // What isn't there is missing, not resolving for ever.
    root->GetPrimAtPath(SdfPath("/A"));
    stage->GetPrimAtPath(SdfPath("/A")).GetAttribute(TfToken("map")).Clear();
    CHECK(timer.elapsed() < kAtOnceMs, "still without waiting: %lld ms", static_cast<long long>(timer.elapsed()));
    field->setValue(VtValue(SdfAssetPath("webdav://studio/tex/gone.png")));
    CHECK(waitUntil([&]() { return sub && sub->text() == QStringLiteral("Unresolved"); }), "a missing file ends as unresolved");
}

USDCC_TEST(asset_answers_outside_a_scope_waits) {
    assets::loadProtocolPlugins();
    if (!assets::transportFor("webdav")) return;
    SlowServer slow;
    slow.folder.delayMs = 0;
    const ArResolverContextBinder binder(ArResolverContext(assets::Context(assets::defaultConfig(), "prod")));
    CHECK(!assets::NoWait::active() && !ArGetResolver().Resolve("webdav://studio/props/chair.usda").empty(), "resolving is as it was");
    {
        const assets::NoWait noWait;
        const std::size_t before = assets::NoWait::unanswered();
        CHECK(!ArGetResolver().Resolve("webdav://studio/props/chair.usda").empty() && assets::NoWait::unanswered() == before,
              "and what it found is known without waiting");
    }
    CHECK(!assets::NoWait::active(), "until the scope ends");
}
