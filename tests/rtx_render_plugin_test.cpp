// The RTX Render plugin (src/plugins/rtx_render, docs/RTX_RENDER.md) with its helper's
// fake renderer: the panel starts the helper, sends it the stage with a camera and a
// render product of its own, writes camera moves and transform edits in place, sends the
// stage again for anything else, and ends the helper when it is deleted. Needs no GPU or
// ovrtx. Needs PySide6, like the other plugin tests; without it this file isn't built.
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
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>

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
               ("usdcc_rtx_" + std::to_string(QCoreApplication::applicationPid()) + ".usda")) {
        std::ofstream(file) << R"(#usda 1.0
(
    upAxis = "Y"
)
def Xform "Box"
{
    double3 xformOp:translate = (0, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]

    def Cube "Shape"
    {
    }
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
        "for name in [n for n in sys.modules if n.split('.')[0] in ('usdcc_rtx_render', 'usdcc_vray_render', "
        "'usdcc_measure_example')]:\n"
        "    del sys.modules[name]\n");
}

// Runs the event loop until `expression` is True in Python, for at most `ms`.
// Generous: the first helper start after a build can be slow.
bool waitFor(const std::string& expression, int ms = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        if (evaluated(expression) == "True") return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return evaluated(expression) == "True";
}

void setTranslate(const StageRefPtr& stage, const char* path, double x, double y, double z) {
    stage->usdStage()
        ->GetPrimAtPath(PXR_NS::SdfPath(path))
        .GetAttribute(PXR_NS::TfToken("xformOp:translate"))
        .Set(PXR_NS::GfVec3d(x, y, z));
}
}  // namespace

USDCC_TEST(rtx_render_plugin) {
    TestSettings settings;
    auto& app = pythonTestApp();
    OpenStage open;
    forgetExampleModules();

    // As shipped, the plugin needs ovrtx installed (python.requires), which a test
    // machine doesn't have: it says so.
    const std::filesystem::path source = std::filesystem::u8path(USDCC_EXAMPLE_PLUGINS) / "rtx_render";
    {
        QTemporaryDir folder;
        std::filesystem::copy(source, std::filesystem::u8path(folder.path().toStdString()) / "rtx_render",
                              std::filesystem::copy_options::recursive);
        PluginManager::Options options;
        options.folders = {std::filesystem::u8path(folder.path().toStdString())};
        PluginManager shipped(options);
        shipped.scan();
        MainWindow window;
        ViewPanelRegistry registry;
        const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &registry, &shipped});
        shipped.activate(app.python);
        const auto* plugin = shipped.find("org.usdcc.rtx-render");
        CHECK(plugin && plugin->state == PluginState::NeedsPackages && plugin->reason.find("ovrtx") != std::string::npos,
              "as shipped, it needs ovrtx installed: %s", plugin ? plugin->reason.c_str() : "not found");
        CHECK(!registry.find(QStringLiteral("org.usdcc.rtx-render.view")), "and none of its code runs");
    }

    // A copy without python.requires (and the index it needs them from), so it loads; the
    // fake renderer needs none of it.
    QTemporaryDir plugins;
    const std::filesystem::path copy = std::filesystem::u8path(plugins.path().toStdString()) / "rtx_render";
    std::filesystem::copy(source, copy, std::filesystem::copy_options::recursive);
    CHECK(run("import json\npath = r'" + (copy / "usdcc-plugin.json").string() +
              "'\nmanifest = json.load(open(path))\ndel manifest['python']['requires']\ndel manifest['python']['indexes']\n"
              "json.dump(manifest, open(path, 'w'))\n"),
          "a copy of the plugin without its packages");
    PluginManager::Options options;
    options.folders = {copy.parent_path()};
    PluginManager manager(options);
    manager.scan();

    MainWindow window;
    ViewPanelRegistry registry;
    const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &registry, &manager});
    manager.activate(app.python);
    const std::string id = "org.usdcc.rtx-render";
    const auto* plugin = manager.find(id);
    CHECK(plugin && plugin->state == PluginState::Active, "it loads: %s", plugin ? plugin->reason.c_str() : "not found");
    CHECK(registry.find(QString::fromStdString(id + ".view")), "its panel type");
    CHECK(evaluated("'RtxRender/python' in __import__('usdcc.settings').settings.keys('RTX')") == "True", "its settings");

    CHECK(run(R"(
import builtins, sys, usdcc.app, usdcc.settings, usdcc.usd
from PySide6 import QtWidgets
rtx = sys.modules["usdcc_rtx_render"]
rtx.config.FAKE_RENDERER = True
usdcc.settings.set("RtxRender/editDelay", 20)
# What the panel sends the helper.
builtins.rtx_sent = []
send = rtx.client.RenderClient.send
def record(self, name, **fields):
    builtins.rtx_sent.append((name, fields))
    send(self, name, **fields)
rtx.client.RenderClient.send = record
builtins.rtx_names = lambda: [name for name, _ in builtins.rtx_sent]
builtins.rtx_last = lambda wanted: [fields for name, fields in builtins.rtx_sent if name == wanted][-1]
stage = [s for s in usdcc.usd.stages() if s.GetRootLayer().identifier.endswith("usdcc_rtx_%d.usda" % __import__("os").getpid())][0]
panel = usdcc.app.open_view_panel("org.usdcc.rtx-render.view", stage)
builtins.rtx_render = panel.widget.findChild(QtWidgets.QToolButton, "renderButton")
builtins.rtx_cameras = panel.widget.findChild(QtWidgets.QComboBox, "renderCamera")
builtins.rtx_status = panel.widget.findChild(QtWidgets.QLabel, "renderStatus")
panel.widget.findChild(QtWidgets.QComboBox, "renderResolution").setCurrentIndex(3)  # 1280 x 720
builtins.rtx_test = panel
builtins.rtx_module = rtx
builtins.rtx_stage = stage
builtins.rtx_usdcc = usdcc
)"),
          "opening a panel");

    CHECK(run("rtx_render.setChecked(True)"), "pressing Render");
    CHECK(waitFor("rtx_test.image_size() == (1280, 720)"),
          "starts the helper, which sends back an image at the asked size: %s", evaluated("rtx_status.text()").c_str());
    CHECK(evaluated("rtx_names()[:2]") == "['init', 'load']", "init, then the stage: %s", evaluated("rtx_names()").c_str());

    // What the helper was given: the stage, with a camera and a render product of our own.
    CHECK(run(R"(
import builtins
from pxr import Usd
exported = Usd.Stage.Open(rtx_last("load")["path"])
product = exported.GetPrimAtPath("/__usdccRtx/Product")
builtins.rtx_exported = (product.GetTypeName(), product.GetRelationship("camera").GetTargets(),
                         product.GetAttribute("omni:rtx:rendermode").Get(), tuple(product.GetAttribute("resolution").Get()),
                         bool(exported.GetPrimAtPath("/Box/Shape")))
)"),
          "reading the export");
    CHECK(evaluated("rtx_exported") ==
              "('RenderProduct', [Sdf.Path('/__usdccRtx/Camera')], 'RealTimePathTracing', (1280, 720), True)",
          "the stage with a render product looking through its camera: %s", evaluated("rtx_exported").c_str());
    CHECK(evaluated("rtx_test.stage.GetPrimAtPath('/__usdccRtx').IsValid()") == "False", "the stage itself is left alone");

    // A viewport on the stage: its camera is written in place, not loaded.
    CHECK(run("import builtins\nbuiltins.rtx_loads = rtx_names().count('load')"), "counting loads");
    auto viewport = std::make_unique<usdcc::usd::ViewportViewPanel>(&app.stages, open.stage,
                                                                    std::make_shared<usdcc::tools::ToolSet>(), nullptr);
    viewport->hydraWindow()->setCameraState({PXR_NS::GfVec3d(3.0, 0.0, 0.0), 12.0, 30.0, -10.0});
    CHECK(waitFor("'camera' in rtx_names() and (__import__('pxr').Gf.Vec3d(*rtx_last('camera')['transform'][12:15]) - "
                  "rtx_usdcc.app.active_viewport_camera().transform.ExtractTranslation()).GetLength() < 1e-6"),
          "moving the viewport camera sends it: %s", evaluated("rtx_names()[-3:]").c_str());

    // A transform edit is written in place; the camera follows, in case it moved.
    setTranslate(open.stage, "/Box", 1.0, 2.0, 3.0);
    CHECK(waitFor("'xforms' in rtx_names()"), "moving a prim sends its transform: %s", evaluated("rtx_names()[-3:]").c_str());
    CHECK(evaluated("[(i['path'], i['matrix'][12:15]) for i in rtx_last('xforms')['items']]") == "[('/Box', [1.0, 2.0, 3.0])]",
          "its local transform: %s", evaluated("rtx_last('xforms')").c_str());
    CHECK(evaluated("rtx_names().count('load') == rtx_loads") == "True", "without loading the stage again");
    CHECK(waitFor("rtx_test.image_size() is not None and rtx_status.text().startswith('Done')"), "and renders again");

    // Looking through a camera prim, then moving it.
    CHECK(run("rtx_cameras.setCurrentIndex(1)"), "picking the Shot camera");
    CHECK(evaluated("rtx_last('camera')['transform'][12:15]") == "[0.0, 1.0, 8.0]", "sends it");
    setTranslate(open.stage, "/Shot", 0.0, 2.0, 9.0);
    CHECK(waitFor("rtx_last('camera')['transform'][12:15] == [0.0, 2.0, 9.0]"), "moving it moves the render's camera: %s",
          evaluated("rtx_last('camera')").c_str());

    // Anything else sends the stage again.
    open.stage->usdStage()->DefinePrim(PXR_NS::SdfPath("/Added"), PXR_NS::TfToken("Sphere"));
    CHECK(waitFor("rtx_names().count('load') == rtx_loads + 1"), "a new prim loads the stage again");
    CHECK(run("rtx_test.mode_combo.setCurrentText('Path Tracing')"), "picking Path Tracing");
    CHECK(evaluated("rtx_names().count('load') == rtx_loads + 2") == "True", "loads the stage again");
    // The stage is held while its prim is asked about: a prim outliving its stage has expired.
    CHECK(evaluated("(lambda s: s.GetPrimAtPath('/__usdccRtx/Product').GetAttribute('omni:rtx:rendermode').Get())"
                    "(__import__('pxr').Usd.Stage.Open(rtx_last('load')['path']))") == "PathTracing",
          "in that mode");

    CHECK(run(R"(
import builtins
rtx_render.setChecked(False)
builtins.rtx_pid = rtx_test.client.process.processId()
builtins.rtx_folder = rtx_test.files.folder
)"),
          "stopping");
    CHECK(evaluated("rtx_names()[-1]") == "stop", "sends stop");
    CHECK(evaluated("rtx_test.client is not None and rtx_test.client.running()") == "True", "and keeps the helper");

    // Deleting the panel ends the helper and removes its files.
    viewport.reset();
    for (ads::CDockWidget* dock : window.dockManager()->dockWidgetsMap()) {
        if (qobject_cast<ScriptedRenderViewPanel*>(dock)) dock->deleteDockWidget();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(run(R"(
import builtins, ctypes
def alive(pid):
    handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    code = ctypes.c_ulong()
    ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
    ctypes.windll.kernel32.CloseHandle(handle)
    return code.value == 259  # STILL_ACTIVE
builtins.rtx_alive = alive
)"),
          "a process check");
    CHECK(waitFor("not rtx_alive(rtx_pid)", 5000), "deleting the panel ends the helper");
    CHECK(evaluated("__import__('os').path.exists(rtx_folder)") == "False", "and removes its files");
    CHECK(evaluated("len(rtx_module.client._running)") == "0", "nothing left running");

    // A first frame that takes long, as a scene with many complex materials' does: the
    // panel says it's being worked on, and then shows it.
    CHECK(run(R"(
import builtins, os, usdcc.app
from PySide6 import QtWidgets
os.environ["USDCC_RTX_FAKE_FIRST_FRAME"] = "4"
os.environ["USDCC_RTX_FAKE_SLOW_AFTER"] = "1"
slow = usdcc.app.open_view_panel("org.usdcc.rtx-render.view", rtx_stage)
builtins.rtx_slow = slow
builtins.rtx_slow_status = slow.widget.findChild(QtWidgets.QLabel, "renderStatus")
slow.widget.findChild(QtWidgets.QToolButton, "renderButton").setChecked(True)
)"),
          "a panel whose first frame takes 4 s");
    CHECK(waitFor("rtx_slow_status.text().startswith('Working on the first frame')", 10000),
          "while it's worked on, the status says so: %s", evaluated("rtx_slow_status.text()").c_str());
    CHECK(waitFor("rtx_slow.image_size() is not None", 15000), "and the frame is shown when it's done");
    CHECK(run("import os\ndel os.environ['USDCC_RTX_FAKE_FIRST_FRAME']\ndel os.environ['USDCC_RTX_FAKE_SLOW_AFTER']\n"),
          "back to quick frames");
    for (ads::CDockWidget* dock : window.dockManager()->dockWidgetsMap()) {
        if (qobject_cast<ScriptedRenderViewPanel*>(dock)) dock->deleteDockWidget();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // Without ovrtx installed, Render says so and starts nothing; the same for another
    // Python that isn't there.
    CHECK(run(R"(
import builtins, usdcc.app
from PySide6 import QtWidgets
rtx_module.config.FAKE_RENDERER = False

def press_render():
    panel = usdcc.app.open_view_panel("org.usdcc.rtx-render.view", rtx_stage)
    button = panel.widget.findChild(QtWidgets.QToolButton, "renderButton")
    button.setChecked(True)
    return (button.isChecked(), panel.client is None, panel.widget.findChild(QtWidgets.QLabel, "renderStatus").text())

builtins.rtx_press_render = press_render
)"),
          "panels that can't render");
    CHECK(run("import builtins\nbuiltins.rtx_without = rtx_press_render()"), "one without ovrtx installed");
    CHECK(evaluated("rtx_without[:2]") == "(False, True)", "doesn't render or start a helper");
    CHECK(evaluated("\"ovrtx isn't installed\" in rtx_without[2] and 'Install Packages' in rtx_without[2]") == "True",
          "and says how to install it: %s", evaluated("rtx_without[2]").c_str());
    CHECK(run("import builtins, usdcc.settings\nusdcc.settings.set('RtxRender/python', 'C:/nowhere/python.exe')\n"
              "builtins.rtx_other = rtx_press_render()"),
          "one with another Python that isn't there");
    CHECK(evaluated("rtx_other[:2]") == "(False, True)", "doesn't render either");
    CHECK(evaluated("\"doesn't exist\" in rtx_other[2]") == "True", "and says why: %s", evaluated("rtx_other[2]").c_str());

    for (ads::CDockWidget* dock : window.dockManager()->dockWidgetsMap()) {
        if (qobject_cast<ScriptedRenderViewPanel*>(dock)) dock->deleteDockWidget();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    // The patched send() goes with the module (forgetExampleModules()).
    CHECK(run("import builtins\nfor name in [n for n in dir(builtins) if n.startswith('rtx_')]: delattr(builtins, name)\n"),
          "cleaning up");
    manager.deactivate(app.python);
    forgetExampleModules();
}
