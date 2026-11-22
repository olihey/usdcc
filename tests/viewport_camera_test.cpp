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
#include "usdcc/usd/viewport_display.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>

#include <QAction>
#include <QCoreApplication>
#include <QJsonObject>
#include <QUndoStack>
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

namespace {
// A camera 10 units back on +Z looking at the origin, turned 15 degrees about its view,
// and a box to look at (docs/VIEWPORT_CAMERA.md).
const char* kCameraScene = R"(#usda 1.0
def Cube "Box"
{
    double size = 2
}

def Xform "Rig"
{
    double3 xformOp:translate = (0, 1, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]

    def Camera "Shot"
    {
        float focalLength = 35
        double3 xformOp:translate = (0, 0, 10)
        float3 xformOp:rotateXYZ = (0, 0, 15)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
}
)";

bool closeMatrix(const PXR_NS::GfMatrix4d& a, const PXR_NS::GfMatrix4d& b, double tolerance = 1e-5) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::abs(a[r][c] - b[r][c]) > tolerance * std::max(1.0, std::abs(b[r][c]))) return false;
    return true;
}

PXR_NS::GfMatrix4d shotWorld(const StageRefPtr& stage) {
    return PXR_NS::UsdGeomCamera(stage->usdStage()->GetPrimAtPath(PXR_NS::SdfPath("/Rig/Shot")))
        .GetCamera(PXR_NS::UsdTimeCode::Default())
        .GetTransform();
}

std::string viewportCamera() { return evaluated("str(__import__('usdcc.app').app.active_viewport().camera)"); }
}  // namespace

USDCC_TEST(viewport_looks_through_a_stage_camera) {
    auto& app = pythonTestApp();
    const AppContextScope scope({&app.stages, &app.undo, &app.tools});
    OpenStage open("look_through", kCameraScene);
    auto viewport = makeViewport(open.stage);
    HydraViewportWindow* window = viewport->hydraWindow();
    const HydraViewportWindow::CameraState orbit{PXR_NS::GfVec3d(1, 2, 3), 20.0, 10.0, -30.0};
    window->setCameraState(orbit);

    const PXR_NS::SdfPath shot("/Rig/Shot");
    CHECK(!viewport->lookThrough(PXR_NS::SdfPath("/Box")), "a cube isn't a camera");
    CHECK(viewport->lookThrough(shot) && window->stageCamera() == shot, "looking through the camera");
    CHECK(closeMatrix(window->frustum().ComputeViewInverse(), shotWorld(open.stage)), "the view is the camera's");
    CHECK(window->hudInput().camera == QStringLiteral("Shot"), "the HUD names it: %s", window->hudInput().camera.toStdString().c_str());
    CHECK(viewportCamera() == "/Rig/Shot", "Python sees it: %s", viewportCamera().c_str());

    // Moving the camera's parent moves the view.
    bool resets = false;
    PXR_NS::UsdGeomXformable(open.stage->usdStage()->GetPrimAtPath(PXR_NS::SdfPath("/Rig")))
        .GetOrderedXformOps(&resets)
        .front()
        .Set(PXR_NS::GfVec3d(0, 5, 0));
    CHECK(std::abs(window->frustum().GetPosition()[1] - 5.0) < 1e-6, "the view follows the camera: %f", window->frustum().GetPosition()[1]);

    // None in Python: the orbit camera, where it was.
    CHECK(run("import usdcc.app\nusdcc.app.active_viewport().camera = None\n"), "back to its own camera");
    CHECK(window->stageCamera().IsEmpty() && window->cameraState().target == orbit.target && window->cameraState().yaw == orbit.yaw,
          "where it was");
    CHECK(viewportCamera() == "None", "which Python says: %s", viewportCamera().c_str());
    CHECK(run("import usdcc.app\nusdcc.app.active_viewport().camera = '/Rig/Shot'\n"), "and through the camera again");
    CHECK(window->stageCamera() == shot, "from Python");
    CHECK(run("import usdcc.app\ntry:\n    usdcc.app.active_viewport().camera = '/Box'\n    raise RuntimeError('no error')\n"
              "except ValueError:\n    pass\n"),
          "a ValueError for a cube");
    CHECK(window->stageCamera() == shot, "which changes nothing");
    viewport.reset();
}

USDCC_TEST(viewport_detaches_from_a_stage_camera) {
    auto& app = pythonTestApp();
    const AppContextScope scope({&app.stages, &app.undo, &app.tools});
    OpenStage open("detach", kCameraScene);
    auto viewport = makeViewport(open.stage);
    HydraViewportWindow* window = viewport->hydraWindow();
    const PXR_NS::SdfPath shot("/Rig/Shot");
    viewport->lookThrough(shot);
    const PXR_NS::GfFrustum through = window->frustum();

    window->detachFromStageCamera();
    CHECK(window->stageCamera().IsEmpty() && window->detachedFrom() == shot, "detached, remembering the camera");
    CHECK(closeMatrix(window->frustum().ComputeViewInverse(), through.ComputeViewInverse()), "seeing from the same place, rolled too");
    const PXR_NS::GfRange2d a = window->frustum().GetWindow(), b = through.GetWindow();
    CHECK((a.GetMin() - b.GetMin()).GetLength() < 1e-6 && (a.GetMax() - b.GetMax()).GetLength() < 1e-6, "as much of it");
    // Turned +15 about its view (Z), its up leans left: a roll of -15 (orbitCameraAxes()).
    CHECK(std::abs(window->cameraState().roll + 15.0) < 1e-4, "with the camera's roll: %f", window->cameraState().roll);
    CHECK(closeMatrix(shotWorld(open.stage), through.ComputeViewInverse()), "the camera stays where it was");
    CHECK(window->hudInput().camera == QStringLiteral("Persp ") + QChar(0x2190) + QStringLiteral(" Shot"), "the HUD says where from: %s",
          window->hudInput().camera.toStdString().c_str());

    // Reset Camera: back to the camera first, then the default view.
    viewport->resetCamera();
    CHECK(window->stageCamera() == shot, "Reset Camera goes back to the camera");
    viewport->resetCamera();
    CHECK(window->stageCamera().IsEmpty() && window->detachedFrom().IsEmpty() &&
              window->cameraState().yaw == usdcc::usd::kDefaultOrbitYaw &&
              window->fieldOfView() == HydraViewportWindow::verticalFieldOfView(),
          "and then to the default view and field of view");

    // A view snapped from the triad forgets the camera.
    viewport->lookThrough(shot);
    window->snapToAxis(usdcc::usd::HudItem::AxisCenter);
    CHECK(window->stageCamera().IsEmpty() && window->detachedFrom().IsEmpty(), "another view: nothing to go back to");

    // The camera going: on from where it was.
    viewport->lookThrough(shot);
    open.stage->usdStage()->GetPrimAtPath(shot).SetActive(false);
    QCoreApplication::processEvents();
    CHECK(window->stageCamera().IsEmpty() && window->detachedFrom().IsEmpty(), "a deactivated camera is left");
    viewport.reset();
}

// A camera rig scaled to centimetres in a millimetre stage: its focus distance is in its
// own space, so the orbit target is a tenth of it away, on what it looks at.
USDCC_TEST(viewport_detaches_onto_a_scaled_cameras_focus) {
    auto& app = pythonTestApp();
    const AppContextScope scope({&app.stages, &app.undo, &app.tools});
    OpenStage open("detach_scaled", R"(#usda 1.0
(
    metersPerUnit = 0.001
)
def Cube "Bottle"
{
    double size = 10
}

def Xform "cameras"
{
    float3 xformOp:scale = (0.1, 0.1, 0.1)
    uniform token[] xformOpOrder = ["xformOp:scale"]

    def Camera "default"
    {
        float focalLength = 200
        float focusDistance = 651
        matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, -651, 0, 1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }
}
)");
    auto viewport = makeViewport(open.stage);
    HydraViewportWindow* window = viewport->hydraWindow();
    CHECK(viewport->lookThrough(PXR_NS::SdfPath("/cameras/default")), "looking through the camera");
    window->detachFromStageCamera();
    const HydraViewportWindow::CameraState state = window->cameraState();
    CHECK(std::abs(state.distance - 65.1) < 1e-3, "the focus distance in the world: %f", state.distance);
    CHECK(state.target.GetLength() < 1e-3, "orbiting the bottle: %f %f %f", state.target[0], state.target[1], state.target[2]);
    viewport.reset();
}

USDCC_TEST(viewport_copies_its_view_to_a_camera) {
    auto& app = pythonTestApp();
    const AppContextScope scope({&app.stages, &app.undo, &app.tools});
    OpenStage open("copy_view", kCameraScene);
    auto viewport = std::make_unique<ViewportViewPanel>(&app.stages, open.stage, std::make_shared<usdcc::tools::ToolSet>(),
                                                        std::shared_ptr<usdcc::usd::UndoManager>(&app.undo, [](auto*) {}));
    HydraViewportWindow* window = viewport->hydraWindow();
    const PXR_NS::SdfPath shot("/Rig/Shot");
    viewport->lookThrough(shot);
    window->detachFromStageCamera();
    window->setFieldOfView(30.0);  // another lens, still detached from the camera
    CHECK(window->detachedFrom() == shot, "still detached from it");
    const PXR_NS::GfMatrix4d view = window->frustum().ComputeViewInverse();
    const PXR_NS::GfRange2d viewWindow = window->frustum().GetWindow();

    QUndoStack* undo = app.undo.stack(open.stage);
    const int steps = undo->count();
    CHECK(viewport->copyViewToCamera(), "copying the view to the camera");
    CHECK(undo->count() == steps + 1, "one undo step: %d", undo->count() - steps);
    CHECK(window->stageCamera() == shot, "looking through it again");
    const PXR_NS::GfRange2d through = window->frustum().GetWindow();
    CHECK(closeMatrix(window->frustum().ComputeViewInverse(), view) && (through.GetMax() - viewWindow.GetMax()).GetLength() < 1e-5,
          "which sees what the view did");
    undo->undo();
    CHECK(std::abs(window->frustum().GetWindow().GetMax()[1] - viewWindow.GetMax()[1]) > 1e-3, "undone, the lens is back");

    // New Camera from View.
    viewport->lookThrough({});
    const PXR_NS::GfMatrix4d orbitView = window->frustum().ComputeViewInverse();
    const PXR_NS::SdfPath made = viewport->newCameraFromView();
    CHECK(made == PXR_NS::SdfPath("/Camera"), "a new camera at the root: %s", made.GetText());
    CHECK(undo->count() == steps + 1, "one undo step, after the undone one: %d", undo->count() - steps);
    CHECK(window->stageCamera() == made && closeMatrix(window->frustum().ComputeViewInverse(), orbitView),
          "looked through, seeing the same");
    viewport.reset();
}

USDCC_TEST(viewport_camera_in_sessions_and_workspaces) {
    auto& app = pythonTestApp();
    const AppContextScope scope({&app.stages, &app.undo, &app.tools});
    OpenStage open("camera_session", kCameraScene);
    auto viewport = makeViewport(open.stage);
    viewport->lookThrough(PXR_NS::SdfPath("/Rig/Shot"));
    const QJsonObject saved = viewport->saveSessionState();
    CHECK(saved.value(QStringLiteral("camera")).toObject().value(QStringLiteral("path")).toString() == QStringLiteral("/Rig/Shot"),
          "the session keeps the camera");
    CHECK(viewport->workspaceCamera().value(QStringLiteral("path")).toString() == QStringLiteral("/Rig/Shot"), "and a workspace");

    auto restored = makeViewport(open.stage);
    restored->restoreSessionState(saved);
    // A viewport that hasn't drawn applies the session once it has renderers.
    if (restored->hydraWindow()->stageCamera().IsEmpty()) Q_EMIT restored->hydraWindow()->rendererPluginsChanged();
    CHECK(restored->hydraWindow()->stageCamera() == PXR_NS::SdfPath("/Rig/Shot"), "restored, it looks through it");

    auto spec = makeViewport(open.stage);
    spec->applyWorkspaceCamera(QJsonObject{{QStringLiteral("path"), QStringLiteral("first")}});
    CHECK(spec->hydraWindow()->stageCamera() == PXR_NS::SdfPath("/Rig/Shot"), "a workspace's \"first\" camera");
    spec.reset();
    restored.reset();
    viewport.reset();
}

// The resolution gate (docs/VIEWPORT_CAMERA.md §4): the viewport sees the gate's shape, and
// the window around it more; the camera's field of view goes to the gate.
USDCC_TEST(viewport_resolution_gate) {
    OpenStage open("gate");
    auto viewport = makeViewport(open.stage);
    HydraViewportWindow* window = viewport->hydraWindow();
    window->resize(400, 900);
    const PXR_NS::GfFrustum filled = window->frustum();
    CHECK((window->gate() == usdcc::usd::ViewRect{0, 0, 400, 900}) && !window->gateResolution(), "Fill Viewport: the window");

    usdcc::usd::ViewportDisplayOverrides overrides;
    overrides.resolution = "1920x1080";
    viewport->setDisplayOverrides(overrides);
    const usdcc::usd::ViewRect gate = window->gate();
    CHECK((window->gateResolution() == usdcc::usd::Resolution{1920, 1080}) && std::abs(gate.height - 225.0) < 1e-6 &&
              std::abs(gate.y - 337.5) < 1e-6,
          "a 16:9 gate in a tall viewport: as wide, centred");
    const PXR_NS::GfFrustum gated = window->frustum();
    const PXR_NS::GfVec2d size = gated.GetWindow().GetSize();
    CHECK(std::abs(size[0] / size[1] - 400.0 / 900.0) < 1e-6, "the frustum still covers the whole window");
    // The orbit camera's vertical field of view now spans the gate, so the window sees 4 times as much up and down.
    const double filledHeight = filled.GetWindow().GetSize()[1];
    CHECK(std::abs(size[1] / filledHeight - 4.0) < 1e-6, "the field of view spans the gate: %g", size[1] / filledHeight);
    viewport.reset();
}
