// The native plugin must activate and report missing SDKs without creating a
// Vulkan context or loading the NVIDIA renderer. Safe on non-GPU CI machines.
#include <QFile>
#include <QComboBox>
#include <QLabel>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUndoStack>
#include <filesystem>
#include <memory>

#include "python_test_app.h"
#include "test_harness.h"
#include "test_settings.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/render_view_panel.h"
#include "usdcc/usd/undo_manager.h"

USDCC_TEST(rtx_native_plugin_deferred_initialization) {
    TestSettings settings;
    {
        auto stored = usdcc::ui::appSettings();
        stored.setValue("RtxRender/maxFrames", 37);
        stored.setValue("RtxRender/frameBudget", 91);
        stored.sync();
    }
    usdcc::core::Settings::instance().setBackend(std::make_unique<usdcc::ui::QSettingsBackend>(TestSettings::path()));
    auto& app = usdcc::test::pythonTestApp();
    QTemporaryDir folder;
    const auto source = std::filesystem::u8path(USDCC_NATIVE_RTX_PLUGIN);
    const auto copy = std::filesystem::u8path(folder.path().toStdString()) / "native_rtx";
    std::filesystem::create_directories(copy / "bin/windows-x64");
    std::filesystem::copy_file(source / "usdcc-plugin.json", copy / "usdcc-plugin.json");
    std::filesystem::copy_file(source / "bin/windows-x64/rtx_render_native.dll",
                               copy / "bin/windows-x64/rtx_render_native.dll");
    QFile runtime(QString::fromStdU16String((copy / "runtime.json").u16string()));
    CHECK(runtime.open(QIODevice::WriteOnly), "test runtime locator");
    runtime.write("{\"ovrtx\":\"C:/missing-rtx-m1\",\"ovstage\":\"C:/missing-stage-m1\"}");
    runtime.close();
    const QByteArray oldRtx = qgetenv("USDCC_RTX_OVRTX_ROOT");
    const QByteArray oldStage = qgetenv("USDCC_RTX_OVSTAGE_ROOT");
    qunsetenv("USDCC_RTX_OVRTX_ROOT");
    qunsetenv("USDCC_RTX_OVSTAGE_ROOT");

    usdcc::scripting::PluginManager::Options options;
    const auto legacyRoot = std::filesystem::u8path(folder.path().toStdString()) / "old-user-copy";
    std::filesystem::create_directories(legacyRoot / "rtx");
    QFile legacy(QString::fromStdU16String((legacyRoot / "rtx/usdcc-plugin.json").u16string()));
    legacy.open(QIODevice::WriteOnly);
    legacy.write(R"({"id":"org.usdcc.rtx-render","name":"Old RTX","version":"0.2.0","api":1,
        "python":{"module":"usdcc_rtx_render","requires":["ovrtx>=0.6","ovstage","numpy"]}})");
    legacy.close();
    options.folders = {legacyRoot, copy.parent_path()};
    usdcc::scripting::PluginManager manager(options);
    manager.scan();
    usdcc::ui::MainWindow window;
    usdcc::ui::ViewPanelRegistry registry;
    const usdcc::scripting::AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &registry, &manager});
    manager.activate(app.python);
    CHECK(usdcc::core::Settings::instance().get<int>("RtxRender/sampleBudget") == 37,
          "legacy frame budget seeds Path Tracing samples");
    CHECK(usdcc::core::Settings::instance().get<int>("RtxRender/frameBudget") == 91,
          "migration preserves an existing native frame budget");
    const auto* plugin = manager.find("org.usdcc.rtx-render");
    CHECK(plugin && plugin->manifest.pythonModule.empty() && plugin->manifest.folder == copy,
          "a shadowing Python RTX user copy is ignored in favor of the native artifact");
    CHECK(manager.packageRequests(app.python, nullptr).empty(), "native RTX asks for no Python package installation");
    CHECK(plugin && plugin->state == usdcc::scripting::PluginState::Active,
          "a native plugin activates without runtime DLLs: %s", plugin ? plugin->reason.c_str() : "missing");
    const auto* entry = registry.find(QStringLiteral("org.usdcc.rtx-render.view"));
    CHECK(entry, "the saved panel identity is registered");
    if (entry) {
        auto stage = app.stages.createStage();
        {
            std::unique_ptr<usdcc::ui::ViewPanel> panel(entry->create(stage, "Native RTX test", &window));
            auto* render = panel->findChild<QToolButton*>("renderButton");
            auto* status = panel->findChild<QLabel*>("renderStatus");
            CHECK(render && render->isEnabled(), "creating a panel does not initialize a renderer");
            CHECK(status && render && !render->isChecked(), "initial state is idle");
            if (render && status) {
                render->click();
                CHECK(status->text().contains("runtime missing") && status->text().contains("runtime.json"),
                      "Render reports an actionable missing-runtime error before GPU initialization");
                CHECK(!panel->property("nativeRtxImageSize").toSize().isValid(), "no image claimed on failure");
                CHECK(render->isEnabled(), "failure permits a retry");
                std::unique_ptr<usdcc::ui::ViewPanel> duplicate(panel->duplicate(&window));
                CHECK(duplicate->currentStage() == stage, "duplicate keeps its stage without initializing the GPU");
                auto* originalMode = panel->findChild<QComboBox*>("nativeRtxMode");
                auto* duplicateMode = duplicate->findChild<QComboBox*>("nativeRtxMode");
                duplicateMode->setCurrentIndex((originalMode->currentIndex() + 1) % 3);
                CHECK(duplicateMode->currentIndex() != originalMode->currentIndex(),
                      "duplicate panels choose modes independently");
                CHECK(usdcc::core::Settings::instance().get<std::string>("RtxRender/mode") == "Real-Time",
                      "panel mode does not overwrite the default for new panels");
                const auto selectionBefore = app.stages.selectedPaths(stage);
                const int undoBefore = app.undo.stack(stage)->count();
                panel->findChild<QToolButton*>("nativeRtxReset")->click();
                auto* originalRender = qobject_cast<usdcc::usd::RenderViewPanel*>(panel.get());
                std::unique_ptr<usdcc::ui::ViewPanel> freeCopy(panel->duplicate(&window));
                auto* copiedRender = qobject_cast<usdcc::usd::RenderViewPanel*>(freeCopy.get());
                CHECK(originalRender->usesLocalCamera() && copiedRender->usesLocalCamera() &&
                      originalRender->currentCamera().cameraToWorld == copiedRender->currentCamera().cameraToWorld,
                      "duplicating Free Camera preserves its pose");
                auto moved = originalRender->currentCamera();
                moved.cameraToWorld.SetTranslateOnly(PXR_NS::GfVec3d(50, 40, 30));
                originalRender->setLocalCamera(moved);
                CHECK(originalRender->currentCamera().cameraToWorld != copiedRender->currentCamera().cameraToWorld,
                      "duplicate panels own independent viewing cameras");
                CHECK(app.stages.selectedPaths(stage) == selectionBefore && app.undo.stack(stage)->count() == undoBefore,
                      "camera reset/navigation does not change selection or undo history");
                panel->setStage({});
                CHECK(!render->isChecked(), "stage close stops rendering");
                panel->setStage(stage);
                CHECK(render->isEnabled(), "another stage enables rendering again");
            }
        }
        // This is the saved entry format used by the retired Python panel.
        const QVariantMap saved{{"type", "org.usdcc.rtx-render.view"}, {"name", "legacy-rtx-dock"},
                                {"title", "RTX Render - previous session"}, {"stage", "legacy-stage.usda"}};
        usdcc::ui::appSettings().setValue("ViewPanels/opened", QVariantList{saved});
        registry.restorePanels(window.dockManager(), &window, [&](const QString& id) {
            return id == "legacy-stage.usda" ? stage : usdcc::usd::StageRefPtr{};
        });
        auto* restored = window.findChild<usdcc::usd::RenderViewPanel*>("legacy-rtx-dock");
        CHECK(restored && restored->currentStage() == stage &&
              restored->property("nativeRtxImageSize").isValid(), "an old layout entry restores a native RTX panel and its stage");
        CHECK(restored && !restored->findChild<QToolButton*>("renderButton")->isChecked(),
              "restoring a legacy layout never starts SDK/GPU work");
        delete restored;
        app.stages.closeStage(stage);
    }
    CHECK(app.python.evaluate("'usdcc_rtx_render' in __import__('sys').modules") == "False",
          "the native plugin does not import the old Python renderer");
    manager.deactivate(app.python);
    if (oldRtx.isNull())
        qunsetenv("USDCC_RTX_OVRTX_ROOT");
    else
        qputenv("USDCC_RTX_OVRTX_ROOT", oldRtx);
    if (oldStage.isNull())
        qunsetenv("USDCC_RTX_OVSTAGE_ROOT");
    else
        qputenv("USDCC_RTX_OVSTAGE_ROOT", oldStage);
}
