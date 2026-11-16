// SPDX-License-Identifier: BSD-3-Clause
#include "python_test_app.h"
#include "test_harness.h"
#include "usdcc/core/log.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <QAction>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QTemporaryDir>
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QTimer>
#include <QTest>
#include <QVariant>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/base/gf/vec3f.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
struct ProbeLog : usdcc::core::LogSink {
    std::mutex mutex;
    std::vector<std::string> messages;
    void onRecord(const usdcc::core::LogRecord& record) override {
        if (record.source != "plugin:org.usdcc.blender-mesh-tools") return;
        std::lock_guard lock(mutex);
        messages.push_back(record.message);
    }
    bool contains(const std::string& text) {
        std::lock_guard lock(mutex);
        for (const auto& message : messages)
            if (message.find(text) != std::string::npos) return true;
        return false;
    }
};
QAction* probeAction(const QList<QAction*>& actions) {
    for (auto* action : actions) {
        if (action->text() == QStringLiteral("Blender Mesh Backend Probe")) return action;
        if (action->menu())
            if (auto* found = probeAction(action->menu()->actions())) return found;
    }
    return nullptr;
}

USDCC_TEST(blender_mesh_native_plugin_real_menu_undo) {
    PXR_NAMESPACE_USING_DIRECTIVE
    auto& app = usdcc::test::pythonTestApp();
    auto stage = app.stages.adoptStage(UsdStage::CreateInMemory());
    app.stages.setCurrentStage(stage);
    auto mesh = UsdGeomMesh::Define(stage->usdStage(), SdfPath("/Mesh"));
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
    mesh.CreatePointsAttr().Set(VtVec3fArray{{0,0,0},{1,0,0},{1,1,0},{0,1,0}});
    mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{4}); mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray{0,1,2,3});
    stage->setSelectedPaths({mesh.GetPath()});
    std::string before; stage->usdStage()->GetRootLayer()->ExportToString(&before);
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdString()) / "plugins";
    std::filesystem::create_directories(root);
    std::filesystem::copy(std::filesystem::path(USDCC_BLENDER_MESH_PLUGIN), root / "blender_mesh_tools", std::filesystem::copy_options::recursive);
    usdcc::scripting::PluginManager::Options options; options.folders = {root};
    usdcc::scripting::PluginManager plugins(options); plugins.scan();
    usdcc::ui::MainWindow window; usdcc::ui::ViewPanelRegistry registry;
    const usdcc::scripting::AppContextScope context({&app.stages,&app.undo,&app.tools,&window,&registry,&plugins});
    plugins.activate(app.python);
    QAction* action = nullptr;
    for (auto* menuAction : window.mainMenuBar()->actions()) if (menuAction->text().remove('&') == "Mesh" && menuAction->menu()) {
        for (auto* candidate : menuAction->menu()->actions()) if (candidate->text().startsWith("Triangulate")) action = candidate;
    }
    CHECK(action, "actual Mesh/Triangulate menu action is registered");
    bool optionsShown = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog && dialog->objectName() == "BlenderMeshOptions") { optionsShown = true; dialog->accept(); }
    });
    if (action) action->trigger();
    auto* jobs = window.property("BlenderMeshJobs").value<QObject*>();
    CHECK(optionsShown && jobs, "menu uses the real options dialog and job controller");
    QElapsedTimer timeout; timeout.start();
    while (jobs && jobs->property("busy").toBool() && timeout.elapsed() < 20000) QTest::qWait(1);
    CHECK(jobs && !jobs->property("busy").toBool() && app.undo.stack(stage)->count() == 1,
          "actual menu dispatch commits one asynchronous undo step");
    VtIntArray counts; mesh.GetFaceVertexCountsAttr().Get(&counts);
    CHECK(counts == VtIntArray({3,3}), "live stage now contains triangles");
    const auto after = [&] { std::string s; stage->usdStage()->GetRootLayer()->ExportToString(&s); return s; }();
    app.undo.stack(stage)->undo();
    std::string restored; stage->usdStage()->GetRootLayer()->ExportToString(&restored);
    CHECK(restored == before, "menu edit undo restores exact original layer");
    app.undo.stack(stage)->redo(); stage->usdStage()->GetRootLayer()->ExportToString(&restored);
    CHECK(restored == after, "menu edit redo restores stored output");
    plugins.deactivate(app.python);
    app.stages.closeStage(stage);
}
}

USDCC_TEST(blender_mesh_native_plugin_lifecycle) {
    auto& app = usdcc::test::pythonTestApp();
    QTemporaryDir temporary;
    const QString file = temporary.filePath(QStringLiteral("scene.usda"));
    CHECK(QFile::copy(QStringLiteral(USDCC_TEST_DATA_DIR "/cube_and_sphere.usda"), file), "copy fixture");
    const auto stage = app.stages.openStage(file.toStdString());
    CHECK(stage, "open fixture stage");
    if (!stage) return;
    const auto root = stage->usdStage()->GetRootLayer();
    std::string before;
    root->ExportToString(&before);
    const int undoCount = app.undo.stack(stage)->count();
    const auto pluginRoot = std::filesystem::path(temporary.path().toStdString()) / "plugins";
    std::filesystem::create_directories(pluginRoot);
    std::filesystem::copy(std::filesystem::path(USDCC_BLENDER_MESH_PLUGIN), pluginRoot / "blender_mesh_tools",
                          std::filesystem::copy_options::recursive);

    // Fresh plugin manager and UI twice, but the DLL remains loaded under the
    // existing native lifecycle policy. Every probe creates and joins its worker.
    for (int cycle = 0; cycle < 2; ++cycle) {
        usdcc::scripting::PluginManager::Options options;
        options.folders = {pluginRoot};
        usdcc::scripting::PluginManager plugins(options);
        plugins.scan();
        usdcc::ui::MainWindow window;
        usdcc::ui::ViewPanelRegistry registry;
        const usdcc::scripting::AppContextScope context({&app.stages, &app.undo, &app.tools, &window, &registry, &plugins});
        auto log = std::make_shared<ProbeLog>();
        usdcc::core::LogHub::instance().addSink(log, false);
        plugins.activate(app.python);
        const auto* plugin = plugins.find("org.usdcc.blender-mesh-tools");
        CHECK(plugin && plugin->state == usdcc::scripting::PluginState::Active, "activate: %s",
              plugin ? plugin->reason.c_str() : "not found");
        CHECK(log->contains("native mesh backend active"), "native activation probe passes");
        auto* action = probeAction(window.mainMenuBar()->actions());
        CHECK(action, "diagnostic action is registered through the SDK");
        if (action) action->trigger();
        CHECK(log->contains("\"iterations\":100"), "actual menu action runs the native backend");
        CHECK(log->contains("\"allocator_blocks_delta\":0"), "guarded allocations return to baseline");
        plugins.deactivate(app.python);
        CHECK(log->contains("native mesh backend shutdown probe passed"), "shutdown hook runs a fresh backend cycle");
    }
    std::string after;
    root->ExportToString(&after);
    CHECK(before == after && !root->IsDirty(), "phase 1 diagnostics never author USD");
    CHECK(app.undo.stack(stage)->count() == undoCount, "probe has no undo step");
    app.stages.closeStage(stage);
}
