// Distributed final renders (docs/FINAL_RENDER.md §7), two instances in one process: a room on
// 127.0.0.1, a submitter and a render node, each with its own RenderNetwork. Tasks go to the
// node and come back stitched; the scene goes packaged or shared; trust and declining; a node
// that leaves mid-task; render_queue rooms only, the rooms a job may use, one worker for a peer
// in two rooms, what a node says it is rendering; and the wire formats.
#include "test_harness.h"

#include "usdcc/peers/room.h"
#include "usdcc/usd/render/render_bundle.h"
#include "usdcc/usd/render/render_job.h"
#include "usdcc/usd/render/render_network.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render/render_spec.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdRender/settings.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::render;
using usdcc::peers::Peer;
using usdcc::peers::Room;
using usdcc::peers::RoomType;

namespace {

bool waitFor(const std::function<bool()>& done, int ms = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

bool ended(const RenderJobPtr& job) {
    return job->status == RenderJob::Status::Done || job->status == RenderJob::Status::Failed ||
           job->status == RenderJob::Status::Cancelled;
}

// R = x / width, G = y / height of the whole image, B = the frame; or it never finishes.
class NetworkGradient : public FinalRenderer {
public:
    std::string id() const override { return "test.network"; }
    std::string name() const override { return "Network Gradient"; }
    Capabilities capabilities() const override { return {true, true}; }
    std::vector<std::string> namespaces() const override { return {"network"}; }
    std::vector<RenderSettingField> settingsSchema() const override {
        RenderSettingField f;
        f.attribute = "network:gain";
        f.rendererKey = TfToken("gain");
        f.type = RenderSettingField::Type::Float;
        f.defaultValue = VtValue(1.0f);
        return {f};
    }
    struct Handle : RenderTaskHandle {
        void cancel() override {}
    };
    std::unique_ptr<RenderTaskHandle> start(const RenderTask& task, TaskCallbacks callbacks) override {
        ++started;
        lastStage = task.stage;
        if (task.settings.count(TfToken("gain"))) gain = VtValue::Cast<float>(task.settings.at(TfToken("gain"))).Get<float>();
        if (hang) return std::make_unique<Handle>();
        FloatImage image(task.region.width, task.region.height);
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x) {
                float* p = image.row(y) + x * 4;
                p[0] = float(task.region.x + x) / task.resolution[0];
                p[1] = float(task.region.y + y) / task.resolution[1];
                p[2] = float(task.frame);
                p[3] = 1.0f;
            }
        std::map<std::string, FloatImage> images;
        for (const RenderAov& aov : task.aovs) images[aov.name] = image;
        callbacks.progress(0.5, "Half");
        callbacks.finished(std::move(images));
        return std::make_unique<Handle>();
    }
    int started = 0;
    bool hang = false;
    float gain = 0.0f;
    UsdStageRefPtr lastStage;
};

// One instance: its network, its queue, its folder.
struct Instance {
    QTemporaryDir folder;
    RenderScheduler scheduler;
    bool node = false;
    bool trusted = true;
    int asked = 0;
    std::function<bool()> answer = [] { return true; };
    std::unique_ptr<RenderNetwork> network;

    explicit Instance(bool isNode, bool local) : node(isNode) {
        if (local) scheduler.addWorker(std::make_shared<LocalWorker>());
        RenderNetworkOptions options;
        options.folder = folder.path();
        options.scheduler = &scheduler;
        options.isNode = [this] { return node; };
        options.trusted = [this](const Peer&) { return trusted; };
        options.ask = [this](const Peer&, const QString&, std::function<void(bool)> reply) {
            ++asked;
            reply(answer());
        };
        network = std::make_unique<RenderNetwork>(std::move(options));
    }
};

// A room the submitter hosts and the node joins, with the profiles' keys (so that the node is
// the same peer in a second room).
struct RoomPair {
    std::unique_ptr<Room> host;
    std::unique_ptr<Room> member;

    RoomPair(const QString& name, const usdcc::peers::Profile& submitter, const usdcc::peers::Profile& node,
             RoomType type = RoomType::RenderQueue) {
        usdcc::peers::HostOptions options;
        options.name = name;
        options.type = type;
        options.profile = submitter;
        QString error;
        host.reset(Room::host(options, nullptr, &error));
        CHECK(host != nullptr, "a room: %s", qPrintable(error));
        if (!host) return;
        usdcc::peers::JoinOptions join;
        join.profile = node;
        join.type = type;
        join.secret = host->secret();
        member.reset(Room::join(QStringLiteral("127.0.0.1:%1").arg(host->port()), join));
        waitFor([this] { return member->state() == Room::State::Open; });
    }
};

usdcc::peers::Profile profile(const QString& name) {
    usdcc::peers::Profile p;
    p.name = name;
    p.identity = usdcc::peers::Identity::generate();
    p.instance = name.toLower();
    return p;
}

struct TwoInstances {
    usdcc::peers::Profile submitterProfile = profile(QStringLiteral("Submitter"));
    usdcc::peers::Profile nodeProfile = profile(QStringLiteral("Node"));
    RoomPair farm{QStringLiteral("Render farm"), submitterProfile, nodeProfile};
    std::unique_ptr<Room>& host = farm.host;
    std::unique_ptr<Room>& member = farm.member;
    Instance submitter{false, false};
    Instance node{true, false};

    TwoInstances() {
        if (!member) return;
        submitter.network->addRoom(host.get());
        node.network->addRoom(member.get());
    }
};

PeerDispatch toPeers(const std::string& delivery = "packaged", std::vector<std::string> rooms = {}) {
    PeerDispatch dispatch;
    dispatch.distribute = "peers";
    dispatch.delivery = delivery;
    dispatch.rooms = std::move(rooms);
    return dispatch;
}

UsdStageRefPtr shotStage(const std::string& file = {}) {
    UsdStageRefPtr stage = file.empty() ? UsdStage::CreateInMemory("network_shot.usda") : UsdStage::CreateNew(file);
    stage->SetStartTimeCode(1);
    stage->SetEndTimeCode(2);
    UsdGeomSphere::Define(stage, SdfPath("/World/Ball"));
    UsdGeomCamera camera = UsdGeomCamera::Define(stage, SdfPath("/World/Cam"));
    UsdGeomXformCommonAPI(camera).SetTranslate(GfVec3d(0, 0, 10));
    return stage;
}

SdfPath settingsFor(const UsdStageRefPtr& stage, const QString& output, GfVec2i tiles) {
    const SdfPath path = createRenderSettings(stage);
    const UsdPrim prim = stage->GetPrimAtPath(path);
    UsdRenderSettings(prim).GetResolutionAttr().Set(GfVec2i(32, 16));
    setJobOption(prim, jobattr::renderer, VtValue(std::string("test.network")));
    setJobOption(prim, jobattr::tiles, VtValue(tiles));
    setJobOption(prim, jobattr::output, VtValue((output + "/<stage>").toStdString()));
    prim.CreateAttribute(TfToken("network:gain"), SdfValueTypeNames->Float, true).Set(2.5f);
    return path;
}

bool gradientOk(const QImage& image, int width, int height, float frame) {
    if (image.size() != QSize(width, height)) return false;
    for (int y = 0; y < height; ++y) {
        const float* row = reinterpret_cast<const float*>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            if (std::abs(row[x * 4] - float(x) / width) > 1e-5f || std::abs(row[x * 4 + 1] - float(y) / height) > 1e-5f ||
                row[x * 4 + 2] != frame) {
                std::printf("    pixel %d,%d: %f %f %f\n", x, y, row[x * 4], row[x * 4 + 1], row[x * 4 + 2]);
                return false;
            }
        }
    }
    return true;
}

struct Registered {
    std::shared_ptr<NetworkGradient> renderer = std::make_shared<NetworkGradient>();
    Registered() { FinalRenderers::instance().add(renderer); }
    ~Registered() { FinalRenderers::instance().remove(renderer->id()); }
};

}  // namespace

USDCC_TEST(render_network_wire_formats) {
    RenderTask task;
    task.jobId = "job-7";
    task.index = 3;
    task.frame = 12.5;
    task.resolution = GfVec2i(640, 360);
    task.pixelAspect = 1.5f;
    task.camera = SdfPath("/Cam");
    task.renderSettings = SdfPath("/Render/Settings");
    task.region = {10, 20, 30, 40};
    task.aovs = {{"color", "color", TfToken("color4f")}, {"depth", "depth", TfToken("float")}};
    task.settings[TfToken("a")] = VtValue(true);
    task.settings[TfToken("b")] = VtValue(7);
    task.settings[TfToken("c")] = VtValue(0.25f);
    task.settings[TfToken("d")] = VtValue(TfToken("PathTracing"));
    task.settings[TfToken("e")] = VtValue(GfVec3f(1, 2, 3));
    task.settings[TfToken("f")] = VtValue(std::string("text"));
    task.includedPurposes = {TfToken("default"), TfToken("render")};
    task.timeLimit = 9;
    const RenderTask back = taskFromJson(taskToJson(task), nullptr);
    CHECK(back.jobId == "job-7" && back.index == 3 && back.frame == 12.5 && back.resolution == GfVec2i(640, 360) &&
              back.pixelAspect == 1.5f,
          "the frame and image");
    CHECK(back.camera == task.camera && back.renderSettings == task.renderSettings && back.region == task.region, "the camera, settings, region");
    CHECK(back.aovs.size() == 2 && back.aovs[1].name == "depth" && back.aovs[1].dataType == TfToken("float"), "the AOVs");
    CHECK(back.settings == task.settings, "every setting, in its type");
    CHECK(back.includedPurposes == task.includedPurposes && back.timeLimit == 9, "purposes and time limit");

    FloatImage image(3, 2);
    for (size_t i = 0; i < image.rgba.size(); ++i) image.rgba[i] = float(i) * 0.5f;
    const std::optional<FloatImage> unpacked = unpackImage(packImage(image));
    CHECK(unpacked && unpacked->width == 3 && unpacked->height == 2 && unpacked->rgba == image.rgba, "an image there and back");
    CHECK(!unpackImage(QByteArray("rubbish")), "rubbish isn't an image");
}

USDCC_TEST(render_network_bundle_and_shared) {
    QTemporaryDir dir;
    // A texture beside the stage, used by a shader: the bundle carries it.
    QFile texture(dir.filePath("tex/wood.png"));
    QDir().mkpath(dir.filePath("tex"));
    texture.open(QIODevice::WriteOnly);
    texture.write("not really a png");
    texture.close();
    const std::string file = dir.filePath("scene.usda").toStdString();
    UsdStageRefPtr stage = shotStage(file);
    stage->GetPrimAtPath(SdfPath("/World/Ball"))
        .CreateAttribute(TfToken("info:texture"), SdfValueTypeNames->Asset, true)
        .Set(SdfAssetPath("./tex/wood.png"));
    stage->Save();

    UsdStageRefPtr snapshot = UsdStage::Open(stage->Flatten());
    const QString bundle = dir.filePath("bundle");
    QString error;
    CHECK(writeSceneBundle(snapshot, bundle, &error), "a bundle: %s", qPrintable(error));
    CHECK(QFileInfo::exists(bundle + "/scene.usdc"), "with the scene");
    bool hasTexture = false;
    for (const QFileInfo& f : QDir(bundle).entryInfoList(QDir::Files | QDir::NoDotAndDotDot) + QDir(bundle + "/tex").entryInfoList(QDir::Files)) {
        hasTexture = hasTexture || f.fileName() == "wood.png";
    }
    if (!hasTexture) {
        for (const QString& sub : QDir(bundle).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            for (const QFileInfo& f : QDir(bundle + "/" + sub).entryInfoList(QDir::Files)) hasTexture = hasTexture || f.fileName() == "wood.png";
        }
    }
    CHECK(hasTexture, "and the texture it uses");

    const SharedScene shared = captureSharedScene(stage);
    CHECK(shared.whyNot.empty() && shared.rootLayer == stage->GetRootLayer()->GetIdentifier(), "a saved stage can be shared: %s",
          shared.whyNot.c_str());
    const bool listsTexture = std::any_of(shared.dependencies.begin(), shared.dependencies.end(),
                                          [](const SharedScene::Dependency& d) { return d.path.find("wood.png") != std::string::npos; });
    CHECK(listsTexture, "its dependencies have the texture");
    CHECK(sharedSceneProblems(SharedScene::fromJson(shared.toJson())).isEmpty(), "and everything is here");
    SharedScene moved = shared;
    for (auto& d : moved.dependencies) {
        if (d.path.find("wood.png") != std::string::npos) d.size += 1;
    }
    moved.dependencies.push_back({"C:/nowhere/at/all.usd", 10});
    moved.dependencies.push_back({"nosuchscheme://server/a.usd", -1});
    const QStringList problems = sharedSceneProblems(moved);
    CHECK(problems.size() == 3, "a different file, a missing one and an unknown scheme: %s", qPrintable(problems.join("; ")));
    CHECK(!captureSharedScene(shotStage()).whyNot.empty(), "an unsaved new stage can't be shared");

    // Opened on a peer: here, the same layer, so it must not be dirty.
    error.clear();
    UsdStageRefPtr opened = openSharedScene(shared, &error);
    CHECK(opened && opened->GetPrimAtPath(SdfPath("/World/Ball")), "opened from its paths: %s", qPrintable(error));
    stage->GetPrimAtPath(SdfPath("/World/Ball")).SetActive(false);
    const SharedScene dirty = captureSharedScene(stage);
    CHECK(dirty.dirtyLayers.size() == 1, "an unsaved edit travels as the layer's text");
    CHECK(!openSharedScene(dirty, &error) && error.contains("open here already"), "but a layer open here isn't overwritten: %s",
          qPrintable(error));
}

USDCC_TEST(render_network_packaged) {
    Registered registered;
    TwoInstances farm;
    if (!farm.member) return;
    CHECK(waitFor([&] { return farm.submitter.scheduler.workers().size() == 1; }), "the node becomes the submitter's worker");
    CHECK(farm.submitter.network->nodesFor("test.network", true).size() == 1, "and says it renders with the test renderer, tiles too");
    CHECK(farm.node.scheduler.workers().empty(), "the submitter isn't a node, so not the node's worker");

    QTemporaryDir out;
    UsdStageRefPtr stage = shotStage();
    settingsFor(stage, out.path(), GfVec2i(2, 2));
    JobRequest request;
    request.stage = stage;
    request.frames = JobRequest::Frames::Range;
    request.dispatch = toPeers();
    std::string error;
    RenderJobPtr job = makeRenderJob(request, &error);
    CHECK(job && job->tasks.size() == 8, "2 frames × 4 tiles: %s", error.c_str());
    if (!job) return;
    std::vector<QImage> frames;
    QObject::connect(&farm.submitter.scheduler, &RenderScheduler::frameRendered,
                     [&frames](const QString&, int, const QImage& image) { frames.push_back(image); });
    farm.submitter.scheduler.submit(job);
    CHECK(waitFor([&] { return ended(job); }, 30000) && job->status == RenderJob::Status::Done, "rendered by the node: %s",
          job->error.c_str());
    CHECK(std::all_of(job->tasks.begin(), job->tasks.end(), [](const JobTask& t) { return t.worker == "Node"; }), "every task by the node");
    CHECK(registered.renderer->started == 8 && registered.renderer->gain == 2.5f, "with the prim's settings");
    CHECK(registered.renderer->lastStage && registered.renderer->lastStage->GetPrimAtPath(SdfPath("/World/Ball")),
          "from the scene it was sent");
    CHECK(frames.size() == 2 && gradientOk(frames[0], 32, 16, 1.0f) && gradientOk(frames[1], 32, 16, 2.0f),
          "the tiles came back and went where they belong");
    CHECK(farm.node.asked == 0, "a trusted peer's job starts without asking");
}

USDCC_TEST(render_network_shared) {
    Registered registered;
    TwoInstances farm;
    if (!farm.member) return;
    waitFor([&] { return farm.submitter.scheduler.workers().size() == 1; });
    QTemporaryDir out;
    const std::string file = out.filePath("shared_shot.usda").toStdString();
    UsdStageRefPtr stage = shotStage(file);
    settingsFor(stage, out.path(), GfVec2i(1, 1));
    stage->Save();
    JobRequest request;
    request.stage = stage;
    request.currentTime = 1;
    request.dispatch = toPeers("shared");
    RenderJobPtr job = makeRenderJob(request, nullptr);
    if (!job) return;
    CHECK(job->shared.whyNot.empty(), "the job has the stage as it is");
    farm.submitter.scheduler.submit(job);
    CHECK(waitFor([&] { return ended(job); }, 30000) && job->status == RenderJob::Status::Done, "rendered: %s", job->error.c_str());
    const QString inbox = farm.node.folder.filePath("inbox");
    const QStringList bundles = QDir(inbox).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    CHECK(bundles.isEmpty(), "shared: no scene was sent: %s", qPrintable(bundles.join(",")));

    // An unsaved edit: the layer is open on the "peer" too (it's this process), so it gets the bundle.
    stage->DefinePrim(SdfPath("/World/Added"));
    RenderJobPtr dirty = makeRenderJob(request, nullptr);
    if (!dirty) return;
    farm.submitter.scheduler.submit(dirty);
    CHECK(waitFor([&] { return ended(dirty); }, 30000) && dirty->status == RenderJob::Status::Done, "falls back to packaged: %s",
          dirty->error.c_str());
    CHECK(registered.renderer->lastStage && registered.renderer->lastStage->GetPrimAtPath(SdfPath("/World/Added")).IsValid(),
          "with the unsaved edit in it");
}

USDCC_TEST(render_network_trust_and_leaving) {
    Registered registered;
    TwoInstances farm;
    if (!farm.member) return;
    waitFor([&] { return farm.submitter.scheduler.workers().size() == 1; });
    QTemporaryDir out;
    UsdStageRefPtr stage = shotStage();
    settingsFor(stage, out.path(), GfVec2i(1, 1));
    JobRequest request;
    request.stage = stage;
    request.currentTime = 1;
    request.dispatch = toPeers();

    // Not trusted, and its user says no: the job waits for another node.
    farm.node.trusted = false;
    farm.node.answer = [] { return false; };
    RenderJobPtr declined = makeRenderJob(request, nullptr);
    farm.submitter.scheduler.submit(declined);
    CHECK(waitFor([&] { return farm.node.asked == 1; }), "the node asks");
    waitFor([&] { return declined->tasks.front().status == JobTask::Status::Pending && declined->tasks.front().attempts == 0; }, 3000);
    CHECK(declined->tasks.front().status == JobTask::Status::Pending && declined->tasks.front().attempts == 0,
          "declined: the task waits again, and that wasn't a try");
    CHECK(!farm.submitter.scheduler.workers().front()->canRun(*declined), "that node is out for this job");
    farm.submitter.scheduler.cancel(declined->id);

    // Its user says yes.
    farm.node.answer = [] { return true; };
    RenderJobPtr accepted = makeRenderJob(request, nullptr);
    farm.submitter.scheduler.submit(accepted);
    CHECK(waitFor([&] { return ended(accepted); }) && accepted->status == RenderJob::Status::Done, "asked, and rendered: %s",
          accepted->error.c_str());

    // Not taking jobs in the room any more: it says so, and stops being a worker.
    farm.node.network->setTakesJobs(farm.member.get(), false);
    CHECK(waitFor([&] { return farm.submitter.scheduler.workers().empty(); }), "a peer that stops rendering for others goes from the queue");
    farm.node.network->setTakesJobs(farm.member.get(), true);
    CHECK(waitFor([&] { return farm.submitter.scheduler.workers().size() == 1; }), "and comes back");

    // Leaving mid-task: the task goes back to the queue.
    farm.node.trusted = true;
    registered.renderer->hang = true;
    RenderJobPtr left = makeRenderJob(request, nullptr);
    farm.submitter.scheduler.submit(left);
    CHECK(waitFor([&] { return farm.node.network->remoteTasks() == 1; }), "the node is rendering it");
    farm.member->close();
    CHECK(waitFor([&] { return left->tasks.front().status == JobTask::Status::Pending; }), "the node left: the task waits again");
    CHECK(waitFor([&] { return farm.submitter.scheduler.workers().empty(); }), "and the node is no worker");
    farm.submitter.scheduler.cancel(left->id);
}

USDCC_TEST(render_network_rooms) {
    Registered registered;
    TwoInstances farm;
    if (!farm.member) return;
    CHECK(waitFor([&] { return farm.submitter.scheduler.workers().size() == 1; }), "the node is a worker");

    // A chat room the two share: not the render network's.
    RoomPair chat(QStringLiteral("Chat"), farm.submitterProfile, farm.nodeProfile, RoomType::Chat);
    if (!chat.member) return;
    farm.submitter.network->addRoom(chat.host.get());
    farm.node.network->addRoom(chat.member.get());
    CHECK(farm.submitter.network->rooms().size() == 1 && farm.node.network->rooms().size() == 1, "chat rooms are ignored");

    // A second render queue with the same two: the node is still one worker.
    RoomPair second(QStringLiteral("Night shift"), farm.submitterProfile, farm.nodeProfile);
    if (!second.member) return;
    farm.submitter.network->addRoom(second.host.get());
    farm.node.network->addRoom(second.member.get());
    CHECK(waitFor([&] { return farm.submitter.network->nodes().size() == 2; }), "the node shows in both rooms");
    CHECK(farm.submitter.scheduler.workers().size() == 1, "but is one worker");
    CHECK(RenderNetwork::roomKey(farm.host.get()) == RenderNetwork::roomKey(farm.member.get()),
          "a room's key is the same for its host and its members");
    CHECK(farm.submitter.network->nodesFor("test.network", false, {RenderNetwork::roomKey(second.host.get())}).size() == 1,
          "nodes by room");

    // A room the node doesn't take jobs in: a job only for that room has nobody.
    farm.node.network->setTakesJobs(second.member.get(), false);
    CHECK(waitFor([&] {
              return farm.submitter.network->nodesFor("test.network", false, {RenderNetwork::roomKey(second.host.get())}).empty();
          }),
          "it says it doesn't take jobs there");
    CHECK(farm.submitter.scheduler.workers().size() == 1, "it is still a worker, through the other room");
    QTemporaryDir out;
    UsdStageRefPtr stage = shotStage();
    settingsFor(stage, out.path(), GfVec2i(1, 1));
    JobRequest request;
    request.stage = stage;
    request.currentTime = 1;
    request.dispatch = toPeers("packaged", {RenderNetwork::roomKey(second.host.get())});
    RenderJobPtr elsewhere = makeRenderJob(request, nullptr);
    CHECK(elsewhere && elsewhere->spec.options.rooms.size() == 1, "the job keeps its rooms");
    if (!elsewhere) return;
    CHECK(!farm.submitter.scheduler.workers().front()->canRun(*elsewhere), "and the node can't take it");

    // The same job for the first room: rendered there, and the node says what it renders.
    registered.renderer->hang = true;
    request.dispatch = toPeers("packaged", {RenderNetwork::roomKey(farm.host.get())});
    RenderJobPtr here = makeRenderJob(request, nullptr);
    if (!here) return;
    farm.submitter.scheduler.submit(here);
    CHECK(waitFor([&] { return farm.node.network->remoteTasks() == 1; }), "the node is rendering it");
    const auto current = [&]() -> std::optional<RenderNode::Current> {
        for (const RenderNode& node : farm.submitter.network->nodes()) {
            if (node.room == farm.host.get() && node.current) return node.current;
        }
        return std::nullopt;
    };
    CHECK(waitFor([&] { return current().has_value(); }), "and says so");
    if (const auto c = current()) {
        CHECK(c->forPeer && c->from == QStringLiteral("Submitter") && c->scene == QStringLiteral("network_shot") &&
                  c->startedMs > 0 && c->frame == 1.0,
              "for whom, which scene and frame, since when: %s %s %s", qPrintable(c->from), qPrintable(c->scene),
              qPrintable(c->title));
    }
    const RenderNode self = farm.node.network->self(farm.member.get());
    CHECK(self.busy && self.current && self.node, "its own row says the same");
    farm.submitter.scheduler.cancel(here->id);
    CHECK(waitFor([&] { return farm.node.network->remoteTasks() == 0 && !farm.node.network->self(farm.member.get()).busy; }),
          "and idle again once cancelled");
}
