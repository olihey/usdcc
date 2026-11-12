// The real panel controller with injected engine/presentation. No CUDA/Vulkan,
// vendor headers, runtime, Python RTX package, or GPU is needed.
#include "rtx_panel.h"
#include "runtime_identity.h"
#include "test_harness.h"
#include "test_settings.h"
#include "usdcc/core/settings.h"
#include "usdcc/usd/stage_manager.h"
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QDir>
#include <QLabel>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <utility>

namespace {
using namespace usdcc::rtx;
struct Frame : GpuFrame {
    QSize size() const override { return {64, 36}; }
    bool copied() const override { return true; }
    void save(const QString&) const override {}
};
struct EngineState {
    Schedule schedule;
    EngineUpdate output;
    int replacements = 0, updates = 0;
};
class FakeEngine final : public Engine {
public:
    explicit FakeEngine(std::shared_ptr<EngineState> value) : state(std::move(value)) {}
    ~FakeEngine() override { close(); }
    uint64_t replace(std::shared_ptr<Snapshot> scene, int budget) override {
        ++state->replacements; state->schedule.replace(std::move(scene), budget); return generation();
    }
    void update(Camera camera, QSize size, const std::map<QString, std::array<double,16>>&) override {
        ++state->updates; state->schedule.desired.camera = camera; state->schedule.desired.size = size;
    }
    void setSelection(QStringList paths) override { state->schedule.desired.selection = std::move(paths); }
    void setActive(bool value) override { state->schedule.active = value; }
    void setVisible(bool value) override { state->schedule.visible = value; }
    void save(QString file, std::shared_ptr<GpuFrame>) override { state->output.saved = file; }
    EngineUpdate poll() override { return std::exchange(state->output, {}); }
    uint64_t generation() const override { return state->schedule.desired.generation; }
    void close() override { state->schedule.closed = true; state->schedule.active = false; }
    bool isClosed() const override { return state->schedule.closed; }
private:
    std::shared_ptr<EngineState> state;
};
struct SurfaceState { int initialized = 0, frames = 0; bool closed = false; };
class FakePresentation final : public Presentation {
public:
    explicit FakePresentation(std::shared_ptr<SurfaceState> value) : state(std::move(value)), content(new QWidget) {}
    QWidget* widget() override { return content; }
    QWindow* window() override { return nullptr; }
    QString initialize() override { ++state->initialized; return "Test presentation"; }
    QString error() const override { return {}; }
    std::shared_ptr<GpuBridge> bridge() const override { return {}; }
    void setFrame(std::shared_ptr<GpuFrame> value) override {
        frame = std::move(value); ++state->frames; if (presented) presented(frame->size(), {});
    }
    std::shared_ptr<GpuFrame> currentFrame() const override { return frame; }
    void clear() override { frame.reset(); }
    void releaseResources() override { clear(); }
    void close() override { clear(); state->closed = true; presented = {}; }
private:
    std::shared_ptr<SurfaceState> state;
    QWidget* content; // owned by the actual RenderViewPanel
    std::shared_ptr<GpuFrame> frame;
};
void file(const QString& path) { QFile f(path); CHECK(f.open(QIODevice::WriteOnly), "fake runtime locator file"); }
}

USDCC_TEST(rtx_native_runtime_identity) {
    QTemporaryDir dir;
    QDir().mkpath(dir.path()+"/bin");
    const QByteArray payload("test loader");
    const auto hash = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
    usdcc::rtx::RuntimePin pin{"ovrtx", "test-build", hash.constData()};
    CHECK(usdcc::rtx::runtimeProblem(dir.path(), pin).contains("metadata missing"), "missing runtime metadata is actionable");
    QFile metadata(dir.filePath("ovrtx-build-info.json")); metadata.open(QIODevice::WriteOnly);
    metadata.write(QJsonDocument(QJsonObject{{"build_string", "wrong"}}).toJson()); metadata.close();
    CHECK(usdcc::rtx::runtimeProblem(dir.path(), pin).contains("requires ovrtx build"), "same API version is insufficient when builds differ");
    metadata.open(QIODevice::WriteOnly|QIODevice::Truncate);
    metadata.write(QJsonDocument(QJsonObject{{"build_string", "test-build"}}).toJson()); metadata.close();
    QFile loader(dir.filePath("bin/ovrtx-dynamic.dll")); loader.open(QIODevice::WriteOnly); loader.write(payload); loader.close();
    CHECK(usdcc::rtx::runtimeProblem(dir.path(), pin).isEmpty(), "matching runtime metadata and loader hash pass without loading a DLL");
    loader.open(QIODevice::WriteOnly|QIODevice::Truncate); loader.write("modified"); loader.close();
    CHECK(usdcc::rtx::runtimeProblem(dir.path(), pin).contains("does not match"), "a mismatched loader is refused before vendor code runs");
}

USDCC_TEST(rtx_native_panel_controller_without_sdk) {
    const auto oldRtx = qgetenv("USDCC_RTX_OVRTX_ROOT"), oldStage = qgetenv("USDCC_RTX_OVSTAGE_ROOT");
    qunsetenv("USDCC_RTX_OVRTX_ROOT"); qunsetenv("USDCC_RTX_OVSTAGE_ROOT");
    const auto restore = qScopeGuard([&] {
        if (oldRtx.isNull()) qunsetenv("USDCC_RTX_OVRTX_ROOT"); else qputenv("USDCC_RTX_OVRTX_ROOT", oldRtx);
        if (oldStage.isNull()) qunsetenv("USDCC_RTX_OVSTAGE_ROOT"); else qputenv("USDCC_RTX_OVSTAGE_ROOT", oldStage);
    });
    TestSettings settings;
    usdcc::rtx::defineRtxSettings();
    QTemporaryDir folder;
    QDir().mkpath(folder.path()+"/runtime/ovrtx/bin");
    QDir().mkpath(folder.path()+"/runtime/ovstage/bin");
    file(folder.path()+"/runtime/ovrtx/bin/ovrtx-dynamic.dll");
    file(folder.path()+"/runtime/ovstage/bin/ovstage-dynamic.dll");
    usdcc::usd::StageManager stages;
    auto stage = stages.createStage();
    PXR_NS::UsdGeomCube::Define(stage->usdStage(), PXR_NS::SdfPath("/Group/Cube"));
    stages.setSelectedPaths(stage, {PXR_NS::SdfPath("/Group")});
    usdcc::plugin::Host host;
    host.folder = std::filesystem::path(folder.path().toStdWString()); host.stageManager = &stages;
    std::vector<std::shared_ptr<EngineState>> engines;
    auto surface = std::make_shared<SurfaceState>();
    EngineFactory factory = [&](QString, QString, std::shared_ptr<GpuBridge>) {
        auto state = std::make_shared<EngineState>(); engines.push_back(state); return std::make_unique<FakeEngine>(state);
    };
    std::unique_ptr<usdcc::usd::RenderViewPanel> panel(createRtxPanel(host, stage, "RTX controller", nullptr,
        factory, [surface] { return std::make_unique<FakePresentation>(surface); }));
    panel->resize(600, 400); panel->show(); QTest::qWait(180);
    CHECK(engines.empty() && surface->initialized == 0, "panel creation never initializes an engine or GPU");
    auto* render = panel->findChild<QToolButton*>("renderButton");
    render->click(); QTest::qWait(80);
    CHECK(engines.size() == 1 && surface->initialized == 1, "Render creates the injected engine once");
    auto first = engines.back();
    CHECK(first->schedule.active && first->schedule.desired.selection == QStringList({"/Group/Cube"}),
          "initial stage selection is expanded before rendering");
    CHECK(first->schedule.desired.snapshot && QFile::exists(first->schedule.desired.snapshot->file),
          "the real controller gives the engine an owned snapshot");
    auto& registry = usdcc::core::Settings::instance();
    CHECK(registry.get<bool>(kMatchStormLightsKey, false) && registry.get<double>(kLightMultiplierKey) == 1.0,
          "Match Storm light intensities is on by default, with multiplier 1");
    const LightMatch matched{.on = true, .multiplier = 1.0};
    CHECK(first->schedule.desired.snapshot && first->schedule.desired.snapshot->light == matched,
          "a default snapshot matches Storm");
    const int beforeLight = first->replacements;
    registry.setValue(kMatchStormLightsKey, false);
    CHECK(first->replacements == beforeLight + 1 && !first->schedule.desired.snapshot->light.on,
          "turning light matching off reloads the snapshot without it");
    registry.setValue(kMatchStormLightsKey, true);
    registry.setValue(kLightMultiplierKey, 2.0);
    CHECK(first->replacements == beforeLight + 3 && first->schedule.desired.snapshot->light.on &&
              first->schedule.desired.snapshot->light.multiplier == 2.0,
          "turning it on again and changing the multiplier reload the snapshot with them");
    CHECK(!registry.setValue(kLightMultiplierKey, 5000.0) && registry.get<double>(kLightMultiplierKey) == 2.0,
          "a multiplier out of range is refused");
    registry.reset(kLightMultiplierKey);
    panel->findChild<QCheckBox*>("renderLive")->setChecked(false);
    stages.setSelectedPaths(stage, {});
    CHECK(first->schedule.desired.selection.isEmpty(), "selection follows the app with Update on Edits off");
    render->click(); stages.setSelectedPaths(stage, {PXR_NS::SdfPath("/Group/Cube")});
    CHECK(!first->schedule.active && !first->schedule.desired.selection.isEmpty(), "Stop retains the newest desired selection");
    render->click(); panel->hide();
    CHECK(first->schedule.active && !first->schedule.visible, "hiding suppresses submissions without losing Render state");
    panel->show(); QTest::qWait(350); // drain the independent resize debounce first
    CHECK(first->schedule.visible, "show resumes the same engine");
    auto local = panel->currentCamera();
    local.cameraToWorld.SetTranslateOnly(PXR_NS::GfVec3d(2, 3, 12));
    const int updates = first->updates;
    panel->setLocalCamera(local); panel->setLocalCamera(local); QTest::qWait(80);
    CHECK(first->updates == updates + 1, "the controller coalesces camera moves before submitting them");
    first->output.frame = std::make_shared<Frame>(); first->output.status = "Done"; first->output.done = true;
    QTest::qWait(60);
    CHECK(surface->frames == 1 && panel->property("nativeRtxImageSize").toSize() == QSize(64,36),
          "completed engine output reaches the presentation and shared controls");
    first->output.saveError = "disk full"; QTest::qWait(50);
    CHECK(render->isChecked(), "a Save Image failure does not stop rendering");
    const int replacements = first->replacements;
    first->output.reloadNeeded = true; QTest::qWait(60);
    CHECK(first->replacements == replacements + 1, "a failed incremental update requests a fresh snapshot");
    auto other = stages.createStage();
    panel->setStage(other); QTest::qWait(80);
    CHECK(first->schedule.closed && engines.size() == 2, "switching stages closes the old client and creates a new one");
    CHECK(!panel->property("nativeRtxImageSize").toSize().isValid(), "stage switching clears the old displayed image");
    auto second = engines.back(); second->output.error = "renderer unavailable"; QTest::qWait(60);
    CHECK(!render->isChecked() && !second->schedule.active, "an engine failure stops the panel and reports it");
    panel.reset();
    CHECK(second->schedule.closed && surface->closed, "deletion closes the client and detaches presentation callbacks");
}
