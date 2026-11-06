// What a script sees of the viewports and of edits: usdcc.app.active_viewport_camera(),
// and the "viewport_camera_changed" and "stage_edited" events (docs/SCRIPTING.md,
// used by the V-Ray render plugin, docs/VRAY_RENDER.md). And Fit to View (F).
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/usd/usd/stage.h>

#include <QAction>
#include <QCoreApplication>
#include <QWidget>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using usdcc::scripting::AppContextScope;
using usdcc::test::pythonTestApp;
using usdcc::usd::HydraViewportWindow;
using usdcc::usd::StageRefPtr;
using usdcc::usd::ViewportViewPanel;

namespace {
struct OpenStage {
    std::filesystem::path file;
    StageRefPtr stage;
    explicit OpenStage(const std::string& name, const std::string& text = "#usda 1.0\ndef Xform \"A\"\n{\n}\n")
        : file(std::filesystem::temp_directory_path() /
               ("usdcc_viewport_" + name + "_" + std::to_string(QCoreApplication::applicationPid()) + ".usda")) {
        std::ofstream(file) << text;
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

std::unique_ptr<ViewportViewPanel> makeViewport(const StageRefPtr& stage) {
    return std::make_unique<ViewportViewPanel>(&pythonTestApp().stages, stage, std::make_shared<usdcc::tools::ToolSet>(),
                                               nullptr);
}

// The camera position Python reports, as "x y z" rounded to 3 decimals.
std::string pythonPosition(const std::string& camera) {
    return evaluated("' '.join(f'{v + 0.0:.3f}' for v in " + camera + ".transform.ExtractTranslation())");
}

std::string position(const HydraViewportWindow* window) {
    const PXR_NS::GfVec3d p = window->frustum().GetPosition();
    char text[96];
    std::snprintf(text, sizeof(text), "%.3f %.3f %.3f", p[0] + 0.0, p[1] + 0.0, p[2] + 0.0);
    return text;
}
}  // namespace

USDCC_TEST(active_viewport_camera) {
    auto& app = pythonTestApp();
    const AppContextScope scope({&app.stages, &app.undo, &app.tools});
    CHECK(evaluated("__import__('usdcc.app').app.active_viewport_camera()") == "None", "None with no viewport");

    OpenStage open("camera");
    auto first = makeViewport(open.stage);
    CHECK(ViewportViewPanel::activeViewport() == first.get(), "the only viewport is the active one");
    CHECK(run(R"(
import builtins, usdcc.app, usdcc.usd
builtins.usdcc_test_cameras = []
builtins.usdcc_test_camera_connection = usdcc.app.connect(
    "viewport_camera_changed", lambda camera: builtins.usdcc_test_cameras.append(camera))
camera = usdcc.app.active_viewport_camera()
builtins.usdcc_test_camera = camera
)"), "reading the camera");
    CHECK(evaluated("usdcc_test_camera.stage == __import__('usdcc.usd').usd.current_stage()") == "True",
          "with the viewport's stage");
    CHECK(evaluated("usdcc_test_camera.fov_y") == "45.0", "its vertical field of view: %s",
          evaluated("usdcc_test_camera.fov_y").c_str());
    CHECK(pythonPosition("usdcc_test_camera") == position(first->hydraWindow()), "and where it is: %s vs %s",
          pythonPosition("usdcc_test_camera").c_str(), position(first->hydraWindow()).c_str());
    CHECK(evaluated("abs(usdcc_test_camera.fov_x() - 45.0) < 1e-6 if usdcc_test_camera.aspect == 1.0 else True") == "True",
          "fov_x() follows the aspect ratio");

    first->hydraWindow()->setCameraState({PXR_NS::GfVec3d(1.0, 2.0, 3.0), 20.0, 10.0, -30.0});
    CHECK(evaluated("len(usdcc_test_cameras)") == "1", "moving the camera tells: %s", evaluated("len(usdcc_test_cameras)").c_str());
    CHECK(pythonPosition("usdcc_test_cameras[-1]") == position(first->hydraWindow()), "with the new camera");
    CHECK(evaluated("abs(usdcc_test_cameras[-1].transform.ExtractTranslation().GetLength() - 0) > 1") == "True",
          "which has moved away from the origin");

    auto second = makeViewport(open.stage);
    CHECK(ViewportViewPanel::activeViewport() == first.get(), "a new viewport doesn't take over");
    second->hydraWindow()->setCameraState({PXR_NS::GfVec3d(-5.0, 0.0, 0.0), 5.0, 90.0, 0.0});
    CHECK(evaluated("len(usdcc_test_cameras)") == "1", "nor does moving its camera tell");
    Q_EMIT second->hydraWindow()->pressed();
    CHECK(ViewportViewPanel::activeViewport() == second.get(), "clicking it makes it the active one");
    CHECK(evaluated("len(usdcc_test_cameras)") == "2", "which tells");
    CHECK(pythonPosition("usdcc_test_cameras[-1]") == position(second->hydraWindow()), "with its camera");
    Q_EMIT second->hydraWindow()->pressed();
    CHECK(evaluated("len(usdcc_test_cameras)") == "2", "clicking it again doesn't");

    second.reset();
    CHECK(ViewportViewPanel::activeViewport() == first.get(), "closing it hands back to the other");
    CHECK(evaluated("len(usdcc_test_cameras)") == "3", "which tells");
    first.reset();
    CHECK(ViewportViewPanel::activeViewport() == nullptr, "none left");
    CHECK(evaluated("usdcc_test_cameras[-1] is None") == "True", "and the callback gets None");

    CHECK(run("import builtins\nbuiltins.usdcc_test_camera_connection.disconnect()\n"), "disconnecting");
    auto third = makeViewport(open.stage);
    CHECK(evaluated("len(usdcc_test_cameras)") == "4", "ends the calls: %s", evaluated("len(usdcc_test_cameras)").c_str());
}

USDCC_TEST(stage_edited_event) {
    auto& app = pythonTestApp();
    const AppContextScope scope({&app.stages, &app.undo, &app.tools});
    OpenStage before("edited_before");
    CHECK(run(R"(
import builtins, usdcc.app
builtins.usdcc_test_edits = []
def edited(stage, resynced, changed):
    builtins.usdcc_test_edits.append((stage.GetRootLayer().identifier, [str(p) for p in resynced], [str(p) for p in changed]))
builtins.usdcc_test_edit_connection = usdcc.app.connect("stage_edited", edited)
)"), "connecting");

    before.stage->usdStage()->DefinePrim(PXR_NS::SdfPath("/New"), PXR_NS::TfToken("Xform"));
    CHECK(evaluated("usdcc_test_edits[-1][1]") == "['/New']", "a new prim is a resync: %s", evaluated("usdcc_test_edits").c_str());
    CHECK(evaluated("usdcc_test_edits[-1][0].endswith('edited_before_' + str(__import__('os').getpid()) + '.usda')") == "True",
          "of that stage");
    before.stage->usdStage()->GetPrimAtPath(PXR_NS::SdfPath("/A")).SetMetadata(PXR_NS::TfToken("comment"), std::string("hi"));
    CHECK(evaluated("usdcc_test_edits[-1][2]") == "['/A']", "metadata is an info change: %s", evaluated("usdcc_test_edits[-1]").c_str());

    {
        OpenStage later("edited_later");
        later.stage->usdStage()->DefinePrim(PXR_NS::SdfPath("/Later"), PXR_NS::TfToken("Xform"));
        CHECK(evaluated("usdcc_test_edits[-1][1]") == "['/Later']", "a stage opened afterwards is followed too");
    }
    CHECK(run("import builtins\nbuiltins.usdcc_test_edit_connection.disconnect()\nbuiltins.usdcc_test_edits.clear()\n"),
          "disconnecting");
    before.stage->usdStage()->DefinePrim(PXR_NS::SdfPath("/AfterDisconnect"), PXR_NS::TfToken("Xform"));
    CHECK(evaluated("len(usdcc_test_edits)") == "0", "ends the calls");
}

namespace {
struct ToastSink : usdcc::core::LogSink {
    std::vector<usdcc::core::LogRecord> toasts;
    void onRecord(const usdcc::core::LogRecord& record) override {
        if (record.toast) toasts.push_back(record);
    }
};

bool closeTo(const PXR_NS::GfVec3d& a, const PXR_NS::GfVec3d& b) { return (a - b).GetLength() < 1e-6; }

const char* kFitScene = R"(#usda 1.0
def Cube "Box"
{
    double size = 2
    double3 xformOp:translate = (10, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}

def Sphere "Ball"
{
    double radius = 1
    double3 xformOp:translate = (-10, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}

def Scope "Looks"
{
}
)";
}  // namespace

USDCC_TEST(fit_to_view) {
    auto& app = pythonTestApp();
    OpenStage open("fit", kFitScene);
    OpenStage empty("fit_empty");
    auto viewport = makeViewport(open.stage);
    HydraViewportWindow* window = viewport->hydraWindow();
    auto sink = std::make_shared<ToastSink>();
    usdcc::core::LogHub::instance().addSink(sink, false);

    const HydraViewportWindow::CameraState start{PXR_NS::GfVec3d(1, 2, 3), 50.0, 30.0, 15.0};
    window->setCameraState(start);
    app.stages.setSelectedPaths(open.stage, {});
    CHECK(viewport->fitToView(), "nothing selected: fits the whole stage");
    HydraViewportWindow::CameraState state = window->cameraState();
    CHECK(closeTo(state.target, PXR_NS::GfVec3d(0, 0, 0)), "aimed at its middle");
    const double halfFov = 22.5 * 3.14159265358979323846 / 180.0;
    CHECK(std::abs(state.distance - std::sqrt(22.0 * 22.0 + 2.0 * 2.0 + 2.0 * 2.0) / 2.0 / std::sin(halfFov)) < 1e-6,
          "from far enough to see both ends: %f", state.distance);
    CHECK(state.yaw == start.yaw && state.pitch == start.pitch, "still looking the same way");

    app.stages.setSelectedPaths(open.stage, {PXR_NS::SdfPath("/Box"), PXR_NS::SdfPath("/Looks")});
    CHECK(viewport->fitToView(), "selected prims: fits those with bounds");
    state = window->cameraState();
    CHECK(closeTo(state.target, PXR_NS::GfVec3d(10, 0, 0)), "the box");
    CHECK(std::abs(state.distance - std::sqrt(12.0) / 2.0 / std::sin(halfFov)) < 1e-6, "close up: %f", state.distance);
    CHECK(sink->toasts.empty(), "no toast while something was framed");

    app.stages.setSelectedPaths(open.stage, {PXR_NS::SdfPath("/Looks")});
    CHECK(!viewport->fitToView(), "only prims without bounds: nothing to fit");
    CHECK(closeTo(window->cameraState().target, PXR_NS::GfVec3d(10, 0, 0)) && window->cameraState().distance == state.distance,
          "the camera stays");
    CHECK(sink->toasts.size() == 1 && sink->toasts.back().toast->kind == usdcc::core::ToastKind::Warning &&
              sink->toasts.back().toast->title == "Nothing to fit to view" &&
              sink->toasts.back().message == "Looks has no bounds.",
          "a warning toast says why: %s", sink->toasts.empty() ? "none" : sink->toasts.back().message.c_str());

    // The F key: one application-wide action, for the active viewport.
    app.stages.setSelectedPaths(open.stage, {PXR_NS::SdfPath("/Ball")});
    {
        QWidget mainWindow;
        QAction* action = ViewportViewPanel::addFitToViewAction(&mainWindow);
        CHECK(action->shortcut() == QKeySequence(Qt::Key_F), "F by default: %s",
              action->shortcut().toString().toStdString().c_str());
        CHECK(usdcc::ui::settingShortcut(usdcc::usd::kFitToViewShortcutKey) == QKeySequence(Qt::Key_F), "a Shortcut setting");
        CHECK(action->shortcutContext() == Qt::ApplicationShortcut && mainWindow.actions().contains(action),
              "on the main window, wherever the focus is");
        action->trigger();
        CHECK(closeTo(window->cameraState().target, PXR_NS::GfVec3d(-10, 0, 0)), "triggering it fits the active viewport");
    }

    viewport->setStage(empty.stage);
    app.stages.setSelectedPaths(empty.stage, {});
    CHECK(!viewport->fitToView(), "a stage with no geometry: nothing to fit");
    CHECK(sink->toasts.size() == 2 && sink->toasts.back().message == "The stage has no geometry.", "which a toast says: %s",
          sink->toasts.empty() ? "none" : sink->toasts.back().message.c_str());

    usdcc::core::LogHub::instance().removeSink(sink.get());
    viewport.reset();
}
