// The render queue server (docs/RENDER_NODE.md Â§4.4, Â§4.5) in one process: a queue hosting a
// render queue room, a render node and artists in it, with a test renderer. Submitting and
// refusing, the frames under the output root, a restart that keeps what was done, frames
// fetched by the artist, the jobs in the Render Queue panel with who may cancel them, the
// queue's web page and its token, and the wire formats.
#include "test_harness.h"

#include "peers_test_support.h"
#include "queue_web.h"

#include "usdcc/peers/room.h"
#include "usdcc/usd/linear_exr.h"
#include "usdcc/usd/render/render_job.h"
#include "usdcc/usd/render/render_network.h"
#include "usdcc/usd/render/render_queue.h"
#include "usdcc/usd/render/render_queue_panel.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render/render_spec.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdRender/settings.h>

#include <QApplication>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <map>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::render;
using usdcc::peers::Peer;
using usdcc::peers::Room;
using usdcc::peers::RoomType;
using usdcc::peers::test::RawMember;
using usdcc::peers::test::waitUntil;

namespace {

// R = x / width, G = y / height, B = the frame. Hangs for the tasks `hang` says.
class QueueGradient : public FinalRenderer {
public:
    std::string id() const override { return "test.queue"; }
    std::string name() const override { return "Queue Gradient"; }
    Capabilities capabilities() const override { return {true, true}; }
    std::vector<std::string> namespaces() const override { return {"queue"}; }
    std::vector<RenderSettingField> settingsSchema() const override { return {}; }
    struct Handle : RenderTaskHandle {
        void cancel() override {}
    };
    std::unique_ptr<RenderTaskHandle> start(const RenderTask& task, TaskCallbacks callbacks) override {
        ++started[{task.frame, task.region.x * 10000 + task.region.y}];
        if (hang && hang(task)) return std::make_unique<Handle>();
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
        callbacks.finished(std::move(images));
        return std::make_unique<Handle>();
    }
    int startedFor(double frame) const {
        int count = 0;
        for (const auto& [key, n] : started) count += key.first == frame ? n : 0;
        return count;
    }
    int total() const {
        int count = 0;
        for (const auto& [key, n] : started) count += n;
        return count;
    }
    std::map<std::pair<double, int>, int> started;
    std::function<bool(const RenderTask&)> hang;
};

struct Registered {
    std::shared_ptr<QueueGradient> renderer = std::make_shared<QueueGradient>();
    Registered() { FinalRenderers::instance().add(renderer); }
    ~Registered() { FinalRenderers::instance().remove(renderer->id()); }
};

usdcc::peers::Profile profile(const QString& name) {
    usdcc::peers::Profile p;
    p.name = name;
    p.identity = usdcc::peers::Identity::generate();
    p.instance = name.toLower();
    return p;
}

std::unique_ptr<Room> join(Room& host, const usdcc::peers::Profile& who) {
    usdcc::peers::JoinOptions options;
    options.profile = who;
    options.type = RoomType::RenderQueue;
    options.secret = host.secret();
    std::unique_ptr<Room> room(Room::join(QStringLiteral("127.0.0.1:%1").arg(host.port()), options));
    waitUntil([&] { return room->state() == Room::State::Open; }, 10000);
    return room;
}

// The queue: its room, its queue, its network, the server.
struct Queue {
    usdcc::peers::Profile who = profile(QStringLiteral("Farm queue"));
    std::unique_ptr<Room> room;
    std::unique_ptr<RenderScheduler> scheduler;
    std::unique_ptr<RenderNetwork> network;
    std::unique_ptr<RenderQueueServer> server;
    QTemporaryDir networkFolder;

    void start(const QString& state, const QString& root, const QStringList& accepted, const QStringList& admins = {}) {
        usdcc::peers::HostOptions host;
        host.name = QStringLiteral("Farm");
        host.type = RoomType::RenderQueue;
        host.profile = who;
        QString error;
        room.reset(Room::host(host, nullptr, &error));
        CHECK(room != nullptr, "the queue's room: %s", qPrintable(error));
        scheduler = std::make_unique<RenderScheduler>();
        RenderNetworkOptions options;
        options.folder = networkFolder.path();
        options.scheduler = scheduler.get();
        options.isNode = [] { return false; };
        network = std::make_unique<RenderNetwork>(std::move(options));
        RenderQueueOptions queue;
        queue.name = QStringLiteral("Farm queue");
        queue.stateFolder = state;
        queue.outputRoot = root;
        queue.scheduler = scheduler.get();
        queue.accepts = [accepted](const Peer& p) { return accepted.contains(p.fingerprint); };
        queue.isAdmin = [admins](const Peer& p) { return admins.contains(p.fingerprint); };
        server = std::make_unique<RenderQueueServer>(std::move(queue));
        server->restore();
        if (!room) return;
        network->addRoom(room.get());
        server->addRoom(room.get());
    }
    void stop() {
        server.reset();
        network.reset();
        scheduler.reset();
        room.reset();
    }
};

// A render node that trusts the queue.
struct Node {
    usdcc::peers::Profile who = profile(QStringLiteral("Node"));
    std::unique_ptr<Room> room;
    RenderScheduler scheduler;
    QTemporaryDir folder;
    std::unique_ptr<RenderNetwork> network;

    Node(Room& host, const QString& queueFingerprint) {
        room = join(host, who);
        RenderNetworkOptions options;
        options.folder = folder.path();
        options.scheduler = &scheduler;
        options.isNode = [] { return true; };
        options.trusted = [queueFingerprint](const Peer& p) { return p.fingerprint == queueFingerprint; };
        network = std::make_unique<RenderNetwork>(std::move(options));
        network->addRoom(room.get());
    }
};

// An artist's instance: its room and its client.
struct Artist {
    usdcc::peers::Profile who;
    std::unique_ptr<Room> room;
    QTemporaryDir folder;
    std::unique_ptr<RenderQueueClient> client;
    QStringList queued, refused, denied;

    Artist(Room& host, const QString& name) : who(profile(name)) {
        room = join(host, who);
        RenderQueueClient::Options options;
        options.folder = folder.path();
        client = std::make_unique<RenderQueueClient>(options);
        client->addRoom(room.get());
        QObject::connect(client.get(), &RenderQueueClient::queued, client.get(),
                         [this](const QString&, const QString& id, int) { queued.append(id); });
        QObject::connect(client.get(), &RenderQueueClient::refused, client.get(),
                         [this](const QString&, const QString& reason) { refused.append(reason); });
        QObject::connect(client.get(), &RenderQueueClient::denied, client.get(),
                         [this](const QString&, const QString& reason) { denied.append(reason); });
    }
};

// A job of the test renderer: frames 1 and 2, 2 Ã— 2 tiles, 32 Ã— 16, to `output`.
RenderJobPtr shotJob(const QString& output) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory("queue_shot.usda");
    stage->SetStartTimeCode(1);
    stage->SetEndTimeCode(2);
    UsdGeomSphere::Define(stage, SdfPath("/World/Ball"));
    UsdGeomCamera camera = UsdGeomCamera::Define(stage, SdfPath("/World/Cam"));
    UsdGeomXformCommonAPI(camera).SetTranslate(GfVec3d(0, 0, 10));
    const SdfPath settings = createRenderSettings(stage);
    const UsdPrim prim = stage->GetPrimAtPath(settings);
    UsdRenderSettings(prim).GetResolutionAttr().Set(GfVec2i(32, 16));
    setJobOption(prim, jobattr::renderer, VtValue(std::string("test.queue")));
    setJobOption(prim, jobattr::tiles, VtValue(GfVec2i(2, 2)));
    setJobOption(prim, jobattr::output, VtValue(output.toStdString()));
    JobRequest request;
    request.stage = stage;
    request.settings = settings;
    request.frames = JobRequest::Frames::Range;
    PeerDispatch dispatch;
    dispatch.distribute = "queue";
    request.dispatch = dispatch;
    std::string error;
    RenderJobPtr job = makeRenderJob(request, &error);
    CHECK(job != nullptr, "a job: %s", error.c_str());
    return job;
}

bool ended(const RenderJobPtr& job) {
    return job && (job->status == RenderJob::Status::Done || job->status == RenderJob::Status::Failed ||
                   job->status == RenderJob::Status::Cancelled);
}

}  // namespace

USDCC_TEST(render_queue_wire_formats) {
    Registered registered;
    RenderJobPtr job = shotJob(QStringLiteral("<stage>"));
    if (!job) return;
    job->tasks[1].status = JobTask::Status::Done;
    job->tasks[1].attempts = 2;
    job->files = {"a.exr"};
    const RenderJobPtr back = jobFromJson(jobToJson(*job, true), job->tasks.front().task.stage);
    CHECK(back && back->tasks.size() == 8 && back->frames == job->frames, "8 tasks of 2 frames back");
    if (!back) return;
    CHECK(back->tasks[1].status == JobTask::Status::Done && back->tasks[1].attempts == 2 && back->files == job->files,
          "with where they stand");
    CHECK(back->tasks[3].core == job->tasks[3].core && back->tasks[3].task.region == job->tasks[3].task.region &&
              back->tasks[3].frameIndex == job->tasks[3].frameIndex,
          "tiles where they were");
    CHECK(back->spec.resolution == GfVec2i(32, 16) && back->spec.options.tilesX == 2 && back->spec.camera == job->spec.camera &&
              back->spec.products.size() == 1 && back->spec.products.front().aovs.size() == job->spec.products.front().aovs.size(),
          "the spec");

    std::string error;
    // Absolute on every platform (C:/ isn't on Linux).
    const QString root = QDir::rootPath() + QStringLiteral("farm/renders");
    const QString keep = QDir::rootPath() + QStringLiteral("state/frames/q1");
    const std::string mine = (QDir::rootPath() + QStringLiteral("mine/<stage>")).toStdString();
    const std::string rootText = root.toStdString();
    auto pattern = queueOutputPattern(*job, root, keep, &error);
    CHECK(pattern && *pattern == rootText + "/<stage>", "relative: under the root (%s)", pattern ? pattern->c_str() : error.c_str());
    job->spec.options.output = rootText + "/shots/<stage>";
    pattern = queueOutputPattern(*job, root, {}, &error);
    CHECK(pattern && *pattern == rootText + "/shots/<stage>", "absolute inside the root");
    job->spec.options.output = mine;
    CHECK(!queueOutputPattern(*job, root, {}, &error) && error.find("root") != std::string::npos, "absolute outside: refused");
    job->spec.options.output = "../escape/<stage>";
    CHECK(!queueOutputPattern(*job, root, {}, &error), "out of the root with ..: refused");
    job->spec.options.output = mine;
    pattern = queueOutputPattern(*job, {}, keep, &error);
    CHECK(pattern && *pattern == keep.toStdString() + "/<stage>", "no root: the file name, kept for the artist");
    job->spec.options.output.clear();
    pattern = queueOutputPattern(*job, root, {}, &error);
    CHECK(pattern && *pattern == rootText + "/<stage>_<settings>", "none authored: the default name");
}


// Phase 6 in one process: the artist submits and leaves; the node renders; the frames are
// under the root. An artist the queue doesn't accept is refused.
USDCC_TEST(render_queue_submit) {
    Registered registered;
    QTemporaryDir state, root;
    usdcc::peers::Profile artistWho = profile(QStringLiteral("Anna"));
    Queue queue;
    queue.start(state.path(), root.path(), {artistWho.identity.fingerprint()});
    if (!queue.room) return;
    Node node(*queue.room, queue.who.identity.fingerprint());
    auto artist = std::make_unique<Artist>(*queue.room, QStringLiteral("Anna"));
    artist->who = artistWho;  // the accepted one: rejoin with it
    artist->room = join(*queue.room, artistWho);
    artist->client = std::make_unique<RenderQueueClient>(RenderQueueClient::Options{artist->folder.path()});
    artist->client->addRoom(artist->room.get());
    QStringList queued;
    QObject::connect(artist->client.get(), &RenderQueueClient::queued, artist->client.get(),
                     [&queued](const QString&, const QString& id, int) { queued.append(id); });
    CHECK(waitUntil([&] { return artist->client->queues().size() == 1; }, 10000), "the artist sees the queue");
    CHECK(waitUntil([&] { return queue.scheduler->workers().size() == 1; }, 10000), "the node is the queue's worker");
    if (artist->client->queues().empty()) return;
    CHECK(artist->client->queues().front().name == QStringLiteral("Farm queue") && artist->client->queues().front().outputRoot,
          "by its name, writing to a shared path");

    RenderJobPtr job = shotJob(QStringLiteral("shots/<stage>"));
    if (!job) return;
    QString error;
    CHECK(artist->client->submit(job, QStringLiteral("Farm queue"), &error), "sent: %s", qPrintable(error));
    CHECK(waitUntil([&] { return !queued.isEmpty(); }, 15000), "queued");
    if (queued.isEmpty()) return;
    const QString id = queued.front();
    // The artist goes: the job is the queue's.
    artist.reset();
    const RenderJobPtr theirs = queue.server->job(id);
    CHECK(theirs && waitUntil([&] { return ended(theirs); }, 30000) && theirs->status == RenderJob::Status::Done,
          "rendered after the artist left: %s", theirs ? theirs->error.c_str() : "no job");
    if (!theirs) return;
    CHECK(std::all_of(theirs->tasks.begin(), theirs->tasks.end(), [](const JobTask& t) { return t.worker == "Node"; }),
          "every task by the node");
    const QString frame1 = root.filePath(QStringLiteral("shots/queue_shot.0001.exr"));
    const QString frame2 = root.filePath(QStringLiteral("shots/queue_shot.0002.exr"));
    if (usdcc::usd::linearExrAvailable()) {
        CHECK(QFileInfo::exists(frame1) && QFileInfo::exists(frame2), "the frames under the root: %s",
              theirs->files.empty() ? "none" : theirs->files.front().c_str());
    }
    CHECK(registered.renderer->total() == 8, "8 tasks rendered once each: %d", registered.renderer->total());

    // Not accepted.
    Artist stranger(*queue.room, QStringLiteral("Stranger"));
    waitUntil([&] { return stranger.client->queues().size() == 1; }, 10000);
    RenderJobPtr other = shotJob(QStringLiteral("<stage>"));
    if (!other) return;
    stranger.client->submit(other, {}, &error);
    CHECK(waitUntil([&] { return !stranger.refused.isEmpty(); }, 10000) && stranger.refused.front().contains("doesn't take"),
          "refused: %s", stranger.refused.isEmpty() ? "nothing" : qPrintable(stranger.refused.front()));
    CHECK(queue.server->jobs().size() == 1, "and not queued");
}

// Phase 7: the queue stops mid-job and starts again. Frame 1 was written, two tiles of frame
// 2 were done: neither renders again; the other two tiles do, and the job finishes.
USDCC_TEST(render_queue_restart) {
    Registered registered;
    QTemporaryDir state, root;
    usdcc::peers::Profile artistWho = profile(QStringLiteral("Anna"));
    // Frame 2's last two tiles hang the first time round.
    registered.renderer->hang = [](const RenderTask& task) { return task.frame == 2.0 && task.index % 4 >= 2; };
    auto queue = std::make_unique<Queue>();
    queue->start(state.path(), root.path(), {artistWho.identity.fingerprint()});
    if (!queue->room) return;
    auto node = std::make_unique<Node>(*queue->room, queue->who.identity.fingerprint());
    std::unique_ptr<Room> artistRoom = join(*queue->room, artistWho);
    QTemporaryDir artistFolder;
    RenderQueueClient client(RenderQueueClient::Options{artistFolder.path()});
    client.addRoom(artistRoom.get());
    QString id;
    QObject::connect(&client, &RenderQueueClient::queued, &client, [&id](const QString&, const QString& queued, int) { id = queued; });
    waitUntil([&] { return client.queues().size() == 1 && queue->scheduler->workers().size() == 1; }, 10000);
    RenderJobPtr job = shotJob(QStringLiteral("<stage>"));
    if (!job) return;
    client.submit(job, {});
    CHECK(waitUntil([&] { return !id.isEmpty(); }, 15000), "queued");
    const RenderJobPtr theirs = queue->server->job(id);
    if (!theirs) return;
    CHECK(waitUntil([&] { return theirs->doneTasks() == 6; }, 20000), "frame 1 and half of frame 2 done: %d", theirs->doneTasks());
    QTest::qWait(800);  // saved
    const int frame1Before = registered.renderer->startedFor(1.0);
    const int frame2Before = registered.renderer->startedFor(2.0);
    const QString frame1 = root.filePath(QStringLiteral("queue_shot.0001.exr"));
    const QDateTime written = QFileInfo(frame1).lastModified();

    // Stopped, and started again from its state.
    artistRoom.reset();
    node.reset();
    queue->stop();
    registered.renderer->hang = nullptr;
    queue = std::make_unique<Queue>();
    queue->start(state.path(), root.path(), {artistWho.identity.fingerprint()});
    const RenderJobPtr restored = queue->server->job(id);
    CHECK(restored != nullptr && restored->doneTasks() == 6, "restored with 6 tasks done: %d", restored ? restored->doneTasks() : -1);
    if (!restored || !queue->room) return;
    node = std::make_unique<Node>(*queue->room, queue->who.identity.fingerprint());
    CHECK(waitUntil([&] { return ended(restored); }, 30000) && restored->status == RenderJob::Status::Done, "finishes: %s",
          restored->error.c_str());
    CHECK(registered.renderer->startedFor(1.0) == frame1Before, "frame 1 isn't rendered again");
    CHECK(registered.renderer->startedFor(2.0) == frame2Before + 2, "only frame 2's two missing tiles: %d more",
          registered.renderer->startedFor(2.0) - frame2Before);
    if (usdcc::usd::linearExrAvailable()) {
        CHECK(QFileInfo(frame1).lastModified() == written, "frame 1's file as it was");
        CHECK(QFileInfo::exists(root.filePath(QStringLiteral("queue_shot.0002.exr"))), "frame 2 written");
    }
    CHECK(QDir(state.filePath(QStringLiteral("tiles/") + id)).isEmpty() || !QDir(state.filePath(QStringLiteral("tiles/") + id)).exists(),
          "its tiles let go");
}

// Phase 7: without an output root, the frames come back to the artist.
USDCC_TEST(render_queue_fetch) {
    Registered registered;
    if (!usdcc::usd::linearExrAvailable()) return;
    QTemporaryDir state, mine;
    usdcc::peers::Profile artistWho = profile(QStringLiteral("Anna"));
    Queue queue;
    queue.start(state.path(), {}, {artistWho.identity.fingerprint()});
    if (!queue.room) return;
    Node node(*queue.room, queue.who.identity.fingerprint());
    std::unique_ptr<Room> artistRoom = join(*queue.room, artistWho);
    QTemporaryDir artistFolder;
    RenderQueueClient client(RenderQueueClient::Options{artistFolder.path()});
    client.addRoom(artistRoom.get());
    QStringList arrived;
    QObject::connect(&client, &RenderQueueClient::framesArrived, &client,
                     [&arrived](const QString&, const QString&, const QStringList& files) { arrived = files; });
    waitUntil([&] { return client.queues().size() == 1 && queue.scheduler->workers().size() == 1; }, 10000);
    CHECK(!client.queues().empty() && !client.queues().front().outputRoot, "a queue that keeps the frames");
    RenderJobPtr job = shotJob(QDir(mine.path()).filePath(QStringLiteral("<stage>")));
    if (!job) return;
    client.submit(job, {});
    CHECK(waitUntil([&] { return arrived.size() == 2; }, 30000), "both frames came back: %d", int(arrived.size()));
    CHECK(QFileInfo::exists(mine.filePath(QStringLiteral("queue_shot.0001.exr"))) &&
              QFileInfo::exists(mine.filePath(QStringLiteral("queue_shot.0002.exr"))),
          "where the artist wanted them");
    CHECK(waitUntil([&] { return !QDir(state.filePath(QStringLiteral("frames"))).exists() ||
                                 QDir(state.filePath(QStringLiteral("frames"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(); }),
          "and the queue let go of them");
}

// Phase 8: the Render Queue panel lists the queue's job; Cancel from it goes to the queue.
// Another artist can't cancel it; an admin can.
USDCC_TEST(render_queue_panel_and_permissions) {
    Registered registered;
    registered.renderer->hang = [](const RenderTask&) { return true; };
    QTemporaryDir state, root;
    usdcc::peers::Profile anna = profile(QStringLiteral("Anna"));
    usdcc::peers::Profile ben = profile(QStringLiteral("Ben"));
    usdcc::peers::Profile admin = profile(QStringLiteral("Admin"));
    Queue queue;
    queue.start(state.path(), root.path(), {anna.identity.fingerprint(), ben.identity.fingerprint()}, {admin.identity.fingerprint()});
    if (!queue.room) return;
    Node node(*queue.room, queue.who.identity.fingerprint());
    struct Seat {
        std::unique_ptr<Room> room;
        QTemporaryDir folder;
        std::unique_ptr<RenderQueueClient> client;
        RenderScheduler local;
        std::unique_ptr<RenderQueueSidePanel> panel;
        QStringList denied;
    };
    const auto seat = [&](const usdcc::peers::Profile& who) {
        auto s = std::make_unique<Seat>();
        s->room = join(*queue.room, who);
        s->client = std::make_unique<RenderQueueClient>(RenderQueueClient::Options{s->folder.path()});
        s->client->addRoom(s->room.get());
        Seat* raw = s.get();
        QObject::connect(s->client.get(), &RenderQueueClient::denied, s->client.get(),
                         [raw](const QString&, const QString& reason) { raw->denied.append(reason); });
        s->panel = std::make_unique<RenderQueueSidePanel>(&s->local, nullptr, s->client.get());
        return s;
    };
    auto annaSeat = seat(anna);
    auto benSeat = seat(ben);
    auto adminSeat = seat(admin);
    waitUntil([&] { return annaSeat->client->queues().size() == 1 && queue.scheduler->workers().size() == 1; }, 10000);

    RenderJobPtr job = shotJob(QStringLiteral("<stage>"));
    if (!job) return;
    annaSeat->client->submit(job, {});
    CHECK(waitUntil([&] { return queue.server->jobs().size() == 1 && queue.server->jobs().front().status == u"running"; }, 15000),
          "the queue renders it");
    if (queue.server->jobs().empty()) return;
    const QString id = queue.server->jobs().front().id;

    // In each artist's panel, under the queue's name.
    const auto queueRow = [](RenderQueueSidePanel& panel) -> QTreeWidgetItem* {
        QTreeWidget* tree = panel.tree();
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* heading = tree->topLevelItem(i);
            if (heading->text(0) == QStringLiteral("On Farm queue") && heading->childCount() == 1) return heading->child(0);
        }
        return nullptr;
    };
    CHECK(waitUntil([&] { return queueRow(*benSeat->panel) && queueRow(*benSeat->panel)->text(1) == QStringLiteral("Rendering"); }, 10000),
          "Ben's panel lists Anna's job on the queue, rendering");
    QTreeWidgetItem* row = queueRow(*benSeat->panel);
    if (!row) return;
    CHECK(row->text(0).startsWith(QStringLiteral("queue_shot")), "by its title: %s", qPrintable(row->text(0)));
    const auto clickCancel = [](RenderQueueSidePanel& panel, QTreeWidgetItem* item) {
        panel.tree()->setCurrentItem(item);
        item->setSelected(true);
        auto* cancel = panel.findChild<QPushButton*>(QStringLiteral("renderQueueCancel"));
        CHECK(cancel && cancel->isEnabled(), "Cancel is there");
        if (cancel) cancel->click();
    };
    clickCancel(*benSeat->panel, row);
    CHECK(waitUntil([&] { return !benSeat->denied.isEmpty(); }, 10000), "Ben may not cancel Anna's job");
    CHECK(queue.server->job(id)->status == RenderJob::Status::Running, "it renders on");

    CHECK(waitUntil([&] { return queueRow(*adminSeat->panel) != nullptr; }, 10000), "the admin's panel lists it");
    clickCancel(*adminSeat->panel, queueRow(*adminSeat->panel));
    CHECK(waitUntil([&] { return queue.server->job(id)->status == RenderJob::Status::Cancelled; }, 10000), "the admin cancels it");
    CHECK(waitUntil([&] { return queueRow(*annaSeat->panel) && queueRow(*annaSeat->panel)->text(1) == QStringLiteral("Cancelled"); }, 10000),
          "and Anna's panel says so");
    CHECK(adminSeat->denied.isEmpty(), "the admin wasn't told no");

    // Anna's own: Retry, then Cancel.
    annaSeat->client->retry(annaSeat->client->jobs().front().key);
    CHECK(waitUntil([&] { return queue.server->job(id)->status == RenderJob::Status::Running; }, 10000), "Anna retries her own");
    CHECK(waitUntil([&] { return queueRow(*annaSeat->panel) && queueRow(*annaSeat->panel)->text(1) == QStringLiteral("Rendering"); }, 10000),
          "her panel says it renders again");
    clickCancel(*annaSeat->panel, queueRow(*annaSeat->panel));
    CHECK(waitUntil([&] { return queue.server->job(id)->status == RenderJob::Status::Cancelled; }, 10000), "and cancels it");
}

// Phase 9: the page on the web port: the guests' link shows the jobs and their tasks as they
// change; Cancel needs the token. The page and usdcc.css are served from the folder.
USDCC_TEST(render_queue_web_page) {
    Registered registered;
    registered.renderer->hang = [](const RenderTask& task) { return task.frame == 2.0; };
    QTemporaryDir state, root, page;
    QFile::copy(QStringLiteral(USDCC_SOURCE_DIR "/src/cpp/node/web/index.html"), page.filePath(QStringLiteral("index.html")));
    QFile::copy(QStringLiteral(USDCC_SOURCE_DIR "/src/cpp/ui/resources/theme/usdcc.css"), page.filePath(QStringLiteral("usdcc.css")));
    usdcc::peers::Profile anna = profile(QStringLiteral("Anna"));
    Queue queue;
    queue.start(state.path(), root.path(), {anna.identity.fingerprint()});
    if (!queue.room) return;
    Node node(*queue.room, queue.who.identity.fingerprint());

    usdcc::node::QueueWebOptions options;
    options.name = QStringLiteral("Farm queue");
    options.address = QHostAddress(QHostAddress::LocalHost);
    options.pageDirectory = page.path();
    options.token = usdcc::node::QueueWeb::makeToken();
    options.accent = QStringLiteral("#ff8800");
    options.previewIntervalMs = 0;
    usdcc::node::QueueWeb web(queue.server.get(), queue.scheduler.get(), queue.network.get(), options);
    QString error;
    CHECK(web.start(&error), "serving: %s", qPrintable(error));
    if (!web.room()) return;
    CHECK(web.link(QStringLiteral("127.0.0.1")).toString().contains(QStringLiteral("#s=")), "a guests' link");

    // The page and the stylesheet, over HTTP.
    QNetworkAccessManager http;
    const auto get = [&](const QString& path) {
        QNetworkReply* reply = http.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(web.room()->port()).arg(path))));
        waitUntil([reply] { return reply->isFinished(); });
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        return body;
    };
    CHECK(get(QStringLiteral("/")).contains("org.usdcc.render/queue-web"), "the page at /");
    CHECK(get(QStringLiteral("/usdcc.css")).contains("--surface-panel"), "usdcc.css beside it");

    // A job, and a page looking at it.
    std::unique_ptr<Room> annaRoom = join(*queue.room, anna);
    QTemporaryDir annaFolder;
    RenderQueueClient client(RenderQueueClient::Options{annaFolder.path()});
    client.addRoom(annaRoom.get());
    waitUntil([&] { return client.queues().size() == 1 && queue.scheduler->workers().size() == 1; }, 10000);
    RenderJobPtr job = shotJob(QStringLiteral("<stage>"));
    if (!job) return;
    client.submit(job, {});
    CHECK(waitUntil([&] { return queue.server->jobs().size() == 1; }, 15000), "queued");
    if (queue.server->jobs().empty()) return;
    const QString id = queue.server->jobs().front().id;

    RawMember browser(QStringLiteral("http://127.0.0.1:%1").arg(web.room()->port()));
    CHECK(browser.hello(*web.room(), web.room()->guestSecret(), QStringLiteral("Producer")) && browser.find("room", "welcome"),
          "the page joins with the guests' link");
    const QString channel = QString::fromLatin1(usdcc::node::kQueueWebChannel);
    const auto latest = [&]() -> QJsonObject {
        for (auto it = browser.frames.crbegin(); it != browser.frames.crend(); ++it) {
            if (it->channel == channel && (it->type == u"snapshot" || it->type == u"state")) return it->body;
        }
        return {};
    };
    CHECK(waitUntil([&] { return !latest()["jobs"].toArray().isEmpty(); }, 10000), "it gets the queue's jobs");
    const QJsonObject first = latest();
    CHECK(first["queue"].toString() == QStringLiteral("Farm queue") && first["accent"].toString() == QStringLiteral("#ff8800") &&
              first["actions"].toBool(),
          "the queue's name, its accent, and that actions take a token");
    CHECK(waitUntil([&] {
              const QJsonArray jobs = latest()["jobs"].toArray();
              return !jobs.isEmpty() && jobs.first().toObject()["tasks"].toArray().size() == 8 &&
                     jobs.first().toObject()["done"].toInt() == 4;
          }, 15000),
          "and its 8 tasks, as they change: frame 1's done");
    CHECK(waitUntil([&] { return browser.find(channel, QStringLiteral("preview")) != nullptr; }, 10000), "a preview of frame 1");
    if (const auto* preview = browser.find(channel, QStringLiteral("preview"))) {
        CHECK(preview->bytes.startsWith("\x89PNG"), "as a PNG");
    }
    bool nodesListed = false;
    for (const QJsonValue& n : latest()["nodes"].toArray()) nodesListed = nodesListed || n.toObject()["name"].toString() == u"Node";
    CHECK(nodesListed, "and the nodes");

    // Cancel: without the token, then with it.
    const auto result = [&]() -> QJsonObject {
        for (auto it = browser.frames.crbegin(); it != browser.frames.crend(); ++it) {
            if (it->channel == channel && it->type == u"result") return it->body;
        }
        return {};
    };
    browser.sendText(channel, QStringLiteral("cancel"), {{"id", id}, {"token", QStringLiteral("guess")}});
    CHECK(waitUntil([&] { return result().contains("ok"); }, 10000) && !result()["ok"].toBool(), "refused without the token");
    CHECK(queue.server->job(id)->status == RenderJob::Status::Running, "and it renders on");
    browser.frames.clear();
    browser.sendText(channel, QStringLiteral("cancel"), {{"id", id}, {"token", options.token}});
    CHECK(waitUntil([&] { return result()["ok"].toBool(); }, 10000), "cancelled with it");
    CHECK(queue.server->job(id)->status == RenderJob::Status::Cancelled, "on the queue");
    CHECK(usdcc::node::QueueWeb::tokenMatches(QStringLiteral("abc"), QStringLiteral("abc")) &&
              !usdcc::node::QueueWeb::tokenMatches(QStringLiteral("abc"), QStringLiteral("abd")) &&
              !usdcc::node::QueueWeb::tokenMatches({}, {}),
          "tokens");
}
