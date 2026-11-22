// usdcc_node (docs/RENDER_NODE.md): its arguments and their overrides, the trust list, the key
// file and the Storm check without a process; then the program itself, as a farm manager and
// an artist's instance would use it: one render whose EXRs match the app's, its exit codes,
// and a node serving a room, declining a peer it doesn't trust and giving a task back when
// it's killed.
#include "test_harness.h"

#include "node.h"
#include "render_options.h"
#include "serve_support.h"

#include "usdcc/peers/identity.h"
#include "usdcc/peers/room.h"
#include "usdcc/usd/render/final_renderer.h"
#include "usdcc/usd/render/hydra_final_renderer.h"
#include "usdcc/usd/render/render_job.h"
#include "usdcc/usd/render/render_network.h"
#include "usdcc/usd/render/render_queue.h"
#include "usdcc/usd/linear_exr.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render/render_spec.h"

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdRender/settings.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>


#include <cmath>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::render;
using namespace usdcc::node;

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

// The parser usdcc_node has, on `arguments` (after the program's name).
struct Parsed {
    QCommandLineParser parser;
    bool ok = false;
    explicit Parsed(const QStringList& arguments) {
        Options::add(parser);
        ok = parser.parse(QStringList{QStringLiteral("usdcc_node")} + arguments);
    }
};

// A sphere above the middle of a camera's view, lit, over frames 1 to 3.
UsdStageRefPtr shotStage(const std::string& file = {}, bool camera = true) {
    UsdStageRefPtr stage = file.empty() ? UsdStage::CreateInMemory("node_shot.usda") : UsdStage::CreateNew(file);
    stage->SetStartTimeCode(1);
    stage->SetEndTimeCode(3);
    UsdGeomSphere ball = UsdGeomSphere::Define(stage, SdfPath("/World/Ball"));
    UsdGeomXformCommonAPI(ball).SetTranslate(GfVec3d(0, 0.6, 0));
    if (camera) {
        UsdGeomCamera cam = UsdGeomCamera::Define(stage, SdfPath("/World/Cam"));
        UsdGeomXformCommonAPI(cam).SetTranslate(GfVec3d(0, 0, 8));
    }
    UsdLuxDistantLight::Define(stage, SdfPath("/World/Sun"));
    return stage;
}

// Embree's samples per pixel, whatever this OpenUSD calls it; empty without one.
RenderSettingField embreeSamples() {
    const auto embree = FinalRenderers::instance().find("HdEmbreeRendererPlugin");
    if (!embree) return {};
    for (const RenderSettingField& field : embree->settingsSchema()) {
        const std::string& key = field.rendererKey.GetString();
        if (TfStringEndsWith(key, "convergedSamplesPerPixel") || TfStringEndsWith(key, "samplesToConvergence")) {
            return field;
        }
    }
    return {};
}

}  // namespace

// The arguments: what each reads as, and the errors a farm script sees.
USDCC_TEST(node_render_arguments) {
    CHECK(parseFrames("12") && parseFrames("12")->start == 12 && parseFrames("12")->end == 12, "one frame");
    const auto range = parseFrames("1-24x2");
    CHECK(range && range->start == 1 && range->end == 24 && range->step == 2, "a range with a step");
    CHECK(parseFrames("-5--1") && parseFrames("-5--1")->start == -5 && parseFrames("-5--1")->end == -1, "negative frames");
    CHECK(!parseFrames("24-1") && !parseFrames("a") && !parseFrames("1-2x0"), "backwards, words and a zero step aren't");
    CHECK(parseSize("1920x1080") == GfVec2i(1920, 1080) && parseSize("2X2") == GfVec2i(2, 2), "sizes");
    CHECK(!parseSize("0x2") && !parseSize("12"), "not a size");
    CHECK(parseValue("8").IsHolding<int>() && parseValue("0.5").IsHolding<double>() && parseValue("true").IsHolding<bool>() &&
              parseValue("Uniform").IsHolding<std::string>(),
          "--set values as they look");

    Parsed good({QStringLiteral("render"), QStringLiteral("shot.usda"), QStringLiteral("--frames"), QStringLiteral("1-3"),
                 QStringLiteral("--output"), QStringLiteral("out/<stage>.####.exr"), QStringLiteral("--tiles"),
                 QStringLiteral("2x2"), QStringLiteral("--set"), QStringLiteral("embree:ty:maxBounces=8")});
    RenderArguments arguments;
    // An absolute folder on every platform: C:/farm/job on Windows, /farm/job elsewhere.
    const QString farm = QDir::rootPath() + QStringLiteral("farm/job");
    const QString error = readRenderArguments(good.parser, farm, &arguments);
    CHECK(good.ok && error.isEmpty(), "read: %s", qPrintable(error));
    CHECK(arguments.output == (farm + QStringLiteral("/out/<stage>.####.exr")).toStdString(),
          "a relative output is under the working folder: %s",
          arguments.output.c_str());
    CHECK(arguments.tiles == GfVec2i(2, 2) && arguments.set.size() == 1 && arguments.set.front().first == "embree:ty:maxBounces",
          "tiles and a setting");
    for (const QStringList& bad : {QStringList{QStringLiteral("--frames"), QStringLiteral("x")},
                                   QStringList{QStringLiteral("--resolution"), QStringLiteral("big")},
                                   QStringList{QStringLiteral("--format"), QStringLiteral("tif")},
                                   QStringList{QStringLiteral("--tiles"), QStringLiteral("100x1")},
                                   QStringList{QStringLiteral("--set"), QStringLiteral("nothing")}}) {
        Parsed parsed(QStringList{QStringLiteral("render"), QStringLiteral("shot.usda")} + bad);
        RenderArguments ignored;
        CHECK(!readRenderArguments(parsed.parser, QDir::currentPath(), &ignored).isEmpty(), "%s is refused",
              qPrintable(bad.join(' ')));
    }
}

// The overrides reach the job through the session layer, never the file.
USDCC_TEST(node_render_overrides) {
    registerHydraFinalRenderers();
    if (!FinalRenderers::instance().find("HdEmbreeRendererPlugin")) {
        CHECK(false, "Embree is a final renderer");
        return;
    }
    QTemporaryDir dir;
    const std::string file = dir.filePath("overrides.usda").toStdString();
    UsdStageRefPtr stage = shotStage(file);
    stage->Save();
    std::string before;
    stage->GetRootLayer()->ExportToString(&before);

    RenderArguments arguments;
    arguments.renderer = "HdEmbreeRendererPlugin";
    arguments.frames = FrameRange{2, 3, 1};
    arguments.resolution = GfVec2i(40, 20);
    arguments.tiles = GfVec2i(2, 1);
    arguments.output = dir.filePath("out/<stage>.####.exr").toStdString();
    const RenderSettingField samples = embreeSamples();
    CHECK(!samples.attribute.empty(), "Embree has a samples setting");
    arguments.set = {{samples.attribute, "4"}};
    std::string error;
    const std::optional<JobRequest> request = prepareRenderRequest(stage, arguments, &error);
    CHECK(request.has_value(), "a request: %s", error.c_str());
    if (!request) return;
    RenderJobPtr job = makeRenderJob(*request, &error);
    CHECK(job != nullptr, "a job: %s", error.c_str());
    if (!job) return;
    CHECK(job->frames == std::vector<double>({2, 3}), "frames 2 and 3");
    CHECK(job->tasks.size() == 4 && job->spec.resolution == GfVec2i(40, 20), "2 × 1 tiles of 40 × 20");
    CHECK(job->spec.camera == SdfPath("/World/Cam"), "the stage's camera, as the settings made for it name it");
    CHECK(job->spec.options.distribute == "local", "rendered here, whatever the profile says");
    CHECK(job->tasks.front().task.settings.count(samples.rendererKey) == 1, "with the --set value");
    CHECK(outputPath(*job, job->spec.products.front(), "color", 2) == dir.filePath("out/overrides.0002.exr").toStdString(),
          "to the --output pattern: %s", outputPath(*job, job->spec.products.front(), "color", 2).c_str());
    std::string after;
    stage->GetRootLayer()->ExportToString(&after);
    CHECK(before == after && !stage->GetRootLayer()->IsDirty(), "the file's layer is untouched");
    CHECK(!stage->GetSessionLayer()->IsEmpty(), "the overrides are in the session layer");

    // What can't be used.
    UsdStageRefPtr empty = shotStage({}, false);
    RenderArguments none;
    none.renderer = "HdEmbreeRendererPlugin";
    CHECK(!prepareRenderRequest(empty, none, &error) && error.find("No camera") != std::string::npos,
          "no camera anywhere: %s", error.c_str());
    RenderArguments unknown;
    unknown.renderer = "NoSuchRenderer";
    CHECK(!prepareRenderRequest(shotStage(), unknown, &error) && error.find("NoSuchRenderer") != std::string::npos,
          "an unknown renderer: %s", error.c_str());
    RenderArguments badSet;
    badSet.renderer = "HdEmbreeRendererPlugin";
    badSet.set = {{"embree:noSuchSetting", "1"}};
    CHECK(!prepareRenderRequest(shotStage(), badSet, &error) && error.find("noSuchSetting") != std::string::npos,
          "a setting the renderer hasn't got: %s", error.c_str());
    RenderArguments badCamera;
    badCamera.renderer = "HdEmbreeRendererPlugin";
    badCamera.camera = "/World/Ball";
    CHECK(!prepareRenderRequest(shotStage(), badCamera, &error), "a camera that isn't one");
}

USDCC_TEST(node_trust_and_keys) {
    const usdcc::peers::Identity someone = usdcc::peers::Identity::generate();
    const QString shortForm = usdcc::peers::shortFingerprint(someone.fingerprint());
    TrustList trust;
    CHECK(!trust.trusts(someone.fingerprint()), "nobody to start with");
    trust.setFromArguments({shortForm.toUpper()});
    CHECK(trust.trusts(someone.fingerprint()), "by the first eight, whatever the case");
    trust.setFromArguments({});
    trust.setFileText(QStringLiteral("# the artists\n%1, aaaa-bbbb\n  # gone: cccc-dddd\n").arg(someone.fingerprint()));
    CHECK(trust.trusts(someone.fingerprint()) && trust.all().size() == 2, "a file, with comments: %s",
          qPrintable(trust.all().join(' ')));
    CHECK(!trust.trusts(usdcc::peers::Identity::generate().fingerprint()) && !trust.trusts({}), "not others");

    QTemporaryDir dir;
    const QString path = dir.filePath("keys/node.key");
    QString error;
    const usdcc::peers::Identity made = loadKeyFile(path, &error);
    CHECK(made.isValid() && QFileInfo(path).size() == 64, "made and written: %s", qPrintable(error));
    const usdcc::peers::Identity again = loadKeyFile(path, &error);
    CHECK(again.isValid() && again.fingerprint() == made.fingerprint(), "the same key next time");
    QFile junk(dir.filePath("junk.key"));
    CHECK(junk.open(QIODevice::WriteOnly), "a junk file");
    junk.write("not a key");
    junk.close();
    CHECK(!loadKeyFile(junk.fileName(), &error).isValid() && !error.isEmpty(), "a file that isn't a key: %s", qPrintable(error));

    usdcc::peers::FoundRoom room;
    room.announcement.type = usdcc::peers::RoomType::RenderQueue;
    room.announcement.admission = usdcc::peers::Admission::Open;
    CHECK(isPublicRenderRoom(room), "an open render queue room is public");
    room.announcement.password = true;
    CHECK(!isPublicRenderRoom(room), "not with a password");
    room.announcement.password = false;
    room.announcement.type = usdcc::peers::RoomType::Chat;
    CHECK(!isPublicRenderRoom(room), "nor a chat room");
}

// Storm only where it can render (docs/RENDER_NODE.md §5).
USDCC_TEST(node_storm_opengl_check) {
    OpenGlInfo none;
    none.why = "started without the GPU";
    CHECK(stormOpenGlProblem(none).find("without the GPU") != std::string::npos, "no context");
    OpenGlInfo gdi;
    gdi.context = true;
    gdi.major = 1;
    gdi.minor = 1;
    gdi.vendor = "Microsoft Corporation";
    gdi.renderer = "GDI Generic";
    CHECK(stormOpenGlProblem(gdi).find("GDI") != std::string::npos, "Windows' software OpenGL in session 0");
    OpenGlInfo old;
    old.context = true;
    old.major = 4;
    old.minor = 1;
    old.renderer = "Some GPU";
    CHECK(stormOpenGlProblem(old).find("4.1") != std::string::npos, "below 4.5");
    OpenGlInfo good = old;
    good.minor = 6;
    CHECK(stormOpenGlProblem(good).empty(), "4.6 on a GPU is fine");

    registerHydraFinalRenderers();
    const auto storm = FinalRenderers::instance().find("HdStormRendererPlugin");
    const auto embree = FinalRenderers::instance().find("HdEmbreeRendererPlugin");
    if (!storm || !embree) return;
    CHECK(storm->setupProblem().empty(), "the app sets no problem: Storm is offered as before");
    setStormProblem("no OpenGL here");
    CHECK(storm->setupProblem() == "no OpenGL here" && embree->setupProblem().empty(), "a node's check reaches Storm only");
    setStormProblem({});
}

#ifdef USDCC_NODE_EXE
namespace {

// usdcc_node, in a profile of its own for the test.
struct NodeProcess {
    QProcess process;
    QString output;

    explicit NodeProcess(const QStringList& arguments, bool wait = true) {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("USDCC_TEST_PROFILE"), QStringLiteral("nodetest"));
        environment.insert(QStringLiteral("USDCC_LOG"), QStringLiteral("warning"));
        process.setProcessEnvironment(environment);
        process.setProcessChannelMode(QProcess::SeparateChannels);
        QObject::connect(&process, &QProcess::readyReadStandardOutput, &process,
                         [this]() { output += QString::fromUtf8(process.readAllStandardOutput()); });
        process.start(QStringLiteral(USDCC_NODE_EXE), arguments);
        if (wait) finish();
    }
    ~NodeProcess() {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(10000);
        }
    }
    void finish(int ms = 120000) {
        waitFor([this] { return process.state() == QProcess::NotRunning; }, ms);
        output += QString::fromUtf8(process.readAllStandardOutput());
    }
    int exitCode() const { return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1; }
    QString errors() { return QString::fromUtf8(process.readAllStandardError()).right(2000); }
};

// An EXR's RGBA as floats, through Hio's OpenEXR reader.
std::vector<float> readExr(const QString& path, int* width, int* height) {
    const HioImageSharedPtr image = HioImage::OpenForReading(path.toStdString());
    if (!image) return {};
    *width = image->GetWidth();
    *height = image->GetHeight();
    const HioFormat format = image->GetFormat();
    const int channels = static_cast<int>(HioGetComponentCount(format));
    const bool half = HioGetHioType(format) == HioTypeHalfFloat;
    std::vector<unsigned char> bytes(static_cast<size_t>(image->GetBytesPerPixel()) * *width * *height);
    HioImage::StorageSpec spec;
    spec.width = *width;
    spec.height = *height;
    spec.format = format;
    spec.data = bytes.data();
    if (!image->Read(spec)) return {};
    std::vector<float> rgba(static_cast<size_t>(*width) * *height * 4, 1.0f);
    for (int i = 0; i < *width * *height; ++i) {
        for (int c = 0; c < std::min(channels, 4); ++c) {
            const size_t at = static_cast<size_t>(i) * channels + c;
            rgba[static_cast<size_t>(i) * 4 + c] =
                half ? float(reinterpret_cast<const GfHalf*>(bytes.data())[at]) : reinterpret_cast<const float*>(bytes.data())[at];
        }
    }
    return rgba;
}

}  // namespace

// Phase 2 (docs/RENDER_NODE.md §7): 3 frames in 2 × 2 tiles with Embree, the same files as the
// app's queue writes for the same settings; and the exit codes.
USDCC_TEST(node_process_render) {
    registerHydraFinalRenderers();
    if (!FinalRenderers::instance().find("HdEmbreeRendererPlugin")) {
        CHECK(false, "Embree is a final renderer");
        return;
    }
    QTemporaryDir dir;
    const QString stageFile = dir.filePath("shot.usda");
    shotStage(stageFile.toStdString())->Save();

    // The app's: its queue, from the same file with the same overrides.
    RenderArguments arguments;
    arguments.renderer = "HdEmbreeRendererPlugin";
    arguments.frames = FrameRange{1, 3, 1};
    arguments.resolution = GfVec2i(48, 32);
    arguments.tiles = GfVec2i(2, 2);
    arguments.timeLimit = 20.0;
    arguments.output = dir.filePath("app/<stage>.####.exr").toStdString();
    const std::string samples = embreeSamples().attribute;
    arguments.set = {{samples, "16"}};
    std::string error;
    UsdStageRefPtr appStage = UsdStage::Open(stageFile.toStdString());
    const auto request = prepareRenderRequest(appStage, arguments, &error);
    RenderJobPtr appJob = request ? makeRenderJob(*request, &error) : nullptr;
    CHECK(appJob != nullptr, "the app's job: %s", error.c_str());
    if (!appJob) return;
    RenderScheduler scheduler;
    scheduler.addWorker(std::make_shared<LocalWorker>());
    scheduler.submit(appJob);
    CHECK(waitFor([&] { return ended(appJob); }, 120000) && appJob->status == RenderJob::Status::Done, "the app renders: %s",
          appJob->error.c_str());

    NodeProcess node({QStringLiteral("render"), stageFile, QStringLiteral("--no-plugins"), QStringLiteral("--renderer"),
                      QStringLiteral("HdEmbreeRendererPlugin"), QStringLiteral("--frames"), QStringLiteral("1-3"),
                      QStringLiteral("--resolution"), QStringLiteral("48x32"), QStringLiteral("--tiles"), QStringLiteral("2x2"),
                      QStringLiteral("--time-limit"), QStringLiteral("20"), QStringLiteral("--set"),
                      QString::fromStdString(samples + "=16"), QStringLiteral("--output"),
                      dir.filePath("node/<stage>.####.exr"), QStringLiteral("--json")});
    CHECK(node.exitCode() == 0, "usdcc_node render exits 0: %d\n%s", node.exitCode(), qPrintable(node.errors()));
    CHECK(node.output.contains(QStringLiteral("\"event\":\"end\"")) && node.output.contains(QStringLiteral("\"status\":\"done\"")),
          "JSON lines on stdout: %s", qPrintable(node.output.right(400)));
    CHECK(node.output.count(QStringLiteral("\"status\":\"done\"")) >= 12, "12 tasks done");

    for (int frame = 1; frame <= 3; ++frame) {
        const QString name = QStringLiteral("shot.%1.exr").arg(frame, 4, 10, QLatin1Char('0'));
        const QString appFile = dir.filePath("app/" + name);
        const QString nodeFile = dir.filePath("node/" + name);
        CHECK(QFileInfo::exists(appFile) && QFileInfo::exists(nodeFile), "%s from both", qPrintable(name));
        int aw = 0, ah = 0, nw = 0, nh = 0;
        const std::vector<float> a = readExr(appFile, &aw, &ah);
        const std::vector<float> n = readExr(nodeFile, &nw, &nh);
        CHECK(!a.empty() && aw == 48 && ah == 32 && nw == aw && nh == ah, "%s: both 48 × 32 (%d × %d, %d × %d)",
              qPrintable(name), aw, ah, nw, nh);
        if (a.empty() || n.size() != a.size()) continue;
        double difference = 0.0, coverage = 0.0;
        for (size_t i = 0; i < a.size(); i += 4) {
            for (int c = 0; c < 3; ++c) difference += std::abs(a[i + c] - n[i + c]);
            coverage += (a[i + 3] > 0.5f) != (n[i + 3] > 0.5f) ? 1.0 : 0.0;
        }
        difference /= double(aw) * ah * 3;
        CHECK(difference < 0.03 && coverage / (aw * ah) < 0.03, "%s matches the app's: mean difference %.4f, %.0f pixels differ",
              qPrintable(name), difference, coverage);
    }

    // Exit codes.
    NodeProcess missing({QStringLiteral("render"), dir.filePath("nothing.usda"), QStringLiteral("--no-plugins")});
    CHECK(missing.exitCode() == 2, "a missing stage: 2, not %d", missing.exitCode());
    NodeProcess unknown({QStringLiteral("render"), stageFile, QStringLiteral("--no-plugins"), QStringLiteral("--renderer"),
                         QStringLiteral("NoSuchRenderer")});
    CHECK(unknown.exitCode() == 2, "an unknown renderer: 2, not %d", unknown.exitCode());
    const QString noCamera = dir.filePath("no_camera.usda");
    shotStage(noCamera.toStdString(), false)->Save();
    NodeProcess cameraless({QStringLiteral("render"), noCamera, QStringLiteral("--no-plugins"), QStringLiteral("--renderer"),
                            QStringLiteral("HdEmbreeRendererPlugin")});
    CHECK(cameraless.exitCode() == 2, "no camera: 2, not %d", cameraless.exitCode());
    NodeProcess nothing({QStringLiteral("frobnicate")});
    CHECK(nothing.exitCode() == 2, "an unknown command: 2, not %d", nothing.exitCode());

    NodeProcess renderers({QStringLiteral("renderers"), QStringLiteral("--no-plugins"), QStringLiteral("--json")});
    CHECK(renderers.exitCode() == 0 && renderers.output.contains(QStringLiteral("\"id\":\"HdEmbreeRendererPlugin\"")),
          "renderers lists Embree: %s", qPrintable(renderers.output.left(400)));
    NodeProcess noGpu({QStringLiteral("renderers"), QStringLiteral("--no-plugins"), QStringLiteral("--no-gpu"), QStringLiteral("--json")});
    bool stormRefused = true;
    for (const QString& line : noGpu.output.split(QLatin1Char('\n'))) {
        if (line.contains(QStringLiteral("HdStormRendererPlugin"))) stormRefused = line.contains(QStringLiteral("\"ready\":false"));
    }
    CHECK(noGpu.exitCode() == 0 && stormRefused, "with --no-gpu, Storm says it can't: %s", qPrintable(noGpu.output.left(600)));
}

// Phase 3: a node as a process, serving a room an instance in this test hosts: untrusted, it
// declines; trusted, it renders frames in tiles; killed mid-task, its task goes back.
USDCC_TEST(node_process_serve) {
    registerHydraFinalRenderers();
    if (!FinalRenderers::instance().find("HdEmbreeRendererPlugin")) return;
    QTemporaryDir dir;

    usdcc::peers::Profile artist;
    artist.name = QStringLiteral("Artist");
    artist.identity = usdcc::peers::Identity::generate();
    artist.instance = QStringLiteral("artist");
    usdcc::peers::HostOptions host;
    host.name = QStringLiteral("Farm");
    host.type = usdcc::peers::RoomType::RenderQueue;
    host.profile = artist;
    QString error;
    std::string jobError;
    std::unique_ptr<usdcc::peers::Room> room(usdcc::peers::Room::host(host, nullptr, &error));
    CHECK(room != nullptr, "a room: %s", qPrintable(error));
    if (!room) return;
    const QString link = room->link(QStringLiteral("127.0.0.1"));

    RenderScheduler scheduler;
    RenderNetworkOptions options;
    options.folder = dir.filePath("artist");
    options.scheduler = &scheduler;
    options.isNode = [] { return false; };
    options.trusted = [](const usdcc::peers::Peer&) { return false; };
    RenderNetwork network(std::move(options));
    network.addRoom(room.get());

    UsdStageRefPtr stage = shotStage();
    const SdfPath settings = createRenderSettings(stage);
    const UsdPrim prim = stage->GetPrimAtPath(settings);
    UsdRenderSettings(prim).GetResolutionAttr().Set(GfVec2i(32, 16));
    setJobOption(prim, jobattr::renderer, VtValue(std::string("HdEmbreeRendererPlugin")));
    setJobOption(prim, jobattr::output, VtValue(dir.filePath("frames/<stage>.####").toStdString()));
    setJobOption(prim, jobattr::timeLimit, VtValue(10.0));
    setJobOption(prim, jobattr::frameStart, VtValue(1.0));
    setJobOption(prim, jobattr::frameEnd, VtValue(2.0));
    setJobOption(prim, jobattr::tiles, VtValue(GfVec2i(2, 2)));
    JobRequest request;
    request.stage = stage;
    request.settings = settings;
    request.frames = JobRequest::Frames::Range;
    PeerDispatch toPeers;
    toPeers.distribute = "peers";
    request.dispatch = toPeers;
    const QString keyFile = dir.filePath("node.key");
    const auto serve = [&](const QString& trusted) {
        return std::make_unique<NodeProcess>(
            QStringList{QStringLiteral("serve"), QStringLiteral("--no-plugins"), QStringLiteral("--join"), link,
                        QStringLiteral("--trust"), trusted, QStringLiteral("--key-file"), keyFile, QStringLiteral("--name"),
                        QStringLiteral("Farm 07"), QStringLiteral("--inbox"), dir.filePath("inbox"),
                        QStringLiteral("--renderers"), QStringLiteral("HdEmbreeRendererPlugin")},
            false);
    };

    // Trusting someone else: the job is declined, and the task waits without counting a try.
    {
        auto node = serve(QStringLiteral("aaaa-bbbb"));
        CHECK(waitFor([&] { return scheduler.workers().size() == 1; }, 90000), "the node joins and becomes a worker: %s",
              qPrintable(node->errors()));
        const auto nodes = network.nodesFor("HdEmbreeRendererPlugin", true);
        CHECK(nodes.size() == 1 && nodes.front().peer.name == QStringLiteral("Farm 07"), "named, offering Embree with tiles");
        RenderJobPtr job = makeRenderJob(request, &jobError);
        CHECK(job != nullptr, "a job: %s", jobError.c_str());
        if (!job) return;
        scheduler.submit(job);
        CHECK(waitFor([&] { return !scheduler.workers().empty() && !scheduler.workers().front()->canRun(*job); }, 30000),
              "an untrusted artist's job is declined");
        CHECK(job->tasks.front().attempts == 0 && job->doneTasks() == 0, "and that wasn't a try");
        scheduler.cancel(job->id);
    }
    CHECK(waitFor([&] { return scheduler.workers().empty(); }, 45000), "a killed node goes from the queue");

    // Trusting the artist: it renders 2 frames in 2 × 2 tiles.
    auto node = serve(usdcc::peers::shortFingerprint(artist.identity.fingerprint()));
    CHECK(waitFor([&] { return scheduler.workers().size() == 1; }, 90000), "the node is back");
    RenderJobPtr job = makeRenderJob(request, &jobError);
    if (!job) return;
    scheduler.submit(job);
    CHECK(waitFor([&] { return ended(job); }, 120000) && job->status == RenderJob::Status::Done, "the node renders: %s",
          job->error.c_str());
    CHECK(std::all_of(job->tasks.begin(), job->tasks.end(), [](const JobTask& t) { return t.worker == "Farm 07"; }),
          "every task by the node");
    CHECK(job->files.size() == 2 && QFileInfo::exists(QString::fromStdString(job->files.front())), "the frames written here");
    CHECK(node->output.contains(QStringLiteral("fingerprint")), "it says its fingerprint: %s", qPrintable(node->output.left(300)));

    // Killed mid-task: the task goes back to the queue.
    UsdRenderSettings(prim).GetResolutionAttr().Set(GfVec2i(1920, 1080));
    setJobOption(prim, jobattr::timeLimit, VtValue(0.0));
    setJobOption(prim, jobattr::tiles, VtValue(GfVec2i(1, 1)));
    for (const RenderSettingField& field : FinalRenderers::instance().find("HdEmbreeRendererPlugin")->settingsSchema()) {
        if (field.rendererKey == embreeSamples().rendererKey) setFieldValue(prim, field, VtValue(100000));
    }
    RenderJobPtr slow = makeRenderJob(request, &jobError);
    if (!slow) return;
    scheduler.submit(slow);
    CHECK(waitFor([&] { return slow->tasks.front().status == JobTask::Status::Running; }, 60000), "the node starts a long task");
    node->process.kill();
    node->process.waitForFinished(10000);
    CHECK(waitFor([&] { return slow->tasks.front().status == JobTask::Status::Pending; }, 60000),
          "killed: its task waits again");
    scheduler.cancel(slow->id);
}
namespace {

// A line usdcc_node printed that starts with `prefix`, without it; empty until there is one.
QString printed(const NodeProcess& process, const QString& prefix) {
    for (const QString& line : process.output.split(QLatin1Char('\n'))) {
        if (line.startsWith(prefix)) return line.mid(prefix.size()).trimmed();
    }
    return {};
}

// An artist's instance in the queue's room, from the link the queue printed.
struct QueueArtist {
    usdcc::peers::Profile who;
    std::unique_ptr<usdcc::peers::Room> room;
    QTemporaryDir folder;
    std::unique_ptr<RenderQueueClient> client;
    QStringList queued, refused;

    QueueArtist(const QString& link, const QString& name, usdcc::peers::Identity identity = {}) {
        who.name = name;
        who.identity = identity.isValid() ? identity : usdcc::peers::Identity::generate();
        who.instance = name.toLower();
        const std::optional<usdcc::peers::JoinLink> parsed = usdcc::peers::parseJoinLink(link);
        CHECK(parsed.has_value(), "a link: %s", qPrintable(link));
        if (!parsed) return;
        usdcc::peers::JoinOptions options;
        options.profile = who;
        options.type = usdcc::peers::RoomType::RenderQueue;
        options.secret = parsed->secret;
        options.expectedFingerprint = parsed->fingerprint;
        room.reset(usdcc::peers::Room::join(parsed->address, options));
        client = std::make_unique<RenderQueueClient>(RenderQueueClient::Options{folder.path()});
        client->addRoom(room.get());
        QObject::connect(client.get(), &RenderQueueClient::queued, client.get(),
                         [this](const QString&, const QString& id, int) { queued.append(id); });
        QObject::connect(client.get(), &RenderQueueClient::refused, client.get(),
                         [this](const QString&, const QString& reason) { refused.append(reason); });
    }
};

// The queue's arguments, in a folder of its own.
QStringList queueArguments(const QTemporaryDir& dir, const QString& accept) {
    return {QStringLiteral("queue"),        QStringLiteral("--no-plugins"),     QStringLiteral("--host-room"),
            QStringLiteral("Farm"),         QStringLiteral("--local"),          QStringLiteral("--name"),
            QStringLiteral("Farm queue"),   QStringLiteral("--key-file"),       dir.filePath(QStringLiteral("queue.key")),
            QStringLiteral("--state"),      dir.filePath(QStringLiteral("state")), QStringLiteral("--output-root"),
            dir.filePath(QStringLiteral("root")), QStringLiteral("--accept"),   accept};
}

std::unique_ptr<NodeProcess> serveQueue(const QTemporaryDir& dir, const QString& link, const QString& queueFingerprint, int n) {
    return std::make_unique<NodeProcess>(
        QStringList{QStringLiteral("serve"), QStringLiteral("--no-plugins"), QStringLiteral("--join"), link,
                    QStringLiteral("--trust"), queueFingerprint, QStringLiteral("--key-file"),
                    dir.filePath(QStringLiteral("node%1.key").arg(n)), QStringLiteral("--name"), QStringLiteral("Farm 0%1").arg(n),
                    QStringLiteral("--inbox"), dir.filePath(QStringLiteral("inbox%1").arg(n)), QStringLiteral("--renderers"),
                    QStringLiteral("HdEmbreeRendererPlugin")},
        false);
}

// The Embree job: the shot's 3 frames at 48 × 32, `tiles`, its frames named <stage>.####.exr.
RenderJobPtr embreeJob(const QString& stageFile, GfVec2i tiles, double timeLimit, int samples, GfVec2i resolution = GfVec2i(48, 32),
                       bool slow = false) {
    RenderArguments arguments;
    arguments.renderer = "HdEmbreeRendererPlugin";
    arguments.frames = FrameRange{1, 3, 1};
    arguments.resolution = resolution;
    arguments.tiles = tiles;
    arguments.timeLimit = timeLimit;
    arguments.output = "<stage>.####.exr";  // relative: under the queue's root
    arguments.set = {{embreeSamples().attribute, std::to_string(samples)}};
    // Slow: no adaptive sampling, so each frame takes its time limit however simple the scene.
    if (slow) {
        const auto embree = FinalRenderers::instance().find("HdEmbreeRendererPlugin");
        for (const RenderSettingField& field : embree ? embree->settingsSchema() : std::vector<RenderSettingField>{}) {
            if (TfStringEndsWith(field.rendererKey.GetString(), "adaptiveThreshold")) arguments.set.push_back({field.attribute, "0"});
        }
    }
    std::string error;
    UsdStageRefPtr stage = UsdStage::Open(stageFile.toStdString());
    const auto request = prepareRenderRequest(stage, arguments, &error);
    RenderJobPtr job = request ? makeRenderJob(*request, &error) : nullptr;
    CHECK(job != nullptr, "the job: %s", error.c_str());
    return job;
}

double meanDifference(const QString& a, const QString& b) {
    int aw = 0, ah = 0, bw = 0, bh = 0;
    const std::vector<float> x = readExr(a, &aw, &ah);
    const std::vector<float> y = readExr(b, &bw, &bh);
    if (x.empty() || x.size() != y.size()) return 1e9;
    double sum = 0.0;
    for (size_t i = 0; i < x.size(); i += 4) {
        for (int c = 0; c < 3; ++c) sum += std::abs(x[i + c] - y[i + c]);
    }
    return sum / (double(aw) * ah * 3);
}

}  // namespace

// Phase 6 (docs/RENDER_NODE.md §7): an artist submits 3 frames in 2 × 2 tiles to a queue
// process and leaves; two node processes render them; the EXRs under the root match the app's.
// An artist the queue doesn't accept is refused.
USDCC_TEST(node_process_queue) {
    registerHydraFinalRenderers();
    if (!FinalRenderers::instance().find("HdEmbreeRendererPlugin") || !usdcc::usd::linearExrAvailable()) return;
    QTemporaryDir dir;
    const QString stageFile = dir.filePath(QStringLiteral("shot.usda"));
    shotStage(stageFile.toStdString())->Save();
    QString error;
    const usdcc::peers::Identity queueKey = loadKeyFile(dir.filePath(QStringLiteral("queue.key")), &error);
    const usdcc::peers::Identity anna = usdcc::peers::Identity::generate();

    NodeProcess queue(queueArguments(dir, anna.fingerprint()), false);
    CHECK(waitFor([&] { return !printed(queue, QStringLiteral("link ")).isEmpty(); }, 90000), "the queue prints its link: %s",
          qPrintable(queue.errors()));
    const QString link = printed(queue, QStringLiteral("link "));
    if (link.isEmpty()) return;
    auto node1 = serveQueue(dir, link, usdcc::peers::shortFingerprint(queueKey.fingerprint()), 1);
    auto node2 = serveQueue(dir, link, usdcc::peers::shortFingerprint(queueKey.fingerprint()), 2);

    // The artist, as the app would: the job made here, sent with its scene; the queue accepts
    // her by her key.
    QueueArtist annaSeat(link, QStringLiteral("Anna"), anna);
    CHECK(waitFor([&] { return annaSeat.client->queues().size() == 1; }, 30000), "Anna sees the queue");
    RenderJobPtr job = embreeJob(stageFile, GfVec2i(2, 2), 20.0, 16);
    if (!job) return;
    CHECK(annaSeat.client->submit(job, QStringLiteral("Farm queue"), &error), "sent: %s", qPrintable(error));
    CHECK(waitFor([&] { return !annaSeat.queued.isEmpty(); }, 60000), "queued");
    // The job is the queue's: Anna goes.
    annaSeat.client.reset();
    annaSeat.room.reset();

    const QString root = dir.filePath(QStringLiteral("root"));
    CHECK(waitFor([&] { return queue.output.contains(QStringLiteral(" done: 3 file(s)")); }, 180000),
          "the queue writes the 3 frames: %s", qPrintable(queue.output.right(600)));

    // The app's, from the same file and settings.
    RenderJobPtr appJob = embreeJob(stageFile, GfVec2i(2, 2), 20.0, 16);
    if (!appJob) return;
    appJob->spec.options.output = dir.filePath(QStringLiteral("app/<stage>.####.exr")).toStdString();
    appJob->spec.options.distribute = "local";
    RenderScheduler scheduler;
    scheduler.addWorker(std::make_shared<LocalWorker>());
    scheduler.submit(appJob);
    CHECK(waitFor([&] { return ended(appJob); }, 120000) && appJob->status == RenderJob::Status::Done, "the app renders");
    for (int frame = 1; frame <= 3; ++frame) {
        const QString name = QStringLiteral("shot.%1.exr").arg(frame, 4, 10, QLatin1Char('0'));
        const double difference = meanDifference(QDir(root).filePath(name), dir.filePath(QStringLiteral("app/") + name));
        CHECK(difference < 0.03, "%s under the root matches the app's: mean difference %.4f", qPrintable(name), difference);
    }

    // Not accepted.
    QueueArtist stranger(link, QStringLiteral("Stranger"));
    waitFor([&] { return stranger.client->queues().size() == 1; }, 30000);
    RenderJobPtr other = embreeJob(stageFile, GfVec2i(1, 1), 5.0, 4);
    if (!other) return;
    stranger.client->submit(other, {}, &error);
    CHECK(waitFor([&] { return !stranger.refused.isEmpty(); }, 30000), "a stranger is refused");
    CHECK(queue.output.contains(QStringLiteral("refused a job from Stranger")), "and the queue says so");
}

// Phase 7: the queue is killed mid-job and started again from its --state: the job finishes,
// and the frames done before aren't rendered again.
USDCC_TEST(node_process_queue_restart) {
    registerHydraFinalRenderers();
    if (!FinalRenderers::instance().find("HdEmbreeRendererPlugin") || !usdcc::usd::linearExrAvailable()) return;
    QTemporaryDir dir;
    const QString stageFile = dir.filePath(QStringLiteral("shot.usda"));
    shotStage(stageFile.toStdString())->Save();
    QString error;
    const usdcc::peers::Identity queueKey = loadKeyFile(dir.filePath(QStringLiteral("queue.key")), &error);
    const usdcc::peers::Identity anna = usdcc::peers::Identity::generate();
    const QString trusted = usdcc::peers::shortFingerprint(queueKey.fingerprint());

    auto queue = std::make_unique<NodeProcess>(queueArguments(dir, anna.fingerprint()), false);
    CHECK(waitFor([&] { return !printed(*queue, QStringLiteral("link ")).isEmpty(); }, 90000), "the queue's link");
    QString link = printed(*queue, QStringLiteral("link "));
    if (link.isEmpty()) return;
    auto node = serveQueue(dir, link, trusted, 1);
    const std::optional<usdcc::peers::JoinLink> parsed = usdcc::peers::parseJoinLink(link);
    usdcc::peers::JoinOptions options;
    options.profile.name = QStringLiteral("Anna");
    options.profile.identity = anna;
    options.profile.instance = QStringLiteral("anna");
    options.type = usdcc::peers::RoomType::RenderQueue;
    options.secret = parsed->secret;
    std::unique_ptr<usdcc::peers::Room> room(usdcc::peers::Room::join(parsed->address, options));
    QTemporaryDir annaFolder;
    auto client = std::make_unique<RenderQueueClient>(RenderQueueClient::Options{annaFolder.path()});
    client->addRoom(room.get());
    QStringList queued;
    QObject::connect(client.get(), &RenderQueueClient::queued, client.get(),
                     [&queued](const QString&, const QString& id, int) { queued.append(id); });
    CHECK(waitFor([&] { return client->queues().size() == 1; }, 30000), "Anna sees the queue");
    // Each frame takes its time limit: 3 s.
    RenderJobPtr job = embreeJob(stageFile, GfVec2i(1, 1), 3.0, 1000000, GfVec2i(160, 120), true);
    if (!job) return;
    client->submit(job, {});
    CHECK(waitFor([&] { return !queued.isEmpty(); }, 60000), "queued");
    client.reset();
    room.reset();

    const QString frame1 = dir.filePath(QStringLiteral("root/shot.0001.exr"));
    CHECK(waitFor([&] { return QFileInfo::exists(frame1); }, 90000), "frame 1 written");
    waitFor([] { return false; }, 700);  // its state saved
    const QDateTime written = QFileInfo(frame1).lastModified();
    CHECK(!QFileInfo::exists(dir.filePath(QStringLiteral("root/shot.0003.exr"))), "frame 3 not yet");
    // Killed, as a crash or a power cut would.
    queue->process.kill();
    queue->process.waitForFinished(10000);
    node.reset();

    queue = std::make_unique<NodeProcess>(queueArguments(dir, anna.fingerprint()), false);
    CHECK(waitFor([&] { return !printed(*queue, QStringLiteral("link ")).isEmpty(); }, 90000), "the queue starts again");
    CHECK(queue->output.contains(QStringLiteral("restored 1 job(s)")), "with the job: %s", qPrintable(queue->output.left(400)));
    link = printed(*queue, QStringLiteral("link "));
    node = serveQueue(dir, link, trusted, 1);
    CHECK(waitFor([&] { return queue->output.contains(QStringLiteral(" done: 3 file(s)")); }, 120000), "the job finishes: %s",
          qPrintable(queue->output.right(400)));
    CHECK(QFileInfo(frame1).lastModified() == written, "frame 1 wasn't rendered again");
    CHECK(QFileInfo::exists(dir.filePath(QStringLiteral("root/shot.0003.exr"))), "frame 3 is there");
}

#endif
