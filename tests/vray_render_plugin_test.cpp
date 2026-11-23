// The V-Ray Render plugin (src/plugins/vray_render, docs/VRAY_RENDER.md) with its
// helper's fake renderer: the panel starts the helper, sends it the stage, the
// camera and the size, shows what comes back, follows the viewport and edits, and
// ends the helper when it closes. Needs no V-Ray. Needs PySide6, like the other
// plugin tests; without it this file isn't built.
#include "python_test_app.h"
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/scripted_render_view_panel.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/render/render_job.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render/render_spec.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdRender/settings.h>

#include <QFileInfo>
#include <QTemporaryDir>

#include <DockManager.h>

#include <QCoreApplication>
#include <QElapsedTimer>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using usdcc::scripting::AppContextScope;
using usdcc::scripting::PluginManager;
using usdcc::scripting::PluginState;
using usdcc::scripting::ScriptedRenderViewPanel;
using usdcc::test::pythonTestApp;
using usdcc::ui::MainWindow;
using usdcc::ui::ViewPanelRegistry;
using usdcc::usd::StageRefPtr;

namespace {
struct OpenStage {
    std::filesystem::path file;
    StageRefPtr stage;
    OpenStage()
        : file(std::filesystem::temp_directory_path() /
               ("usdcc_vray_" + std::to_string(QCoreApplication::applicationPid()) + ".usda")) {
        std::ofstream(file) << R"(#usda 1.0
(
    upAxis = "Y"
)
def Cube "Box"
{
}
def Camera "Shot"
{
    float focalLength = 50
    float horizontalAperture = 36
    double3 xformOp:translate = (0, 1, 8)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)";
        stage = pythonTestApp().stages.openStage(file.string());
    }
    ~OpenStage() {
        pythonTestApp().stages.closeStage(stage);
        std::error_code error;
        std::filesystem::remove(file, error);
    }
};

bool run(const std::string& code) { return pythonTestApp().python.runString(code); }

std::string evaluated(const std::string& expression) {
    const auto value = pythonTestApp().python.evaluate(expression);
    return value ? *value : "?";
}

// The example plugins' modules, which another test of this process may have
// loaded already: a plugin isn't loaded over a module of the same name.
void forgetExampleModules() {
    run("import sys\n"
        "for name in [n for n in sys.modules if n.split('.')[0] in ('usdcc_vray_render', 'usdcc_measure_example')]:\n"
        "    del sys.modules[name]\n");
}

// Runs the event loop until `expression` is True in Python, for at most `ms`.
// Generous: the first helper start after a build can be slow (a fresh python.exe
// process, files being scanned).
bool waitFor(const std::string& expression, int ms = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        if (evaluated(expression) == "True") return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return evaluated(expression) == "True";
}
}  // namespace

USDCC_TEST(vray_render_plugin) {
    TestSettings settings;
    auto& app = pythonTestApp();
    OpenStage open;
    forgetExampleModules();
    // No shader nodes: with V-Ray installed here, the plugin would run plugininfo
    // (vray_shader_nodes_test covers them).
    run("import os\nos.environ['USDCC_VRAY_SHADER_NODES'] = '0'");
    PluginManager::Options options;
    options.folders = {std::filesystem::u8path(USDCC_EXAMPLE_PLUGINS)};
    PluginManager manager(options);
    manager.scan();

    MainWindow window;
    ViewPanelRegistry registry;
    const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &registry, &manager});
    manager.activate(app.python);
    const std::string id = "org.usdcc.vray-render";
    const auto* plugin = manager.find(id);
    CHECK(plugin && plugin->state == PluginState::Active, "it loads: %s", plugin ? plugin->reason.c_str() : "not found");
    CHECK(registry.find(QString::fromStdString(id + ".view")), "its panel type");
    CHECK(evaluated("'VRayRender/appSdkFolder' in __import__('usdcc.settings').settings.keys('V-Ray')") == "True",
          "its settings");

    CHECK(run(R"(
import builtins, math, sys, usdcc.app, usdcc.settings, usdcc.usd
builtins.math = math
from pxr import Usd
vray = sys.modules["usdcc_vray_render"]
vray.config.FAKE_RENDERER = True
usdcc.settings.set("VRayRender/editDelay", 50)
stage = [s for s in usdcc.usd.stages() if s.GetRootLayer().identifier.endswith("usdcc_vray_%d.usda" % __import__("os").getpid())][0]
from PySide6 import QtWidgets
panel = usdcc.app.open_view_panel("org.usdcc.vray-render.view", stage)
# The app's panel's own controls, by name.
builtins.vray_render = panel.widget.findChild(QtWidgets.QToolButton, "renderButton")
builtins.vray_cameras = panel.widget.findChild(QtWidgets.QComboBox, "renderCamera")
builtins.vray_status = panel.widget.findChild(QtWidgets.QLabel, "renderStatus")
panel.widget.findChild(QtWidgets.QComboBox, "renderResolution").setCurrentIndex(3)  # 1280 x 720, whatever the panel's size
# The cameras the panel sends.
builtins.vray_sent = []
def record(camera, send=panel.camera_changed):
    builtins.vray_sent.append(camera)
    send(camera)
panel.camera_changed = record
builtins.vray_test = panel
builtins.vray_module = vray
builtins.vray_stage = stage
builtins.vray_usdcc = usdcc
builtins.vray_Usd = Usd
)"), "opening a panel");
    CHECK(evaluated("vray_cameras.itemText(1)") == "Shot", "its camera menu lists the stage's cameras");

    CHECK(run("vray_render.setChecked(True)"), "pressing Render");
    CHECK(waitFor("vray_test.image_size() == (1280, 720)"),
          "starts the helper, which sends back an image at the asked size, 1280 x 720: %s", evaluated("vray_status.text()").c_str());
    CHECK(evaluated("vray_render.text()") == "Stop", "the button stops it now");

    // What the helper was given: the stage with render settings and a camera of our own.
    CHECK(run(R"(
import builtins
from pxr import Usd
exported = Usd.Stage.Open(vray_test.files._files[-1])
builtins.vray_exported = (exported.GetMetadata("renderSettingsPrimPath"),
                          exported.GetPrimAtPath("/__usdccVRay/RenderSettings").GetRelationship("camera").GetTargets(),
                          bool(exported.GetPrimAtPath("/Box")))
)"), "reading the export");
    CHECK(evaluated("vray_exported") == "('/__usdccVRay/RenderSettings', [Sdf.Path('/__usdccVRay/Camera')], True)",
          "the stage with render settings and a camera: %s", evaluated("vray_exported").c_str());
    CHECK(evaluated("vray_test.stage.GetPrimAtPath('/__usdccVRay').IsValid()") == "False", "the stage itself is left alone");

    // A viewport on the stage: the render follows its camera.
    auto viewport = std::make_unique<usdcc::usd::ViewportViewPanel>(&app.stages, open.stage,
                                                                    std::make_shared<usdcc::tools::ToolSet>(), nullptr);
    viewport->hydraWindow()->setCameraState({PXR_NS::GfVec3d(3.0, 0.0, 0.0), 12.0, 30.0, -10.0});
    CHECK(waitFor("len(vray_sent) > 0 and (vray_sent[-1].transform.ExtractTranslation() - "
                  "vray_usdcc.app.active_viewport_camera().transform.ExtractTranslation()).GetLength() < 1e-6"),
          "moving the viewport camera moves the render's");
    CHECK(evaluated("abs(vray_module.scene.Camera.from_render(vray_sent[-1]).fov - "
                    "2 * math.atan(math.tan(math.radians(22.5)) * 1280 / 720)) < 1e-9") == "True",
          "keeping the viewport's vertical field of view at the image's aspect ratio: %s", evaluated("vray_sent[-1]").c_str());

    // A camera prim instead.
    CHECK(run("vray_cameras.setCurrentIndex(1)"), "picking the Shot camera");
    CHECK(evaluated("abs(vray_module.scene.Camera.from_render(vray_sent[-1]).fov - 2 * math.atan(18 / 50)) < 1e-6") == "True",
          "renders through it: %s", evaluated("vray_sent[-1]").c_str());
    CHECK(evaluated("tuple(vray_sent[-1].transform.ExtractTranslation())") == "(0.0, 1.0, 8.0)", "from where it is");
    CHECK(evaluated("str(vray_sent[-1].prim)") == "/Shot", "and says which it is");

    // An edit sends the stage again.
    CHECK(run("import builtins\nbuiltins.vray_before = vray_test.files._files[-1]"), "remembering the export");
    open.stage->usdStage()->DefinePrim(PXR_NS::SdfPath("/Added"), PXR_NS::TfToken("Sphere"));
    CHECK(waitFor("vray_test.files._files[-1] != vray_before"), "an edit exports the stage again");
    // The stage is held while its prim is asked about: a prim outliving its stage has expired.
    CHECK(evaluated("(lambda s: bool(s.GetPrimAtPath('/Added')))(vray_Usd.Stage.Open(vray_test.files._files[-1]))") == "True",
          "with the edit");

    CHECK(run(R"(
import builtins
vray_render.setChecked(False)
builtins.vray_pid = vray_test.client.process.processId()
builtins.vray_folder = vray_test.files.folder
)"), "stopping");
    CHECK(evaluated("vray_test.client is not None and vray_test.client.running()") == "True", "keeps the helper for the next render");

    // Closing the panel ends the helper and removes its files.
    viewport.reset();
    // As File > Close Stage does: out of the dock manager, then deleted. (A plain
    // delete leaves the dock manager holding it, and the next panel opened reads it.)
    for (ads::CDockWidget* dock : window.dockManager()->dockWidgetsMap()) {
        if (qobject_cast<ScriptedRenderViewPanel*>(dock)) dock->deleteDockWidget();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(run(R"(
import builtins, ctypes, os
def alive(pid):
    if os.name != "nt":
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        return True
    handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    code = ctypes.c_ulong()
    ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
    ctypes.windll.kernel32.CloseHandle(handle)
    return code.value == 259  # STILL_ACTIVE
builtins.vray_alive = alive
)"), "a process check");
    CHECK(waitFor("not vray_alive(vray_pid)", 5000), "closing the panel ends the helper");
    CHECK(evaluated("__import__('os').path.exists(vray_folder)") == "False", "and removes its files");
    CHECK(evaluated("len(vray_module.client._running)") == "0", "nothing left running");

    // Without the App SDK, Render says so and starts nothing.
    CHECK(run(R"(
import builtins, usdcc.app, usdcc.settings
vray_module.config.FAKE_RENDERER = False
usdcc.settings.set("VRayRender/appSdkFolder", "C:/nowhere/AppSDK")
from PySide6 import QtWidgets
broken = usdcc.app.open_view_panel("org.usdcc.vray-render.view", vray_stage)
builtins.vray_broken_render = broken.widget.findChild(QtWidgets.QToolButton, "renderButton")
builtins.vray_broken_status = broken.widget.findChild(QtWidgets.QLabel, "renderStatus")
builtins.vray_broken_render.setChecked(True)
builtins.vray_broken = broken
)"), "a panel without the App SDK");
    CHECK(evaluated("vray_broken_render.isChecked()") == "False", "doesn't render");
    CHECK(evaluated("vray_broken.client is None") == "True", "starts no helper");
    CHECK(evaluated("'isn\\'t a V-Ray App SDK folder' in vray_broken_status.text()") == "True", "and says why: %s",
          evaluated("vray_broken_status.text()").c_str());

    CHECK(run("import builtins\nfor name in [n for n in dir(builtins) if n.startswith('vray_')]: delattr(builtins, name)\n"),
          "cleaning up");
    manager.deactivate(app.python);
    forgetExampleModules();
}

// V-Ray for final renders (final.py, docs/VRAY_RENDER.md section 10), with the helper's fake
// renderer: the plugin adds the renderer and takes it away, a job in two tiles renders through
// one helper, the setting reaches it, and the tiles go where they belong.
USDCC_TEST(vray_final_render) {
    TestSettings settings;
    auto& app = pythonTestApp();
    OpenStage open;
    forgetExampleModules();
    run("import os\nos.environ['USDCC_VRAY_SHADER_NODES'] = '0'");
    PluginManager::Options options;
    options.folders = {std::filesystem::u8path(USDCC_EXAMPLE_PLUGINS)};
    PluginManager manager(options);
    manager.scan();
    MainWindow window;
    ViewPanelRegistry registry;
    const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &registry, &manager});
    manager.activate(app.python);
    const std::string id = "org.usdcc.vray-render";
    CHECK(usdcc::render::FinalRenderers::instance().find(id) != nullptr, "the plugin adds V-Ray as a final renderer");
    CHECK(run("import sys\nsys.modules['usdcc_vray_render'].config.FAKE_RENDERER = True\n"), "the fake renderer");
    const auto renderer = usdcc::render::FinalRenderers::instance().find(id);
    if (!renderer) return;
    CHECK(renderer->setupProblem().empty() && renderer->capabilities().tiles, "it can render here, and tiles");
    const auto schema = renderer->settingsSchema();
    CHECK(schema.size() == 5 && schema[1].attribute == "vray:noiseThreshold", "its settings: %zu", schema.size());

    const PXR_NS::UsdStageRefPtr stage = open.stage->usdStage();
    const PXR_NS::SdfPath path = usdcc::render::createRenderSettings(stage);
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
    PXR_NS::UsdRenderSettings(prim).GetResolutionAttr().Set(PXR_NS::GfVec2i(24, 12));
    usdcc::render::setJobOption(prim, usdcc::render::jobattr::renderer, PXR_NS::VtValue(id));
    usdcc::render::setJobOption(prim, usdcc::render::jobattr::tiles, PXR_NS::VtValue(PXR_NS::GfVec2i(2, 1)));
    QTemporaryDir dir;
    usdcc::render::setJobOption(prim, usdcc::render::jobattr::output, PXR_NS::VtValue((dir.path() + "/<stage>").toStdString()));
    prim.CreateAttribute(PXR_NS::TfToken("vray:noiseThreshold"), PXR_NS::SdfValueTypeNames->Float, true).Set(0.25f);

    usdcc::render::JobRequest request;
    request.stage = stage;
    std::string error;
    usdcc::render::RenderJobPtr job = usdcc::render::makeRenderJob(request, &error);
    CHECK(job && job->tasks.size() == 2, "a job in two tiles: %s", error.c_str());
    if (!job) return;
    usdcc::render::RenderScheduler scheduler;
    scheduler.addWorker(std::make_shared<usdcc::render::LocalWorker>());
    QImage frame;
    QObject::connect(&scheduler, &usdcc::render::RenderScheduler::frameRendered,
                     [&frame](const QString&, int, const QImage& image) { frame = image; });
    scheduler.submit(job);
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 60000 && (job->status == usdcc::render::RenderJob::Status::Queued ||
                                       job->status == usdcc::render::RenderJob::Status::Running)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    CHECK(job->status == usdcc::render::RenderJob::Status::Done, "rendered: %s", job->error.c_str());
    bool exact = frame.size() == QSize(24, 12);
    for (int y = 0; y < 12 && exact; ++y) {
        const float* row = reinterpret_cast<const float*>(frame.constScanLine(y));
        for (int x = 0; x < 24 && exact; ++x) {
            exact = std::abs(row[x * 4] - x / 24.0f) < 1e-5f && std::abs(row[x * 4 + 1] - y / 12.0f) < 1e-5f &&
                    std::abs(row[x * 4 + 2] - 0.25f) < 1e-6f;
            if (!exact) std::printf("    pixel %d,%d: %f %f %f\n", x, y, row[x * 4], row[x * 4 + 1], row[x * 4 + 2]);
        }
    }
    CHECK(exact, "each tile rendered as a region of the whole, stitched, with the prim's noise threshold");
    CHECK(evaluated("len(__import__('sys').modules['usdcc_vray_render'].client._running)") == "1", "one helper for both tiles, kept");

    manager.deactivate(app.python);
    CHECK(usdcc::render::FinalRenderers::instance().find(id) == nullptr, "deactivating takes the renderer away");
    CHECK(evaluated("len(__import__('sys').modules['usdcc_vray_render'].client._running)") == "0", "and ends its helper");
    forgetExampleModules();
}
