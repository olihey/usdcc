// A render view panel made in Python (docs/RENDER_VIEW_PANEL.md): a
// usdcc.ui.RenderViewPanel subclass registered with register_view_panel() opens as
// a ScriptedRenderViewPanel, gets its hooks called, shows the images it hands back,
// and is closed once. Needs PySide6; without it this file isn't built.
#include "python_test_app.h"
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/scripted_render_view_panel.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <QCoreApplication>
#include <QElapsedTimer>

#include <filesystem>
#include <fstream>
#include <string>

using usdcc::scripting::AppContextScope;
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
               ("usdcc_render_panel_" + std::to_string(QCoreApplication::applicationPid()) + ".usda")) {
        std::ofstream(file) << "#usda 1.0\ndef Cube \"Box\"\n{\n}\ndef Camera \"Shot\"\n{\n}\n";
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

bool waitFor(const std::string& expression, int ms = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        if (evaluated(expression) == "True") return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return evaluated(expression) == "True";
}

void closeRenderPanels(MainWindow& window) {
    // As File > Close Stage does: out of the dock manager, then deleted.
    for (ads::CDockWidget* dock : window.dockManager()->dockWidgetsMap()) {
        if (qobject_cast<ScriptedRenderViewPanel*>(dock)) dock->deleteDockWidget();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
}  // namespace

USDCC_TEST(python_render_view_panel) {
    TestSettings settings;
    auto& app = pythonTestApp();
    OpenStage open;
    MainWindow window;
    ViewPanelRegistry registry;
    const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &registry});

    CHECK(run(R"(
import builtins, usdcc.app, usdcc.ui, usdcc.usd
from PySide6 import QtGui, QtWidgets

class Probe(usdcc.ui.RenderViewPanel):
    renderer_name = "Probe"

    def __init__(self):
        self.calls = []

    def create_toolbar(self, layout):
        self.extra = QtWidgets.QComboBox()
        self.extra.addItems(["Fast", "Good"])
        layout.addWidget(self.extra)
        self.set_edit_delay(20)

    def start(self):
        self.calls.append("start")
        image = QtGui.QImage(64, 32, QtGui.QImage.Format.Format_RGBA8888)
        image.fill(QtGui.QColor(200, 100, 50))
        self.show_image(image)
        self.set_state("Done")

    def stop(self):
        self.calls.append("stop")

    def reload(self):
        self.calls.append("reload")

    def camera_changed(self, camera):
        self.calls.append(("camera", camera))

    def size_changed(self, width, height):
        self.calls.append(("size", width, height))

    def stage_edited(self, resynced, changed):
        self.calls.append(("edited", [str(p) for p in resynced], [str(p) for p in changed]))

    def close(self):
        self.calls.append("close")

builtins.render_probe_class = Probe
usdcc.app.register_view_panel("test.render-probe", "Render Probe", Probe)
stage = [s for s in usdcc.usd.stages() if "usdcc_render_panel_" in s.GetRootLayer().identifier][0]
builtins.render_stage = stage
)"),
          "registering a RenderViewPanel subclass");
    CHECK(registry.find(QStringLiteral("test.render-probe")), "makes a panel type");

    // Opening and closing many: nothing is freed twice, nothing crashes.
    for (int i = 0; i < 20; ++i) {
        CHECK(run("import builtins, usdcc.app\nbuiltins.render_many = usdcc.app.open_view_panel('test.render-probe', render_stage)\n"),
              "opening panel %d", i);
        QCoreApplication::processEvents();
        closeRenderPanels(window);
        CHECK(evaluated("render_many.calls[-1]") == "close", "closing panel %d calls close()", i);
    }

    CHECK(run(R"(
import builtins, usdcc.app
from PySide6 import QtWidgets
panel = usdcc.app.open_view_panel("test.render-probe", render_stage)
builtins.render_panel = panel
builtins.render_button = panel.widget.findChild(QtWidgets.QToolButton, "renderButton")
builtins.render_cameras = panel.widget.findChild(QtWidgets.QComboBox, "renderCamera")
builtins.render_resolution = panel.widget.findChild(QtWidgets.QComboBox, "renderResolution")
builtins.render_status = panel.widget.findChild(QtWidgets.QLabel, "renderStatus")
)"),
          "opening one");
    CHECK(evaluated("isinstance(render_panel, render_probe_class)") == "True", "open_view_panel() returns the instance");
    CHECK(evaluated("render_panel.extra.parent() is not None") == "True", "its own control is on the toolbar");
    CHECK(evaluated("render_cameras.itemText(1)") == "Shot", "the camera menu lists the stage's cameras");
    CHECK(evaluated("render_panel.stage == render_stage") == "True", "it knows its stage");

    CHECK(run("render_resolution.setCurrentIndex(3)"), "picking 1280 x 720");
    CHECK(evaluated("render_panel.render_size()") == "(1280, 720)", "is the size");
    CHECK(evaluated("[c for c in render_panel.calls if c[0] == 'size'][-1]") == "('size', 1280, 720)",
          "size_changed() hears of it: %s", evaluated("render_panel.calls").c_str());
    CHECK(evaluated("abs(render_panel.calls[-1][1].aspect - 1280 / 720) < 1e-9") == "True",
          "and camera_changed() follows, at the new aspect ratio");

    CHECK(run("render_cameras.setCurrentIndex(1)"), "picking the Shot camera");
    CHECK(evaluated("str(render_panel.calls[-1][1].prim)") == "/Shot", "camera_changed() gets it");
    CHECK(evaluated("str(render_panel.camera().prim)") == "/Shot", "and so does camera()");

    CHECK(run("render_button.setChecked(True)"), "pressing Render");
    CHECK(evaluated("render_panel.calls[-1]") == "start", "calls start()");
    CHECK(evaluated("render_panel.image_size()") == "(64, 32)", "the image it hands back is shown");
    CHECK(evaluated("render_status.text().startswith('Done')") == "True", "with its state: %s",
          evaluated("render_status.text()").c_str());
    CHECK(evaluated("render_button.text()") == "Stop", "Render became Stop");

    open.stage->usdStage()->DefinePrim(PXR_NS::SdfPath("/Added"), PXR_NS::TfToken("Sphere"));
    CHECK(waitFor("render_panel.calls[-1][0] == 'edited'"), "an edit calls stage_edited(), after the delay");
    CHECK(evaluated("render_panel.calls[-1][1]") == "['/Added']", "with what changed: %s",
          evaluated("render_panel.calls[-1]").c_str());

    CHECK(run("render_panel.report_error('It broke', fatal=True)"), "a fatal error");
    CHECK(evaluated("render_button.isChecked()") == "False", "stops rendering");
    CHECK(evaluated("render_panel.calls[-1][0]") == "edited", "without calling stop()");

    // Its plugin reloads (docs/PLUGIN_DEV.md): the open panel gets an instance of the
    // new class, and the old one is closed and let go.
    CHECK(run(R"(
import builtins, usdcc.app

class Probe2(render_probe_class):
    renderer_name = "Probe 2"

    def __init__(self):
        super().__init__()
        builtins.render_rebuilt = self

usdcc.app.register_view_panel("test.render-probe", "Render Probe", Probe2)
)"),
          "registering the type again, as a reload does");
    usdcc::scripting::rebuildScriptedPanels({"test.render-probe"});
    CHECK(evaluated("render_panel.calls[-1]") == "close", "rebuilding closes the old instance");
    CHECK(evaluated("render_panel.is_open()") == "False", "which lets go of the panel");
    CHECK(!run("render_panel.set_state('late')"), "so a late call from it raises");
    CHECK(evaluated("render_rebuilt.is_open() and render_rebuilt.stage == render_stage") == "True",
          "the new instance has the panel and its stage");
    CHECK(evaluated("render_rebuilt.extra.parent() is not None") == "True", "and its own control on the toolbar");
    CHECK(evaluated("render_rebuilt.widget.findChild(__import__('PySide6.QtWidgets').QtWidgets.QComboBox, "
                    "'renderResolution').count()") == "5",
          "the panel's own controls are still there");

    closeRenderPanels(window);
    CHECK(evaluated("render_rebuilt.calls.count('close')") == "1", "closing calls close() once");
    CHECK(evaluated("render_rebuilt.is_open()") == "False", "and the panel is gone");
    CHECK(!run("render_rebuilt.render_size()"), "so its calls raise");
    CHECK(evaluated("render_rebuilt.widget is None") == "True", "and it has no widget");

    CHECK(run("import builtins, usdcc.app\nusdcc.app.unregister_view_panel('test.render-probe')\n"
              "for name in [n for n in dir(builtins) if n.startswith('render_')]: delattr(builtins, name)\n"),
          "cleaning up");
}
