#include <QApplication>

#include "app_updates.h"
#include "startup.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <tuple>

#include "usdcc/ui/about_dialog.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/current_stage_tracker.h"
#include "usdcc/ui/log_side_panel.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/peer_rooms_debug_dialog.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/ui/peers_side_panel.h"
#include "usdcc/ui/qt_message_bridge.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/settings_dialog.h"
#include "usdcc/ui/theme.h"
#include "usdcc/ui/toast_manager.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/ui/workspace_applier.h"
#include "usdcc/ui/workspace_controller.h"
#include "usdcc/ui/workspace_library.h"

#include <DockAreaWidget.h>
#include <DockManager.h>

#include <QAction>
#include <QDesktopServices>
#include <QCommandLineParser>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QStatusBar>
#include <QToolButton>
#include <QUrl>
#include <QSurfaceFormat>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include "usdcc/core/console_log_sink.h"
#include "usdcc/core/file_log_sink.h"
#include "usdcc/core/log.h"
#include "usdcc/core/native_stream_capture.h"
#include "usdcc/core/settings.h"
#include "usdcc/core/version.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/plugin_manager_side_panel.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/python_settings.h"
#include "usdcc/scripting/script_runner.h"
#include "usdcc/scripting/script_editor_side_panel.h"
#include "usdcc/scripting/session_controller.h"
#include "usdcc/scripting/sessions_dialog.h"
#include "usdcc/scripting/startup_scripts.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/open_stage_options_dialog.h"
#include "usdcc/usd/asset_settings.h"
#include "usdcc/usd/asset_picker.h"
#include "usdcc/usd/asset_servers.h"
#include "usdcc/assets/activity.h"
#include "usdcc/usd/close_stage_dialog.h"
#include "usdcc/usd/create_menu.h"
#include "usdcc/usd/physics_menu.h"
#include "usdcc/usd/current_stage_ui.h"
#include "usdcc/usd/dependencies_view_panel.h"
#include "usdcc/usd/schema_explorer_view_panel.h"
#include "usdcc/usd/schema_libraries.h"
#include "usdcc/usd/schema_library_editor.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/layer_editor_view_panel.h"
#include "usdcc/usd/prim_tree_dialog.h"
#include "usdcc/usd/recent_files.h"
#include "usdcc/usd/renderer_settings.h"
#include "usdcc/usd/render/hydra_final_renderer.h"
#include "usdcc/usd/render/render_menu.h"
#include "usdcc/usd/render/render_network.h"
#include "usdcc/usd/render/render_queue_panel.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render/render_settings_view_panel.h"
#include "usdcc/usd/render/render_spec.h"
#include "usdcc/usd/render/swarm_panel.h"
#include "usdcc/usd/save_stage_as.h"
#include "usdcc/usd/selection_menu.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/usda_view_panel.h"
#include "usdcc/usd/variant_editor_dialog.h"
#include "usdcc/usd/log_test_messages.h"
#include "usdcc/usd/material_editor_view_panel.h"
#include "usdcc/usd/shader_nodes_dialog.h"
#include "usdcc/usd/color_management.h"
#include "usdcc/usd/usd_environment.h"
#include "usdcc/usd/usd_runtime_dialog.h"
#include "usdcc/usd/shading_preview_widget.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/properties/data_type_gallery_dialog.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"
#include "usdcc/usd/stage_opener.h"
#include "usdcc/usd/stages_menu.h"
#include "usdcc/usd/playback_controller.h"
#include "usdcc/usd/tf_diagnostic_bridge.h"
#include "usdcc/usd/timeline_bar.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_tool_bars.h"
#include "usdcc/usd/viewport_view_panel.h"

// MaterialX's version, for Help > About, when OpenUSD was built with it: its
// generated header is self-contained, so nothing else of MaterialX is needed.
#if __has_include(<MaterialXCore/Generated.h>)
#include <MaterialXCore/Generated.h>
#define USDCC_MATERIALX_STR2(x) #x
#define USDCC_MATERIALX_STR(x) USDCC_MATERIALX_STR2(x)
#define USDCC_MATERIALX_VERSION                                                                    \
    USDCC_MATERIALX_STR(MATERIALX_MAJOR_VERSION) "." USDCC_MATERIALX_STR(MATERIALX_MINOR_VERSION) "." \
        USDCC_MATERIALX_STR(MATERIALX_BUILD_VERSION)
#endif

using usdcc::app::ScopedLogSink;

int main(int argc, char** argv) {
    // Logging comes first, so nothing logged during start-up is lost: records
    // wait in the hub's ring buffer until the log file (below) attaches and
    // replays them. USDCC_LOG sets levels, e.g. "debug" or
    // "info,usdcc.viewport=debug,usd=warning" (docs/LOGGING.md §3.1).
    usdcc::app::configureLogLevels("usdcc.app");
    // Then everything that would otherwise only reach stdout/stderr: the C
    // level streams themselves (TF_DEBUG, printf from libraries), OpenUSD's
    // diagnostics and Qt's messages (docs/LOGGING.md §6). All three outlive
    // everything below; the capture is restored last.
    usdcc::core::NativeStreamCapture nativeStreams;
    usdcc::usd::TfDiagnosticBridge usdDiagnostics;
    usdcc::ui::QtMessageBridge qtMessages;
    // With the bridges in, neither OpenUSD nor Qt prints anything itself any
    // more, and stdout/stderr lead into the log; this keeps it all visible in
    // a terminal and the debugger (through the original stderr).
#ifdef NDEBUG
    const bool logToConsole = !qEnvironmentVariableIsEmpty("USDCC_LOG_STDERR");
#else
    const bool logToConsole = true;
#endif
    ScopedLogSink console(logToConsole ? std::make_shared<usdcc::core::ConsoleLogSink>() : nullptr);

    // Before QApplication: the OpenGL context the viewports and Storm want.
    usdcc::app::setDefaultSurfaceFormat();

    // The viewport is a native child window (QWidget::createWindowContainer),
    // and by default Qt then makes its siblings native too — including the
    // custom title bar, which then receives the mouse itself instead of the
    // main window's hit test, so dragging it no longer moves the window.
    // QWindowKit requires this attribute, set before QApplication exists.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

    QApplication app(argc, argv);

    // Integration tests need isolation before settings, plugins, Python and layouts load.
    usdcc::app::applyTestProfile(app, QStringLiteral("usdcc"));

    // usdcc [--script file.py]... [--no-startup-scripts] [--no-plugins] [--open-session name]
    // [--plugin-path folder]... [stage] (docs/SCRIPTING.md §6, docs/PLUGINS.md §2). Qt's own options (-platform, ...) are already
    // gone from the arguments here.
    QCommandLineParser commandLine;
    commandLine.setApplicationDescription(QObject::tr("usdcc, a USD editor"));
    commandLine.addHelpOption();
    const QCommandLineOption scriptOption(
        QStringLiteral("script"), QObject::tr("Run <file> once the window is up, after the startup scripts. Repeatable."),
        QObject::tr("file"));
    const QCommandLineOption noStartupScriptsOption(QStringLiteral("no-startup-scripts"),
                                                    QObject::tr("Don't run the scripts in the startup folder."));
    const QCommandLineOption noPluginsOption(QStringLiteral("no-plugins"),
                                             QObject::tr("Don't load any plugins, to start clean when one breaks the app."));
    const QCommandLineOption pluginPathOption(
        QStringLiteral("plugin-path"), QObject::tr("Also look for plugins in the folders inside <folder>. Repeatable."),
        QObject::tr("folder"));
    commandLine.addOption(scriptOption);
    commandLine.addOption(noStartupScriptsOption);
    commandLine.addOption(noPluginsOption);
    commandLine.addOption(pluginPathOption);
    const QCommandLineOption sessionOption(
        QStringLiteral("open-session"),
        QObject::tr("Start with a session: its name, or a session file's path (docs/SESSIONS.md)."),
        QObject::tr("name"));
    commandLine.addOption(sessionOption);
    const QCommandLineOption installPackagesOption(
        QStringLiteral("install-plugin-packages"),
        QObject::tr("Install the Python packages the plugins ask for, and exit. Exit code: 0 done, 2 some left out, 1 failed."));
    commandLine.addOption(installPackagesOption);
    commandLine.addPositionalArgument(QStringLiteral("stage"), QObject::tr("A USD file to open."),
                                      QStringLiteral("[stage]"));
    // Shows a message box and exits for --help and unknown options.
    commandLine.process(app);

    QString logFilePath;
    const auto fileLog = usdcc::app::openLogFile("usdcc.app", "usdcc", &logFilePath);

    // Preferences (docs/SETTINGS.md) from usdcc.ini, USD's environment, the OCIO config, and
    // the Python and plugins settings (shared with usdcc_node, startup.h).
    usdcc::app::installSettings();
    usdcc::ui::theme::apply(app);
    usdcc::scripting::SessionController::defineSettings();
    // Settings > Updates (docs/UPDATES.md §4).
    usdcc::app::AppUpdates::defineSettings();
    // Settings > Viewport's grid and background, there before any viewport opens.
    usdcc::usd::defineViewportDisplaySettings();
    usdcc::usd::defineMaterialPreviewSettings();

    // Reads the plugins' manifests and registers their USD files, running none
    // of their code. Before anything can make a stage: USD's schema registry
    // doesn't take in a plugin's schemas once it exists (docs/PLUGINS.md §3.1).
    usdcc::scripting::PluginManager plugins(usdcc::scripting::PluginManager::defaultOptions(
        usdcc::app::pluginFolders(commandLine.values(pluginPathOption)), !commandLine.isSet(noPluginsOption)));
    plugins.scan();
    // The schema libraries in the plugins' USD folders, and Settings > Assets.
    usdcc::app::configurePluginUsd(plugins);
    // Settings > Renderers: each render delegate's own settings, all of them listed only
    // when the dialog opens, as some take a while (a viewport defines its renderer's itself).
    // Final renders (docs/FINAL_RENDER.md): Settings > Final Render, and every Hydra render
    // delegate as a renderer (Arnold's too, set up above). Plugins add theirs when they start.
    usdcc::app::registerRenderers();
    usdcc::ui::SettingsDialog::setBeforeShow([]() {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        usdcc::usd::defineRendererSettings();
        QApplication::restoreOverrideCursor();
    });

    // Declared before `window` (and so destroyed after it, per C++'s
    // reverse-construction-order rule) since the viewport panel created
    // below is parented into `window` and holds usdcc::core::Signal
    // connections to this (see stage_manager.h). Owned here (the app's
    // composition root) rather than by MainWindow, which stays decoupled
    // from any one specific StageManager instance.
    usdcc::usd::StageManager stageManager;
    // File > Open Recent lists what's opened from here on, however it's opened,
    // the stage on the command line included.
    usdcc::usd::defineRecentFilesSetting(&app);
    const auto recentFilesConnection = usdcc::usd::rememberOpenedStages(stageManager);
    auto undoManager = std::make_shared<usdcc::usd::UndoManager>();
    undoManager->trackSelection(stageManager);  // selecting is undone and redone too
    // The ViewPanel types the Stages menu (and the viewport for a newly opened
    // stage) can create; filled in below, once the tool set exists. Declared
    // before `window` so it outlives the menu that reads it.
    usdcc::ui::ViewPanelRegistry panelTypes;

    // The embedded Python (docs/SCRIPTING.md §3). Declared after the stage
    // manager and before the window, so it is finalized after the panels are
    // gone but while the stages Python may still reference exist. If it
    // can't start, it says why in the log and the app runs without scripting.
    usdcc::scripting::PythonRuntime python;

    // For studios and farm machines, with nobody there to click (docs/PYTHON_PACKAGES.md).
    if (commandLine.isSet(installPackagesOption)) {
        const auto report = [](const std::string& line) {
            USDCC_LOG_INFO("usdcc.packages", "{}", line);
            // Not stdout, which the log captures once it has been set up.
            usdcc::core::writeToOriginalStderr(line + "\n");
        };
        return plugins.installPackages(python, report);
    }

    usdcc::ui::MainWindow window;
    window.logPanel()->setLogFile(logFilePath);

    // The editing tools and which is active, shared by every viewport and
    // driven by the tool rail / options bar in the main window's layout (the
    // toolbars are owned by `window`).
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    new usdcc::usd::ViewportToolBars(&window, toolSet, &stageManager);
    // The timeline (docs/ANIMATION.md §4.2, §4.3): one transport, on the current stage,
    // in a bar under the dock area. Both are the window's.
    auto* playback = new usdcc::usd::PlaybackController(&stageManager, &window);
    playback->addActions(&window);
    window.setBottomBar(new usdcc::usd::TimelineBar(playback, undoManager, &window), QObject::tr("Timeline"));
    // F: fit the active viewport to the selection, or to the whole stage.
    usdcc::usd::ViewportViewPanel::addFitToViewAction(&window);
    // The active viewport's HUD on or off (no key until one is set).
    usdcc::usd::ViewportViewPanel::addToggleHudAction(&window);
    // Shift+F: fit every viewport on screen (docs/WORKSPACES.md §3.5.7).
    QAction* fitAllViewsAction = usdcc::usd::ViewportViewPanel::addFitAllViewsAction(&window);

    // Workspaces (docs/WORKSPACES.md): predefined layouts of the view panels, from JSON
    // files in folders; the title bar's button switches between them. The app's three
    // fixed panels are only ever closed, never deleted (sessions count on them).
    usdcc::ui::WorkspaceLibrary workspaceLibrary;
    usdcc::ui::WorkspaceController workspaces(
        {&window, &panelTypes, &stageManager,
         {QStringLiteral("Viewport"), QStringLiteral("Outliner"), QStringLiteral("Attributes")}},
        &workspaceLibrary);
    workspaces.setFitAllViewsAction(fitAllViewsAction);
    window.addTitleBarWidget(workspaces.createButton(&window));

    // Scripts reach the app's stages, undo history, tools, window and view
    // panel types through these (usdcc.usd / usdcc.app / usdcc.tools,
    // docs/SCRIPTING.md §4, §8) until this goes out of scope, before the tool
    // set does. ScriptRunner makes each run one undo step per stage it
    // changes. Python view panel types registered in panelTypes outlive the
    // interpreter; they let go of their Python class without it.
    // Sessions (docs/SESSIONS.md). While one loads, a stage opened doesn't get a
    // viewport of its own (below): the session says which panels there are.
    bool sessionLoading = false;
    bool restartRequested = false;
    QStringList restartArguments;
    usdcc::scripting::SessionController::Context sessionContext;
    sessionContext.stageManager = &stageManager;
    sessionContext.window = &window;
    sessionContext.panelTypes = &panelTypes;
    sessionContext.plugins = &plugins;
    sessionContext.python = &python;
    sessionContext.fixedPanels = {QStringLiteral("Viewport"), QStringLiteral("Outliner"), QStringLiteral("Attributes")};
    sessionContext.setLoading = [&sessionLoading](bool loading) { sessionLoading = loading; };
    sessionContext.workspace = [&workspaces]() { return workspaces.currentId(); };
    sessionContext.setWorkspace = [&workspaces](const QString& id) { workspaces.setCurrentId(id); };
    // Restart and Load (§4.3): closes the window, asking what closing asks, and once
    // the app has quit starts it again with `arguments` (end of main()).
    sessionContext.restart = [&window, &restartRequested, &restartArguments](const QStringList& arguments) {
        restartRequested = true;
        restartArguments = arguments;
        window.close();
        if (window.isVisible()) restartRequested = false;
        return restartRequested;
    };
    usdcc::scripting::SessionController sessions(sessionContext);

    const usdcc::scripting::AppContextScope scriptingContext(
        {&stageManager, undoManager.get(), toolSet.get(), &window, &panelTypes, &plugins, &sessions, &workspaces, playback});
    usdcc::scripting::ScriptRunner scriptRunner(python, stageManager, *undoManager);
    // Docked on the right until the user moves it (a saved layout wins).
    auto* scriptEditor = new usdcc::scripting::ScriptEditorSidePanel(python, scriptRunner);
    window.addSidePanel(scriptEditor, Qt::RightDockWidgetArea);
    sessions.setScriptEditor(scriptEditor);

    // Panels created on demand get a "<type> - <stage>" title and a unique
    // objectName (see ViewPanelRegistry::open()); the three startup panels
    // below keep their plain names, which saved layouts refer to. Viewports
    // drop the stage from their title and are just "Viewport".
    panelTypes.add(QStringLiteral("viewport"), QObject::tr("Viewport"),
                   [&stageManager, toolSet, undoManager](const usdcc::usd::StageRefPtr& stage, const QString& /*title*/,
                                                          QWidget* parent) -> usdcc::ui::ViewPanel* {
                       return new usdcc::usd::ViewportViewPanel(&stageManager, stage, toolSet, undoManager,
                                                                QObject::tr("Viewport"), parent);
                   });
    panelTypes.add(QStringLiteral("outliner"), QObject::tr("Outliner"),
                   [&stageManager, undoManager](const usdcc::usd::StageRefPtr& stage, const QString& title,
                                                 QWidget* parent) -> usdcc::ui::ViewPanel* {
                       return new usdcc::usd::OutlinerViewPanel(&stageManager, stage, undoManager, title, parent);
                   });
    panelTypes.add(QStringLiteral("attributes"), QObject::tr("Attributes"),
                   [&stageManager, undoManager](const usdcc::usd::StageRefPtr& stage, const QString& title,
                                                 QWidget* parent) -> usdcc::ui::ViewPanel* {
                       return new usdcc::usd::AttributesViewPanel(&stageManager, stage, undoManager, title, parent);
                   });
    panelTypes.add(QStringLiteral("layers"), QObject::tr("Layers"),
                   [&stageManager, undoManager](const usdcc::usd::StageRefPtr& stage, const QString& title,
                                                 QWidget* parent) -> usdcc::ui::ViewPanel* {
                       return new usdcc::usd::LayerEditorViewPanel(&stageManager, stage, undoManager, title, parent);
                   });
    panelTypes.add(QStringLiteral("usda"), QObject::tr("USDA"),
                   [&stageManager](const usdcc::usd::StageRefPtr& stage, const QString& title,
                                   QWidget* parent) -> usdcc::ui::ViewPanel* {
                       return new usdcc::usd::UsdaViewPanel(&stageManager, stage, title, parent);
                   });
    panelTypes.add(QStringLiteral("materialEditor"), QObject::tr("Material Editor"),
                   [&stageManager, undoManager](const usdcc::usd::StageRefPtr& stage, const QString& title,
                                                 QWidget* parent) -> usdcc::ui::ViewPanel* {
                       return new usdcc::usd::MaterialEditorViewPanel(&stageManager, stage, undoManager, title, parent);
                   });
    panelTypes.add(QStringLiteral("renderSettings"), QObject::tr("Render Settings"),
                   [&stageManager, undoManager](const usdcc::usd::StageRefPtr& stage, const QString& title,
                                                 QWidget* parent) -> usdcc::ui::ViewPanel* {
                       return new usdcc::render::RenderSettingsViewPanel(&stageManager, stage, undoManager, title, parent);
                   });
    panelTypes.add(QStringLiteral("dependencies"), QObject::tr("Dependencies"),
                   [&stageManager, undoManager](const usdcc::usd::StageRefPtr& stage, const QString& title,
                                                 QWidget* parent) -> usdcc::ui::ViewPanel* {
                       return new usdcc::usd::DependenciesViewPanel(&stageManager, stage, undoManager, title, parent);
                   });
    // The schemas USD has registered (docs/SCHEMAS.md §4).
    panelTypes.add(QStringLiteral("schemas"), QObject::tr("Schemas"),
                   [&stageManager, undoManager](const usdcc::usd::StageRefPtr& stage, const QString& title,
                                                 QWidget* parent) -> usdcc::ui::ViewPanel* {
                       return new usdcc::usd::SchemaExplorerViewPanel(&stageManager, stage, undoManager, title, parent);
                   });

    // The stage passed on the command line, if any. Without one (or if it
    // failed to open) the app starts with no stage: the panels below are
    // empty, with their Stage dropdowns disabled, until one is opened.
    const QStringList positional = commandLine.positionalArguments();
    // A session to start with (docs/SESSIONS.md §4.5): --open-session, or the Last session
    // when Settings > Files > Sessions > On start says so. A stage named too opens after it.
    QString startSession = commandLine.value(sessionOption);
    if (startSession.isEmpty() && usdcc::scripting::SessionController::loadsLastSessionAtStart() &&
        usdcc::ui::SessionStore().contains(QLatin1String(usdcc::ui::session::kLastSessionName))) {
        startSession = QLatin1String(usdcc::ui::session::kLastSessionName);
    }
    usdcc::usd::StageRefPtr initialStage;
    if (!positional.isEmpty() && startSession.isEmpty()) {
        // A URL given on the command line: its host may be contacted, for this session.
        usdcc::assets::allowHostOfUrl(positional.front().toStdString());
        initialStage = stageManager.openStage(positional.front().toStdString());
    }
    if (positional.size() > 1) {
        USDCC_LOG_WARNING("usdcc.app", "Only one stage can be opened from the command line; ignored {}",
                          positional.mid(1).join(QStringLiteral(", ")).toStdString());
    }

    // The center dock area is established here (rather than by MainWindow)
    // since it's the first USD-dependent panel — see MainWindow's comment on
    // why it no longer seeds a stand-in there itself.
    auto* viewport =
        new usdcc::usd::ViewportViewPanel(&stageManager, initialStage, toolSet, undoManager, "Viewport", &window);
    auto* centerArea = window.dockManager()->addDockWidget(ads::CenterDockWidgetArea, viewport);

    auto* outliner = new usdcc::usd::OutlinerViewPanel(&stageManager, initialStage, undoManager, "Outliner", &window);
    window.dockManager()->addDockWidgetTabToArea(outliner, centerArea);

    auto* attributes = new usdcc::usd::AttributesViewPanel(&stageManager, initialStage, undoManager, "Attributes", &window);
    window.dockManager()->addDockWidgetTabToArea(attributes, centerArea);

    // Their types, so their duplicates are made again after a restart too
    // (ViewPanelRegistry::savePanels()).
    viewport->setProperty(usdcc::ui::ViewPanelRegistry::kTypeProperty, QStringLiteral("viewport"));
    outliner->setProperty(usdcc::ui::ViewPanelRegistry::kTypeProperty, QStringLiteral("outliner"));
    attributes->setProperty(usdcc::ui::ViewPanelRegistry::kTypeProperty, QStringLiteral("attributes"));

    // Every stage opened from here on (e.g. via File > Open Stage below) is
    // shown in one viewport, which is brought to the front: a viewport with no
    // stage if there is one (one on screen first, else a closed one, which is
    // reopened), otherwise a new viewport (ViewPanelRegistry::open()).
    // Viewports leave this choice to this handler (see
    // ViewportViewPanel::adoptsOpenedStage()); other empty panels switch to the
    // stage themselves. Safe from double-handling initialStage: its
    // stageOpened already fired synchronously above, before this connection
    // existed. Kept alive for the whole app lifetime by living in main()'s
    // own scope.
    //
    // The panels are looked up at call time through the dock manager — which
    // also knows floating panels and no longer lists one File > Close Stage is
    // deleting — rather than captured here: restoreLayout() below can rebuild
    // the dock area tree, and panels come and go.
    //
    // That viewport is focused and the stage made current (docs/
    // CURRENT_STAGE.md), as the stage just opened is the one being looked at:
    // bringing a tab to the front doesn't move ADS's focus, so the panel
    // focused before would otherwise keep its stage current.
    // Opens off the UI thread behind a progress dialog (the stage named on the
    // command line, above, deliberately just blocks instead: there is no window
    // yet). Owned by `window`. Made before the handler below, which asks it how a
    // stage is being opened (Open Stage with Options, docs/WORKSPACES.md §8).
    auto* stageOpener = new usdcc::usd::StageOpener(&stageManager, &window);
    auto viewportOnStageOpened = stageManager.stageOpened.connect(
        [&window, &panelTypes, &stageManager, &sessionLoading, stageOpener](const usdcc::usd::StageRefPtr& stage) {
            // While Save Stage As puts the new file in the old stage's panels, the stage
            // it opens gets no viewport of its own.
            if (sessionLoading || usdcc::usd::isSwitchingStage()) return;
            // Open Stage with Options, Make current off: its viewport goes behind, and
            // the current stage stays (docs/WORKSPACES.md §8.2).
            const usdcc::usd::StageOpener::Options* opening = stageOpener->adopting();
            const bool behind = opening && opening->makeCurrent == usdcc::usd::StageOpener::Options::MakeCurrent::No;
            auto focus = [&](usdcc::ui::ViewPanel* panel) {
                if (behind) return;
                if (panel) window.dockManager()->setDockWidgetFocused(panel);
                stageManager.setCurrentStage(stage);
            };
            usdcc::usd::ViewportViewPanel* empty = nullptr;
            for (ads::CDockWidget* dockWidget : window.dockManager()->dockWidgetsMap()) {
                auto* existing = qobject_cast<usdcc::usd::ViewportViewPanel*>(dockWidget);
                // One following the current stage isn't free: it shows whatever's current.
                if (!existing || existing->followsCurrentStage()) {
                    continue;
                }
                if (!existing->currentStage() && (!empty || (empty->isClosed() && !existing->isClosed()))) {
                    empty = existing;
                }
            }

            if (empty) {
                if (empty->isClosed()) {
                    empty->toggleView(true);
                }
                empty->setStage(stage);
                if (!behind) empty->setAsCurrentTab();
                focus(empty);
                return;
            }

            // Every viewport follows the current stage (Appearance > Follow Current Stage):
            // one on screen shows the stage once it's current, and a new one would only
            // show the same. Made current behind the others, it waits in the Stages menu.
            if (usdcc::ui::ViewPanelRegistry::followsCurrentStageByDefault(QStringLiteral("viewport"))) {
                usdcc::ui::ViewPanel* following = nullptr;
                for (ads::CDockWidget* dockWidget : window.dockManager()->dockWidgetsMap()) {
                    auto* existing = qobject_cast<usdcc::usd::ViewportViewPanel*>(dockWidget);
                    if (existing && !existing->isClosed() && existing->followsCurrentStage()) {
                        following = existing;
                        break;
                    }
                }
                if (following) {
                    if (!behind) following->setAsCurrentTab();
                    focus(following);
                    return;
                }
            }

            // A new viewport, as a tab of the workspace's viewport area (§7.3); without
            // one, beside any viewport.
            ads::CDockManager* docks = window.dockManager();
            usdcc::ui::ViewPanel* made = nullptr;
            if (ads::CDockAreaWidget* area = usdcc::ui::workspace::viewportArea(docks)) {
                ads::CDockWidget* front = area->currentDockWidget();
                made = panelTypes.create(QStringLiteral("viewport"), stage, docks, &window);
                if (made) {
                    docks->addDockWidgetTabToArea(made, area);
                    if (behind && front) {
                        front->setAsCurrentTab();
                    } else {
                        made->setAsCurrentTab();
                    }
                }
            } else {
                made = panelTypes.open(QStringLiteral("viewport"), stage, docks, &window);
            }
            focus(made);
        });

    QMenu* fileMenu = window.mainMenuBar()->addMenu(QObject::tr("&File"));
    QAction* openStageAction = fileMenu->addAction(QObject::tr("&Open Stage..."));
    QObject::connect(stageOpener, &usdcc::usd::StageOpener::busyChanged, openStageAction,
                     [openStageAction](bool busy) { openStageAction->setEnabled(!busy); });
    QObject::connect(stageOpener, &usdcc::usd::StageOpener::showLogRequested, window.logPanel(),
                     &usdcc::ui::LogSidePanel::showLatest);
    // The asset picker (docs/ASSET_PICKER.md): a stage on disk or on a server.
    const auto pickStage = [&window](const QString& title) {
        usdcc::usd::AssetPickerOptions options;
        options.title = title;
        options.filters = {QObject::tr("USD Files (*.usd *.usda *.usdc *.usdz)")};
        options.context = usdcc::usd::stageResolverContext(std::string());
        return usdcc::usd::pickAsset(&window, options);
    };
    QObject::connect(openStageAction, &QAction::triggered, &window, [stageOpener, pickStage]() {
        const QString path = pickStage(QObject::tr("Open USD Stage"));
        if (!path.isEmpty()) {
            stageOpener->open(path);
        }
    });
    // File > Open Stage with Options...: the file, then how to open it (docs/WORKSPACES.md §8).
    QAction* openWithOptionsAction = fileMenu->addAction(QObject::tr("Open Stage with O&ptions..."));
    openWithOptionsAction->setObjectName(QStringLiteral("openStageWithOptions"));
    openWithOptionsAction->setStatusTip(
        QObject::tr("Open a stage, choosing whether it becomes current, a workspace, and whether its payloads load"));
    QObject::connect(stageOpener, &usdcc::usd::StageOpener::busyChanged, openWithOptionsAction,
                     [openWithOptionsAction](bool busy) { openWithOptionsAction->setEnabled(!busy); });
    QObject::connect(openWithOptionsAction, &QAction::triggered, &window, [stageOpener, &window, &workspaceLibrary, pickStage]() {
        const QString path = pickStage(QObject::tr("Open USD Stage"));
        if (path.isEmpty()) return;
        usdcc::usd::OpenStageOptionsDialog dialog(path, &workspaceLibrary, &window);
        if (dialog.exec() == QDialog::Accepted) stageOpener->open(path, dialog.options());
    });
    // File > Open Stage from URL...: a stage on a server (docs/REMOTE_ASSETS.md §10).
    QAction* openUrlAction = fileMenu->addAction(QObject::tr("Open Stage from &URL..."));
    openUrlAction->setObjectName(QStringLiteral("openStageFromUrl"));
    openUrlAction->setStatusTip(QObject::tr("Open a stage on a server: a webdav://, http:// or https:// path"));
    QObject::connect(stageOpener, &usdcc::usd::StageOpener::busyChanged, openUrlAction,
                     [openUrlAction](bool busy) { openUrlAction->setEnabled(!busy); });
    QObject::connect(openUrlAction, &QAction::triggered, &window, [stageOpener, &window]() {
        const QString url = usdcc::usd::askForAssetUrl(&window);
        if (!url.isEmpty()) stageOpener->open(url);
    });
    // Both keys are settings (Shortcuts > File).
    for (const auto& [key, label, fallback, action] :
         {std::tuple{"Shortcuts/file.openStage", "Open stage", "Ctrl+O", openStageAction},
          std::tuple{"Shortcuts/file.openStageWithOptions", "Open stage with options", "Ctrl+Shift+O", openWithOptionsAction}}) {
        usdcc::core::Settings::instance().define({.key = key,
                                                  .category = "Shortcuts",
                                                  .group = "File",
                                                  .label = label,
                                                  .type = usdcc::core::SettingType::Shortcut,
                                                  .defaultValue = std::string(fallback)});
        usdcc::ui::bindShortcut(action, key);
    }
    // After a stage opened with options: Make current, the workspace, and a word when
    // the file was open already, so its payloads stayed as they were.
    QObject::connect(stageOpener, &usdcc::usd::StageOpener::opened, &window,
                     [&stageManager, &window, &workspaces](const usdcc::usd::StageRefPtr& stage,
                                                            const usdcc::usd::StageOpener::Options& options, bool wasOpen) {
                         if (!stage) return;
                         if (wasOpen && !options.loadPayloads) {
                             USDCC_LOG_TOAST(usdcc::core::LogLevel::Info, "usdcc.stage",
                                             "{} was open already; its payloads are as they were.",
                                             QFileInfo(QString::fromStdString(stage->usdStage()->GetRootLayer()->GetIdentifier()))
                                                 .fileName()
                                                 .toStdString());
                         }
                         if (!options.workspace.isEmpty()) {
                             QString error;
                             if (!workspaces.switchTo(options.workspace, &error)) {
                                 USDCC_LOG_WARNING("usdcc.workspaces", "{}", error.toStdString());
                             }
                         }
                         if (options.makeCurrent == usdcc::usd::StageOpener::Options::MakeCurrent::Everywhere) {
                             usdcc::usd::makeCurrentEverywhere(&stageManager, window.dockManager(), stage);
                             for (ads::CDockWidget* dock : window.dockManager()->dockWidgetsMap()) {
                                 auto* viewport = qobject_cast<usdcc::usd::ViewportViewPanel*>(dock);
                                 if (viewport && viewport->currentStage() == stage && !viewport->isClosed()) {
                                     viewport->setAsCurrentTab();
                                     window.dockManager()->setDockWidgetFocused(viewport);
                                     break;
                                 }
                             }
                         }
                     });

    // File > Open Recent: build from history without probing file availability.
    // Through the handler Open as Stage uses, so a file that's open comes forward.
    QMenu* recentMenu = fileMenu->addMenu(QObject::tr("Open &Recent"));
    QObject::connect(fileMenu, &QMenu::aboutToShow, recentMenu, [recentMenu]() {
        recentMenu->menuAction()->setEnabled(!usdcc::usd::recentFiles().isEmpty());
    });
    QObject::connect(recentMenu, &QMenu::aboutToShow, recentMenu, [recentMenu]() {
        usdcc::usd::populateRecentFilesMenu(recentMenu, [](const QString& path) { usdcc::usd::openStageFile(path); });
    });

    // File > Sessions > the newest five, then Manage... (docs/SESSIONS.md §6).
    // Manage... lives on the window too, so Ctrl+Alt+S works with the menu shut;
    // the window owns it, so the menu's clear() below leaves it alone.
    QAction* manageSessionsAction = new QAction(QObject::tr("&Manage..."), &window);
    manageSessionsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S));
    manageSessionsAction->setStatusTip(QObject::tr("Save the app as a session, or load one"));
    window.addAction(manageSessionsAction);
    QObject::connect(manageSessionsAction, &QAction::triggered, &window,
                     [&sessions, &window]() { usdcc::scripting::SessionsDialog::showDialog(&sessions, &window); });
    QMenu* openSessionMenu = fileMenu->addMenu(QObject::tr("Sess&ions"));
    openSessionMenu->setObjectName(QStringLiteral("sessionsMenu"));
    QObject::connect(openSessionMenu, &QMenu::aboutToShow, &window,
                     [openSessionMenu, manageSessionsAction, &sessions, &window]() {
        openSessionMenu->clear();
        const QList<usdcc::ui::SessionInfo> list = sessions.store().list();
        for (int i = 0; i < std::min<int>(5, list.size()); ++i) {
            const QString name = list[i].name;
            QAction* action = openSessionMenu->addAction(name);
            action->setEnabled(list[i].problem.isEmpty());
            // Queued: the questions a load may ask don't run inside the menu.
            QObject::connect(action, &QAction::triggered, &window, [name, &sessions, &window]() {
                QMetaObject::invokeMethod(
                    &window,
                    [name, &sessions, &window]() {
                        usdcc::scripting::SessionController::LoadOptions options;
                        options.source = name;
                        QString error;
                        if (sessions.load(name, options, &error) == usdcc::scripting::SessionController::LoadResult::Failed) {
                            QMessageBox::warning(&window, QObject::tr("Load Session"), error);
                        }
                    },
                    Qt::QueuedConnection);
            });
        }
        if (list.isEmpty()) openSessionMenu->addAction(QObject::tr("No sessions yet"))->setEnabled(false);
        openSessionMenu->addSeparator();
        openSessionMenu->addAction(manageSessionsAction);
    });
    fileMenu->addSeparator();

    // Saving a stage is per stage: File > Save Stage / Save Stage As (below), Stages > a stage
    // > Save Stage / Save Stage As..., and a panel tab's Save Stage for the stage it shows
    // (docs/LAYER_EDITOR.md §5).
    usdcc::ui::ViewPanel::setTabMenuExtension([&window](usdcc::ui::ViewPanel* panel, QMenu* menu) {
        const usdcc::usd::StageRefPtr stage = panel->currentStage();
        menu->addSeparator();
        QAction* save = menu->addAction(QObject::tr("Save Stage"));
        save->setObjectName(QStringLiteral("tabSaveStage"));
        save->setEnabled(usdcc::usd::stageHasUnsavedLayers(stage));
        QObject::connect(save, &QAction::triggered, &window, [stage, &window]() {
            QMetaObject::invokeMethod(
                &window, [stage, &window]() { usdcc::usd::saveStageInteractively(stage, &window); }, Qt::QueuedConnection);
        });
    });
    // Closing the window asks about unsaved layers of every open stage.
    window.addCloseCheck([&stageManager, &window]() {
        return usdcc::usd::confirmUnsavedLayers(&stageManager, stageManager.stages(), &window);
    });

    // File > Preview, first in the menu: looks at a file without opening it as a
    // stage of the app.
    QMenu* previewMenu = new QMenu(QObject::tr("&Preview"), fileMenu);
    fileMenu->insertMenu(openStageAction, previewMenu);
    fileMenu->insertSeparator(openStageAction);

    // File > Create Stage: a new stage, which like an opened one gets a viewport and
    // becomes current. One as a file is written at once, and goes in Open Recent.
    QMenu* createStageMenu = new QMenu(QObject::tr("Create S&tage"), fileMenu);
    fileMenu->insertMenu(openStageAction, createStageMenu);
    createStageMenu->addAction(QObject::tr("In &Memory"), &window, [&stageManager]() { stageManager.createStage(); });
    createStageMenu->addAction(QObject::tr("As &File..."), &window, [&stageManager, &window]() {
        // Its own question before replacing a file, which USD then would without asking.
        QString path = QFileDialog::getSaveFileName(
            &window, QObject::tr("Create Stage"), QString(),
            QObject::tr("USD Text (*.usda);;USD Binary (*.usdc);;USD (*.usd)"));
        if (path.isEmpty()) return;
        // USD needs an extension to know the format.
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".usda");
        std::string error;
        if (!stageManager.createStageFile(path.toStdString(), &error)) {
            QMessageBox::warning(&window, QObject::tr("Create Stage"),
                                 QObject::tr("Could not create %1.\n\n%2").arg(QDir::toNativeSeparators(path), QString::fromStdString(error)));
        }
    });

    // File > Preview > Show USDA...: as text (docs/USDA_VIEW.md §10).
    QAction* showUsdaAction = previewMenu->addAction(QObject::tr("Show &USDA..."));
    QObject::connect(showUsdaAction, &QAction::triggered, &window, [&window, pickStage]() {
        const QString path = pickStage(QObject::tr("Show USDA"));
        if (!path.isEmpty()) usdcc::usd::showUsdaPreview(path, &window);
    });
    // File > Preview > Show Prims...: as a tree of its prims (docs/SHOW_PRIMS.md).
    QAction* showPrimsAction = previewMenu->addAction(QObject::tr("Show &Prims..."));
    QObject::connect(showPrimsAction, &QAction::triggered, &window, [&window, pickStage]() {
        const QString path = pickStage(QObject::tr("Show Prims"));
        if (!path.isEmpty()) usdcc::usd::showPrimTree(path, &window);
    });

    // Outliner > Composition > an arc > Open as Stage: a file already open as
    // a stage has its viewport brought forward (and becomes current) rather
    // than being opened a second time, as File > Open Stage would.
    // USDA view > a link > Open in New Panel: a USDA panel from the registry,
    // docked next to the one the link was in.
    usdcc::usd::UsdaViewPanel::setNewPanelHandler([&window, &panelTypes](const usdcc::usd::StageRefPtr& stage) {
        return qobject_cast<usdcc::usd::UsdaViewPanel*>(
            panelTypes.open(QStringLiteral("usda"), stage, window.dockManager(), &window));
    });

    usdcc::usd::setOpenStageHandler([&stageManager, &window, &panelTypes, stageOpener](const QString& path) {
        const QString wanted = QFileInfo(path).canonicalFilePath();
#ifdef Q_OS_WIN
        constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
        constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif
        for (const usdcc::usd::StageRefPtr& stage : stageManager.stages()) {
            const QString open =
                QFileInfo(QString::fromStdString(stage->usdStage()->GetRootLayer()->GetRealPath())).canonicalFilePath();
            if (open.isEmpty() || open.compare(wanted, kPathCase) != 0) continue;
            usdcc::ui::ViewPanel* shown = nullptr;
            for (ads::CDockWidget* dockWidget : window.dockManager()->dockWidgetsMap()) {
                auto* viewport = qobject_cast<usdcc::usd::ViewportViewPanel*>(dockWidget);
                if (!viewport || viewport->currentStage() != stage) continue;
                shown = viewport;
                if (!viewport->isClosed()) break;  // one on screen first
            }
            if (shown) {
                if (shown->isClosed()) shown->toggleView(true);
                shown->setAsCurrentTab();
            } else {
                shown = panelTypes.open(QStringLiteral("viewport"), stage, window.dockManager(), &window);
            }
            if (shown) window.dockManager()->setDockWidgetFocused(shown);
            stageManager.setCurrentStage(stage);
            return;
        }
        stageOpener->open(path);
    });

    // File > Save Stage and Save Stage As..., above Close Stage. With one stage open each is
    // a plain action for it; with more, a submenu of the open stages, as Close Stage's, since
    // the current stage isn't always the one you mean. Queued: a message box or file dialog
    // doesn't run inside the menu, and the stage is held by value until it does.
    const auto saveStage = [&window](const usdcc::usd::StageRefPtr& stage) {
        QMetaObject::invokeMethod(
            &window, [stage, &window]() { usdcc::usd::saveStageInteractively(stage, &window); }, Qt::QueuedConnection);
    };
    const auto saveStageAs = [&stageManager, &window](const usdcc::usd::StageRefPtr& stage) {
        QMetaObject::invokeMethod(
            &window, [&stageManager, stage, &window]() { usdcc::usd::saveStageAsInteractively(stageManager, stage, &window); },
            Qt::QueuedConnection);
    };
    QAction* saveStageAction = fileMenu->addAction(QObject::tr("&Save Stage"));
    saveStageAction->setObjectName(QStringLiteral("fileSaveStage"));
    saveStageAction->setStatusTip(QObject::tr("Save every layer of the stage that has unsaved changes"));
    QMenu* saveStageMenu = fileMenu->addMenu(QObject::tr("&Save Stage"));
    saveStageMenu->setObjectName(QStringLiteral("fileSaveStageMenu"));
    saveStageMenu->setToolTipsVisible(true);
    QAction* saveStageAsAction = fileMenu->addAction(QObject::tr("Save Stage &As..."));
    saveStageAsAction->setObjectName(QStringLiteral("fileSaveStageAs"));
    saveStageAsAction->setStatusTip(QObject::tr("Save the stage to another file, and go on in that file"));
    QMenu* saveStageAsMenu = fileMenu->addMenu(QObject::tr("Save Stage &As"));
    saveStageAsMenu->setObjectName(QStringLiteral("fileSaveStageAsMenu"));
    saveStageAsMenu->setToolTipsVisible(true);
    // The only stage is looked up when the action runs, not when the menu opened.
    QObject::connect(saveStageAction, &QAction::triggered, &window, [&stageManager, saveStage]() {
        if (stageManager.stages().size() == 1) saveStage(stageManager.stages().front());
    });
    QObject::connect(saveStageAsAction, &QAction::triggered, &window, [&stageManager, saveStageAs]() {
        if (stageManager.stages().size() == 1) saveStageAs(stageManager.stages().front());
    });
    // The Save Stage key (Ctrl+S, a setting): the only stage, or a question which one. The
    // window owns it, so it works with the menu shut; the Layers panel and the Script Editor
    // keep Ctrl+S while focus is in them (bindSaveStageShortcut()).
    QAction* saveStageKeyAction = new QAction(QObject::tr("Save Stage"), &window);
    saveStageKeyAction->setObjectName(QStringLiteral("saveStageShortcut"));
    window.addAction(saveStageKeyAction);
    usdcc::usd::bindSaveStageShortcut(saveStageKeyAction);
    QObject::connect(saveStageKeyAction, &QAction::triggered, &window,
                     [&stageManager, &window]() { usdcc::usd::saveStageFromShortcut(stageManager, &window); });
    QObject::connect(fileMenu, &QMenu::aboutToShow, &window,
                     [&stageManager, saveStageAction, saveStageMenu, saveStageAsAction, saveStageAsMenu]() {
        const auto& stages = stageManager.stages();
        const bool several = stages.size() > 1;
        bool anyUnsaved = false;
        for (const usdcc::usd::StageRefPtr& stage : stages) anyUnsaved = anyUnsaved || usdcc::usd::stageHasUnsavedLayers(stage);
        // No stage open: the plain actions, greyed. The plain Save Stage shows the key the
        // window's action has, as text only: a second action with it would be ambiguous.
        // So does the submenu with several stages, where the key asks which one to save.
        const QKeySequence key = usdcc::ui::settingShortcut(usdcc::usd::kSaveStageShortcutKey);
        const QString saveLabel = key.isEmpty() ? QObject::tr("&Save Stage")
                                                : QObject::tr("&Save Stage") + QLatin1Char('\t') +
                                                      key.toString(QKeySequence::NativeText);
        saveStageAction->setText(saveLabel);
        saveStageMenu->menuAction()->setText(saveLabel);
        saveStageAction->setVisible(!several);
        saveStageAction->setEnabled(stages.size() == 1 && anyUnsaved);
        saveStageAsAction->setVisible(!several);
        saveStageAsAction->setEnabled(stages.size() == 1);
        saveStageMenu->menuAction()->setVisible(several);
        saveStageMenu->menuAction()->setEnabled(anyUnsaved);
        saveStageAsMenu->menuAction()->setVisible(several);
    });
    // One entry per open stage, built when the submenu opens.
    const auto fillStageMenu = [&stageManager](QMenu* menu, bool onlyUnsaved,
                                               std::function<void(const usdcc::usd::StageRefPtr&)> run) {
        menu->clear();
        const auto& stages = stageManager.stages();
        const std::vector<std::string> names = usdcc::usd::shortStageNames(stages);
        for (std::size_t i = 0; i < stages.size(); ++i) {
            const usdcc::usd::StageRefPtr stage = stages[i];
            QAction* action = menu->addAction(QString::fromStdString(names[i]));
            action->setToolTip(QString::fromStdString(stage->usdStage()->GetRootLayer()->GetIdentifier()));
            if (onlyUnsaved) action->setEnabled(usdcc::usd::stageHasUnsavedLayers(stage));
            QObject::connect(action, &QAction::triggered, menu, [run, stage]() { run(stage); });
        }
    };
    QObject::connect(saveStageMenu, &QMenu::aboutToShow, &window,
                     [fillStageMenu, saveStageMenu, saveStage]() { fillStageMenu(saveStageMenu, true, saveStage); });
    QObject::connect(saveStageAsMenu, &QMenu::aboutToShow, &window,
                     [fillStageMenu, saveStageAsMenu, saveStageAs]() { fillStageMenu(saveStageAsMenu, false, saveStageAs); });

    // File > Close Stage, as Save Stage above: a plain action with one stage open, else a
    // submenu of the open stages.
    const auto closeStage = [&stageManager, &window](const usdcc::usd::StageRefPtr& stage) {
        QMetaObject::invokeMethod(
            &window, [&stageManager, &window, stage]() { usdcc::usd::closeStageInteractively(&stageManager, stage, &window); },
            Qt::QueuedConnection);
    };
    QAction* closeStageAction = fileMenu->addAction(QObject::tr("&Close Stage"));
    closeStageAction->setObjectName(QStringLiteral("fileCloseStage"));
    closeStageAction->setStatusTip(QObject::tr("Close the stage, with every panel showing it"));
    QMenu* closeStageMenu = fileMenu->addMenu(QObject::tr("&Close Stage"));
    closeStageMenu->setObjectName(QStringLiteral("fileCloseStageMenu"));
    closeStageMenu->setToolTipsVisible(true);
    QObject::connect(closeStageAction, &QAction::triggered, &window, [&stageManager, closeStage]() {
        if (stageManager.stages().size() == 1) closeStage(stageManager.stages().front());
    });
    QObject::connect(fileMenu, &QMenu::aboutToShow, &window, [&stageManager, closeStageAction, closeStageMenu]() {
        const std::size_t count = stageManager.stages().size();
        closeStageAction->setVisible(count <= 1);
        closeStageAction->setEnabled(count == 1);
        closeStageMenu->menuAction()->setVisible(count > 1);
    });
    // With more than one stage, Close All under the stages, past a separator: one
    // question about unsaved layers and one about the panels, whose only stage to
    // switch them to is "No stage".
    QObject::connect(closeStageMenu, &QMenu::aboutToShow, &window,
                     [&stageManager, &window, fillStageMenu, closeStageMenu, closeStage]() {
        fillStageMenu(closeStageMenu, false, closeStage);
        if (stageManager.stages().size() < 2) return;
        closeStageMenu->addSeparator();
        QAction* closeAll = closeStageMenu->addAction(QObject::tr("Close &All"));
        closeAll->setObjectName(QStringLiteral("fileCloseAllStages"));
        closeAll->setToolTip(QObject::tr("Close every open stage, with every panel showing them"));
        QObject::connect(closeAll, &QAction::triggered, closeStageMenu, [&stageManager, &window]() {
            QMetaObject::invokeMethod(
                &window,
                [&stageManager, &window]() {
                    usdcc::usd::closeStagesInteractively(&stageManager, stageManager.stages(), &window);
                },
                Qt::QueuedConnection);
        });
    });
    // File > Exit, last: closing the window, which asks about unsaved layers.
    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(QObject::tr("E&xit"), &window, &QWidget::close);
    exitAction->setStatusTip(QObject::tr("Close usdcc"));
    exitAction->setMenuRole(QAction::QuitRole);

    // A closed stage's undo history goes with it (it also holds the stage).
    auto undoOnStageClosed = stageManager.stageClosed.connect(
        [undoManager](const usdcc::usd::StageRefPtr& stage) { undoManager->removeStage(stage); });

    // Undo and Redo act on the current stage: the stage of the view panel
    // last focused (CurrentStageTracker below, docs/CURRENT_STAGE.md). They
    // name the step and grey out when there is none.
    QMenu* editMenu = window.mainMenuBar()->addMenu(QObject::tr("&Edit"));
    auto* undoGroup = new usdcc::usd::CurrentStageUndoGroup(*undoManager, stageManager, &window);
    QAction* undoAction = undoGroup->createUndoAction(&window, QObject::tr("&Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    editMenu->addAction(undoAction);
    QAction* redoAction = undoGroup->createRedoAction(&window, QObject::tr("&Redo"));
    redoAction->setShortcut(QKeySequence::Redo);
    editMenu->addAction(redoAction);
    editMenu->addSeparator();
    // Edit > Selection: None, All, Invert, By Type, By Schema, By Name (docs/SELECTION_MENU.md).
    editMenu->addMenu(new usdcc::usd::SelectionMenu(&stageManager, &window));
    editMenu->addSeparator();
    // A login for an asset server, kept in the system's credential store (docs/REMOTE_ASSETS.md §8).
    editMenu->addAction(QObject::tr("Asset Server &Login..."), &window,
                        [&window]() { usdcc::usd::showAssetLoginDialog(&window); });
    QAction* settingsAction = editMenu->addAction(QObject::tr("&Settings..."), &window,
                                                  [&window]() { usdcc::ui::SettingsDialog::showDialog(&window); });
    settingsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    settingsAction->setMenuRole(QAction::PreferencesRole);

    // The menu bar runs File, Edit, View, Create, (plugin menus such as Mesh),
    // Physics, Stages, Help, Debug: QMenuBar orders entries by insertion, and
    // addMainMenuAction puts a plugin's new menu in after Create.
    window.buildViewMenu();
    // View > Workspace, first: the title bar button's menu (docs/WORKSPACES.md §6.2).
    if (QMenu* viewMenu = window.viewMenu()) {
        QAction* first = viewMenu->actions().value(0);
        QMenu* workspaceMenu = workspaces.createMenu(QObject::tr("&Workspace"), viewMenu);
        viewMenu->insertMenu(first, workspaceMenu);
        viewMenu->insertSeparator(first);
    }
    // And in a panel tab's menu, under Close.
    usdcc::ui::ViewPanel::setTabWorkspaceMenu(
        [&workspaces](QWidget* parent) { return workspaces.createMenu(QObject::tr("&Workspace"), parent); });

    // Create > a prim of any type, in the current stage (docs/CREATE_MENU.md).
    window.mainMenuBar()->addMenu(new usdcc::usd::PrimCreateMenu(&stageManager, undoManager, &window));
    // Physics > rigid bodies, colliders, a ground and a scene; Simulate (docs/PHYSICS.md §5.4).
    window.mainMenuBar()->addMenu(new usdcc::usd::PhysicsMenu(&stageManager, undoManager, playback, &window));

    // Render > Render Frame, Render Animation, Render Settings, Render Queue (docs/FINAL_RENDER.md §5).
    window.mainMenuBar()->addMenu(
        usdcc::render::createRenderMenu(&stageManager, &panelTypes, window.dockManager(), &window));

    // Stages > <stage> > Workspace, a new panel of each type, or Close Stage.
    auto* stagesMenu = new usdcc::usd::StagesMenu(&stageManager, &panelTypes, window.dockManager(), &window);
    stagesMenu->setWorkspaces(&workspaces);
    window.mainMenuBar()->addMenu(stagesMenu);

    // Help before Debug; filled below.
    QMenu* helpMenu = window.mainMenuBar()->addMenu(QObject::tr("&Help"));

    // Developer tools, last; always shown until a setting can hide it (M10).
    QMenu* debugMenu = window.mainMenuBar()->addMenu(QObject::tr("&Debug"));
    QAction* dataTypesAction = debugMenu->addAction(QObject::tr("USD &Data Types..."));
    QObject::connect(dataTypesAction, &QAction::triggered, &window, [&window]() {
        // One non-modal instance; reopening the action brings it back to front.
        static QPointer<usdcc::usd::properties::DataTypeGalleryDialog> dialog;
        if (!dialog) {
            dialog = new usdcc::usd::properties::DataTypeGalleryDialog(&window);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
        }
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    });
    // The Material Editor's catalog (docs/MATERIAL_EDITOR.md §3).
    QAction* shaderNodesAction = debugMenu->addAction(QObject::tr("&Shader Nodes..."));
    QObject::connect(shaderNodesAction, &QAction::triggered, &window, [&window]() {
        static QPointer<usdcc::usd::ShaderNodesDialog> dialog;
        if (!dialog) {
            dialog = new usdcc::usd::ShaderNodesDialog(&window);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
        }
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    });
    // Checks for the Log panel (docs/LOGGING.md §7): one message of every
    // level from every source, and a large burst.
    debugMenu->addSeparator();
    QAction* testMessagesAction = debugMenu->addAction(QObject::tr("Emit &Test Log Messages"));
    QObject::connect(testMessagesAction, &QAction::triggered, &window, []() { usdcc::usd::emitLogTestMessages(); });
    QAction* burstAction = debugMenu->addAction(QObject::tr("Emit 10 000 Log &Messages"));
    QObject::connect(burstAction, &QAction::triggered, &window, []() { usdcc::usd::emitLogBurst(10000); });
    // One toast of every kind, a long one, a sticky one and one from the log
    // (docs/NOTIFICATIONS.md).
    QAction* testToastsAction = debugMenu->addAction(QObject::tr("Show Test T&oasts"));
    QObject::connect(testToastsAction, &QAction::triggered, &window, [&window]() {
        using usdcc::core::ToastKind;
        using usdcc::core::ToastRequest;
        usdcc::ui::ToastManager* toasts = window.toasts();
        toasts->show({.kind = ToastKind::Info, .title = "Stage opened", .body = "cube_and_sphere.usda"});
        toasts->show({.kind = ToastKind::Success, .title = "Saved", .body = "cube_and_sphere.usda\n3 prims, 2 layers"});
        toasts->show({.kind = ToastKind::Warning,
                      .title = "Unresolved asset paths",
                      .body = "Two references point at files that don't exist:\n"
                              "  ./props/chair.usda\n"
                              "  ./props/table_with_a_rather_long_file_name_that_wraps.usda\n"
                              "The prims that use them stay empty until the files are there."});
        toasts->show({.kind = ToastKind::Error,
                      .title = "Export failed (stays until dismissed)",
                      .body = "Could not write C:/out/scene.usda:\nPermission denied",
                      .durationMs = ToastRequest::kSticky});
        USDCC_LOG_TOAST(usdcc::core::LogLevel::Warning, "usdcc.test", "A warning logged as a toast, from {}", "C++");
    });
    // Runs a file without opening it in the Script Editor panel.
    debugMenu->addSeparator();
    QAction* runPythonAction = debugMenu->addAction(QObject::tr("Run &Python File..."));
    runPythonAction->setEnabled(python.isAvailable());
    runPythonAction->setStatusTip(python.isAvailable()
                                      ? QObject::tr("Run a Python script in the app (Python %1)")
                                            .arg(QString::fromStdString(python.version()))
                                      : QString::fromStdString(python.unavailableReason()));
    QObject::connect(runPythonAction, &QAction::triggered, &window, [&scriptRunner, &window]() {
        const QString path = QFileDialog::getOpenFileName(&window, QObject::tr("Run Python File"), QString(),
                                                            QObject::tr("Python Files (*.py)"));
        if (!path.isEmpty()) {
            scriptRunner.runFile(std::filesystem::path(path.toStdU16String()));
        }
    });

    // Help > docs/USD_RUNTIME.md §1.
    QAction* usdRuntimeAction = helpMenu->addAction(QObject::tr("&USD Runtime..."));
    usdRuntimeAction->setStatusTip(QObject::tr("The OpenUSD usdcc runs with: its plugins, file formats, render delegates, "
                                               "environment and debug flags"));
    QObject::connect(usdRuntimeAction, &QAction::triggered, &window,
                     [&window]() { usdcc::usd::UsdRuntimeDialog::showDialog(&window); });
    helpMenu->addSeparator();
    // Help > Check for Updates (docs/UPDATES.md §4): an installed usdcc updates itself from the
    // GitHub releases; any other copy is pointed to them. Closing for Restart Now asks what
    // File > Exit asks.
    usdcc::app::AppUpdates updates(&window, [&window]() {
        window.close();
        return !window.isVisible();
    });
    QAction* checkForUpdatesAction = helpMenu->addAction(QObject::tr("Check for &Updates..."));
    checkForUpdatesAction->setStatusTip(updates.canUpdate()
                                            ? QObject::tr("Look for a newer release of usdcc, and install it")
                                            : QObject::tr("Look for a newer release of usdcc on its releases page"));
    QObject::connect(checkForUpdatesAction, &QAction::triggered, &window, [&updates]() { updates.checkNow(); });
    QAction* projectHomeAction = helpMenu->addAction(QObject::tr("&Project Home"));
    projectHomeAction->setStatusTip(QObject::tr("Open usdcc's GitHub page in your browser"));
    QObject::connect(projectHomeAction, &QAction::triggered, &window, []() {
        QDesktopServices::openUrl(QUrl(QString::fromLatin1(usdcc::ui::AboutDialog::kRepositoryUrl)));
    });
    QAction* aboutAction = helpMenu->addAction(QObject::tr("&About usdcc"));
    aboutAction->setMenuRole(QAction::AboutRole);
    QObject::connect(aboutAction, &QAction::triggered, &window, [&window, &python]() {
        usdcc::ui::AboutDialog::Info info = usdcc::ui::AboutDialog::currentBuild();
        info.components.prepend({QStringLiteral("OpenUSD"), QStringLiteral("%1.%2.%3")
                                                                 .arg(PXR_MAJOR_VERSION)
                                                                 .arg(PXR_MINOR_VERSION)
                                                                 .arg(PXR_PATCH_VERSION)});
#ifdef USDCC_MATERIALX_VERSION
        info.components.insert(1, {QStringLiteral("MaterialX"), QStringLiteral(USDCC_MATERIALX_VERSION)});
#endif
        if (python.isAvailable()) {
            info.components.append({QStringLiteral("Python"), QString::fromStdString(python.version())});
            // Without raising (evaluate() logs that) when PySide6 isn't there.
            const auto pyside = python.evaluate(
                "getattr(__import__('importlib.util').util.find_spec('PySide6') and __import__('PySide6'), "
                "'__version__', '')");
            if (pyside && !pyside->empty()) {
                info.components.append({QStringLiteral("PySide6"), QString::fromStdString(*pyside)});
            }
        }
        info.components.append({QStringLiteral("Compiler"), QStringLiteral(USDCC_COMPILER)});
        usdcc::ui::AboutDialog dialog(info, &window);
        dialog.exec();
    });

    // In the View menu, and off until the user opens it (a saved layout wins).
    auto* pluginPanel = new usdcc::scripting::PluginManagerSidePanel(plugins);
    window.addSidePanel(pluginPanel, Qt::RightDockWidgetArea);
    pluginPanel->hide();

    // Peers (docs/PEERS.md): rooms with other usdcc instances, found on this machine and
    // network, with the people in them and their chat. Nothing opens a port or goes on
    // the network until a room is hosted or joined, or the panel looks for rooms (§12).
    usdcc::peers::Peers::Options peersOptions = usdcc::app::peersOptions();
    usdcc::ui::PeersController::Hooks peersHooks;
    // A prim path in a chat message selects that prim, if the current stage has it.
    peersHooks.selectPrim = [&stageManager, &window](const QString& path) {
        const usdcc::usd::StageRefPtr stage = stageManager.currentStage();
        const std::string text = path.toStdString();
        if (stage && PXR_NS::SdfPath::IsValidPathString(text) &&
            stage->usdStage()->GetPrimAtPath(PXR_NS::SdfPath(text))) {
            stageManager.setSelectedPaths(stage, {PXR_NS::SdfPath(text)});
        } else {
            window.statusBar()->showMessage(QObject::tr("The current stage has no prim %1").arg(path), 5000);
        }
    };
    peersHooks.currentStage = [&stageManager]() {
        const usdcc::usd::StageRefPtr stage = stageManager.currentStage();
        return stage ? QString::fromStdString(stage->usdStage()->GetRootLayer()->GetDisplayName()) : QString();
    };
    auto* peersController = new usdcc::ui::PeersController(
        std::make_unique<usdcc::peers::Peers>(std::move(peersOptions)), &window, std::move(peersHooks), &window);
    auto* peersPanel = new usdcc::ui::PeersSidePanel(peersController);
    window.addSidePanel(peersPanel, Qt::RightDockWidgetArea);
    peersPanel->hide();
    peersController->showPanel = [peersPanel]() {
        peersPanel->show();
        peersPanel->setCollapsed(false);
        peersPanel->raise();
    };
    // Every room, whatever its type, with its raw data.
    debugMenu->addSeparator();
    QAction* peerRoomsAction = debugMenu->addAction(QObject::tr("Peer &Rooms..."));
    QObject::connect(peerRoomsAction, &QAction::triggered, &window, [&window, peersController]() {
        static QPointer<usdcc::ui::PeerRoomsDebugDialog> dialog;
        if (!dialog) {
            dialog = new usdcc::ui::PeerRoomsDebugDialog(peersController->peers(), &window);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
        }
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    });
    // The Render Queue (docs/FINAL_RENDER.md §5.2): off until a render is queued or the user
    // opens it. Rendering stops when the app quits.
    auto* renderQueuePanel = new usdcc::render::RenderQueueSidePanel(&usdcc::render::RenderScheduler::instance());
    window.addSidePanel(renderQueuePanel, Qt::RightDockWidgetArea);
    renderQueuePanel->hide();
    usdcc::render::setShowRenderQueue([renderQueuePanel]() {
        renderQueuePanel->show();
        renderQueuePanel->setCollapsed(false);
        renderQueuePanel->raise();
    });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        auto& scheduler = usdcc::render::RenderScheduler::instance();
        for (const usdcc::render::RenderJobPtr& job : scheduler.jobs()) scheduler.cancel(job->id);
    });

    // Distributed final renders (docs/FINAL_RENDER.md §7): in every room, this instance says
    // what it renders; render nodes there become workers of the render queue, and with Render
    // for peers on, it renders theirs.
    const auto trustedFingerprints = []() {
        const QString text = QString::fromStdString(usdcc::core::Settings::instance().get<std::string>("FinalRender/trusted"));
        return text.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
    };
    usdcc::render::RenderNetworkOptions renderNetworkOptions;
    renderNetworkOptions.folder =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("render-network"));
    renderNetworkOptions.isNode = []() { return usdcc::core::Settings::instance().get<bool>("FinalRender/node"); };
    renderNetworkOptions.trusted = [trustedFingerprints](const usdcc::peers::Peer& peer) {
        return !peer.fingerprint.isEmpty() && trustedFingerprints().contains(peer.fingerprint, Qt::CaseInsensitive);
    };
    renderNetworkOptions.ask = [&window, trustedFingerprints](const usdcc::peers::Peer& from, const QString& what,
                                                             std::function<void(bool)> answer) {
        if (!usdcc::core::Settings::instance().get<bool>("FinalRender/askUntrusted", true)) {
            answer(false);
            return;
        }
        auto answered = std::make_shared<bool>(false);
        const auto once = [answered, answer](bool take) {
            if (*answered) return;
            *answered = true;
            answer(take);
        };
        usdcc::core::ToastRequest request = usdcc::core::ToastRequest::sticky();
        request.title = QObject::tr("Render for %1?").arg(from.name).toStdString();
        request.body = what.toStdString();
        QList<usdcc::ui::ToastAction> actions;
        actions.append({QObject::tr("Render"), [once]() { once(true); }});
        actions.append({QObject::tr("Always for %1").arg(from.name), [once, from, trustedFingerprints]() {
                            QStringList trusted = trustedFingerprints();
                            if (!from.fingerprint.isEmpty() && !trusted.contains(from.fingerprint)) trusted.append(from.fingerprint);
                            usdcc::core::Settings::instance().setValue("FinalRender/trusted", trusted.join(QLatin1Char(' ')).toStdString());
                            once(true);
                        }});
        actions.append({QObject::tr("Decline"), [once]() { once(false); }});
        window.toasts()->show(request, actions);
    };
    auto* renderNetwork = new usdcc::render::RenderNetwork(std::move(renderNetworkOptions), &window);
    usdcc::render::RenderNetwork::setInstance(renderNetwork);
    // Only render_queue rooms (addRoom() ignores the others).
    for (usdcc::peers::Room* room : peersController->peers()->rooms(usdcc::peers::RoomType::RenderQueue)) {
        renderNetwork->addRoom(room);
    }
    QObject::connect(peersController->peers(), &usdcc::peers::Peers::roomAdded, renderNetwork,
                     [renderNetwork](usdcc::peers::Room* room) { renderNetwork->addRoom(room); });
    // Where this instance's jobs go: app state, kept between runs.
    {
        QSettings settings = usdcc::ui::appSettings();
        usdcc::render::PeerDispatch dispatch;
        const QString distribute = settings.value(QStringLiteral("FinalRender/distribute"), QStringLiteral("local")).toString();
        if (distribute == u"local" || distribute == u"both" || distribute == u"peers") dispatch.distribute = distribute.toStdString();
        const QString delivery = settings.value(QStringLiteral("FinalRender/delivery"), QStringLiteral("packaged")).toString();
        if (delivery == u"packaged" || delivery == u"shared") dispatch.delivery = delivery.toStdString();
        for (const QString& key : settings.value(QStringLiteral("FinalRender/rooms")).toStringList()) {
            dispatch.rooms.push_back(key.toStdString());
        }
        renderNetwork->setDispatch(dispatch);
    }
    QObject::connect(renderNetwork, &usdcc::render::RenderNetwork::dispatchChanged, renderNetwork, [renderNetwork]() {
        const usdcc::render::PeerDispatch dispatch = renderNetwork->dispatch();
        QSettings settings = usdcc::ui::appSettings();
        settings.setValue(QStringLiteral("FinalRender/distribute"), QString::fromStdString(dispatch.distribute));
        settings.setValue(QStringLiteral("FinalRender/delivery"), QString::fromStdString(dispatch.delivery));
        QStringList rooms;
        for (const std::string& key : dispatch.rooms) rooms.append(QString::fromStdString(key));
        settings.setValue(QStringLiteral("FinalRender/rooms"), rooms);
    });
    QObject::connect(peersController->peers(), &usdcc::peers::Peers::roomRemoved, renderNetwork,
                     [renderNetwork](usdcc::peers::Room* room) { renderNetwork->removeRoom(room); });
    // Render > Swarm: the render queue rooms and who is in them.
    auto* swarmPanel = new usdcc::render::SwarmSidePanel(renderNetwork, peersController);
    window.addSidePanel(swarmPanel, Qt::RightDockWidgetArea);
    swarmPanel->hide();
    usdcc::render::setShowSwarm([swarmPanel]() {
        swarmPanel->show();
        swarmPanel->setCollapsed(false);
        swarmPanel->raise();
    });
    auto renderersAdvertised = usdcc::render::FinalRenderers::instance().changed.connect([renderNetwork]() { renderNetwork->advertise(); });
    auto renderNodeSetting = usdcc::core::Settings::instance().changed.connect([renderNetwork](const std::string& key) {
        if (key == "FinalRender/node") renderNetwork->advertise();
    });

    // `presence` says which stage each peer has open.
    auto presenceOnCurrentStage = stageManager.currentStageChanged.connect(
        [peersController](const usdcc::usd::StageRefPtr&) { peersController->updatePresence(); });

    // Developing a plugin from inside the app (docs/PLUGIN_DEV.md).
    scriptEditor->setPluginHooks({[&plugins](const std::string& id) { return plugins.whyNotReloadable(id); },
                                  [&plugins, &python](const std::string& id) { return plugins.reload(id, python); }});
    QObject::connect(pluginPanel, &usdcc::scripting::PluginManagerSidePanel::editPluginRequested, scriptEditor,
                     [scriptEditor](const QString& id, const QString& name, const QString& folder, const QString& mainFile) {
                         scriptEditor->show();
                         scriptEditor->setCollapsed(false);
                         scriptEditor->raise();
                         scriptEditor->openPlugin(id, name, folder, mainFile);
                     });
    // The Schema Editor (docs/SCHEMAS.md §5): usdGenSchema with the app's own Python;
    // Restart as Restart and Load does, back to the Last session; Edit as Text in the
    // Script Editor; and the Schemas panel for Promote to Schema.
    {
        usdcc::usd::SchemaEditorHooks schemaHooks;
        if (python.isAvailable()) {
            schemaHooks.python.executable = python.executable();
            schemaHooks.python.paths = python.settings().paths;
        }
        // A deploy's OpenUSD libraries are beside the executable.
        schemaHooks.python.dllFolders = {std::filesystem::path(QCoreApplication::applicationDirPath().toStdU16String())};
        schemaHooks.restart = [&window, &restartRequested, &restartArguments]() {
            restartRequested = true;
            restartArguments = {QStringLiteral("--open-session"), QLatin1String(usdcc::ui::session::kLastSessionName)};
            window.close();
            if (window.isVisible()) restartRequested = false;
            return restartRequested;
        };
        schemaHooks.editText = [scriptEditor](const QString& path) {
            scriptEditor->show();
            scriptEditor->setCollapsed(false);
            scriptEditor->raise();
            scriptEditor->openFile(path);
        };
        schemaHooks.showSchemasPanel = [&window, &panelTypes, &stageManager]() -> usdcc::usd::SchemaExplorerViewPanel* {
            const QList<usdcc::usd::SchemaExplorerViewPanel*> open = window.findChildren<usdcc::usd::SchemaExplorerViewPanel*>();
            usdcc::usd::SchemaExplorerViewPanel* panel = open.isEmpty() ? nullptr : open.front();
            if (!panel) {
                panel = qobject_cast<usdcc::usd::SchemaExplorerViewPanel*>(
                    panelTypes.open(QStringLiteral("schemas"), stageManager.currentStage(), window.dockManager(), &window));
            }
            if (panel) {
                panel->setAsCurrentTab();
                panel->raise();
            }
            return panel;
        };
        usdcc::usd::setSchemaEditorHooks(std::move(schemaHooks));
    }
    QObject::connect(pluginPanel, &usdcc::scripting::PluginManagerSidePanel::reloadPluginRequested, scriptEditor,
                     &usdcc::scripting::ScriptEditorSidePanel::reloadPlugin);
    QObject::connect(scriptEditor, &usdcc::scripting::ScriptEditorSidePanel::pluginReloaded, pluginPanel,
                     &usdcc::scripting::PluginManagerSidePanel::refresh);

    // The plugins' code, with the menus and registries they add to in place,
    // and before the layout is restored, so their panels come back like the
    // built-in ones (docs/PLUGINS.md §3.3).
    plugins.setPeers(peersController);
    plugins.activate(python);
    pluginPanel->refresh();
    // A plugin's workspaces/ folder, looked in after the settings' (docs/WORKSPACES.md §5).
    {
        QList<usdcc::ui::WorkspaceLibrary::Folder> folders;
        for (const usdcc::scripting::PluginInfo& plugin : plugins.plugins()) {
            if (plugin.state != usdcc::scripting::PluginState::Active) continue;
            const QString folder = QDir(QString::fromStdU16String(plugin.manifest.folder.u16string()))
                                       .absoluteFilePath(QStringLiteral("workspaces"));
            if (QFileInfo(folder).isDir()) folders.append({folder, QString::fromStdString(plugin.manifest.name)});
        }
        if (!folders.isEmpty()) workspaceLibrary.setPluginFolders(folders);
    }
    if (const auto* crashed = plugins.find(plugins.crashedLastStart())) {
        window.statusBar()->showMessage(
            QObject::tr("The plugin %1 crashed the last start, so it's turned off. See View > Plugins.")
                .arg(QString::fromStdString(crashed->manifest.name)));
    }
    // Waits for the window to be visible. Nothing is installed without being asked.
    if (plugins.packagesPending()) {
        usdcc::core::ToastRequest request;
        request.title = QObject::tr("Python packages are needed").toStdString();
        request.body = QObject::tr("Some plugins or your own settings ask for packages that aren't installed.").toStdString();
        request.kind = usdcc::core::ToastKind::Warning;
        window.toasts()->show(request, {{QObject::tr("Install..."), [pluginPanel]() {
                                             pluginPanel->show();
                                             pluginPanel->installPackages();
                                         }}});
    }

    // Every panel for this session must exist before restoring — see
    // MainWindow::restoreLayout()'s declaration for why: so the panels opened
    // from the Stages menu (or duplicated) last time are made again first,
    // showing their stage if it's open (the one from the command line).
    panelTypes.restorePanels(window.dockManager(), &window, [&stageManager](const QString& identifier) {
        for (const usdcc::usd::StageRefPtr& stage : stageManager.stages()) {
            if (stage->usdStage()->GetRootLayer()->GetIdentifier() == identifier.toStdString()) return stage;
        }
        return usdcc::usd::StageRefPtr();
    });
    // A first start has no layout saved: it gets the default workspace once the
    // window has settled (docs/WORKSPACES.md §6.3).
    const bool hadLayout = usdcc::ui::appSettings().contains(QStringLiteral("MainWindow/dockLayout"));
    window.restoreLayout();
    if (!hadLayout && startSession.isEmpty()) {
        QObject::connect(
            &window, &usdcc::ui::MainWindow::layoutSettled, &workspaces,
            [&workspaces]() {
                QString error;
                if (!workspaces.switchTo(QLatin1String(usdcc::ui::WorkspaceController::kDefaultId), &error)) {
                    USDCC_LOG_WARNING("usdcc.workspaces", "{}", error.toStdString());
                }
            },
            Qt::SingleShotConnection);
    }
    if (!startSession.isEmpty()) {
        usdcc::scripting::SessionController::LoadOptions options;
        options.atStart = true;
        options.interactive = false;
        options.discardUnsaved = true;
        QString error;
        if (sessions.load(startSession, options, &error) != usdcc::scripting::SessionController::LoadResult::Loaded) {
            USDCC_LOG_WARNING("usdcc.session", "Couldn't start with the session {}: {}", startSession.toStdString(),
                              error.toStdString());
        }
        if (!positional.isEmpty()) {
            usdcc::assets::allowHostOfUrl(positional.front().toStdString());
            stageManager.openStage(positional.front().toStdString());
        }
    }
    // The Last session, as the window closes and while it's still on screen (§4.5).
    QObject::connect(&window, &usdcc::ui::MainWindow::closing, &window, [&sessions]() {
        QString error;
        if (!sessions.save(QLatin1String(usdcc::ui::session::kLastSessionName), sessions.takePreview(), false, &error)) {
            USDCC_LOG_WARNING("usdcc.session", "Couldn't save the Last session: {}", error.toStdString());
        }
    });

    // The current stage follows the focused view panel, and the title says
    // which it is.
    new usdcc::ui::CurrentStageTracker(window.dockManager(), &stageManager, &window);
    auto updateTitle = [&window, &stageManager](const usdcc::usd::StageRefPtr&) {
        window.setWindowTitle(usdcc::usd::windowTitle(stageManager));
    };
    // Opening or closing another stage can change how the current one is
    // named (shortStageNames()).
    auto titleOnCurrentStage = stageManager.currentStageChanged.connect(updateTitle);
    auto titleOnStageOpened = stageManager.stageOpened.connect(updateTitle);
    auto titleOnStageClosed = stageManager.stageClosed.connect(updateTitle);
    updateTitle(nullptr);

    // The status bar says which layer of the current stage edits go to
    // (docs/LAYER_EDITOR.md §3), following both the current stage and its
    // edit target.
    auto* editingLabel = new QLabel(&window);
    window.statusBar()->addPermanentWidget(editingLabel);
    usdcc::core::Signal<>::Connection editTargetOfCurrent;
    std::function<void()> updateEditing = [editingLabel, &stageManager]() {
        const usdcc::usd::StageRefPtr stage = stageManager.currentStage();
        const PXR_NS::SdfLayerHandle layer = stage ? stage->editTargetLayer() : PXR_NS::SdfLayerHandle();
        const QString variant = stage ? usdcc::usd::variantEditText(*stage) : QString();
        const QString layerName = layer ? QString::fromStdString(layer->GetDisplayName()) : QString();
        editingLabel->setText(!layer              ? QString()
                              : variant.isEmpty() ? QObject::tr("Editing: %1").arg(layerName)
                                                  : QObject::tr("%1, in %2").arg(variant, layerName));
        editingLabel->setToolTip(layer ? QString::fromStdString(layer->GetIdentifier()) : QString());
    };
    auto editingOnCurrentStage = stageManager.currentStageChanged.connect(
        [&editTargetOfCurrent, &updateEditing](const usdcc::usd::StageRefPtr& stage) {
            editTargetOfCurrent = stage ? stage->editTargetChanged.connect([&updateEditing]() { updateEditing(); })
                                        : usdcc::core::Signal<>::Connection();
            updateEditing();
        });
    updateEditing();

    // Downloads in the status bar, and a note when a stage asks for a host that isn't allowed.
    usdcc::usd::installAssetActivity(&window);
    window.show();
    // "Updated to ..." after an update, and the check a few seconds later (docs/UPDATES.md §4).
    updates.start();

    // Once the window is up and its layout settled (so panels the scripts add
    // stay docked, see MainWindow::layoutSettled()), with the initial stage
    // open if there is one: the startup scripts, then each --script, each as
    // one undo step like Debug > Run Python File. Their output goes to the log.
    const QStringList scripts = commandLine.values(scriptOption);
    const bool runStartup = !commandLine.isSet(noStartupScriptsOption);
    QObject::connect(&window, &usdcc::ui::MainWindow::layoutSettled, &window, [&python, &scriptRunner, scripts, runStartup]() {
        usdcc::scripting::runStartupScripts(python, usdcc::scripting::userScriptsFolder(), runStartup);
        for (const QString& script : scripts) {
            if (!python.isAvailable()) {
                USDCC_LOG_ERROR("usdcc.python", "Can't run {}: {}", script.toStdString(), python.unavailableReason());
                continue;
            }
            scriptRunner.runFile(std::filesystem::path(script.toStdU16String()));
        }
    }, Qt::SingleShotConnection);

    const int result = app.exec();
    usdcc::usd::setOpenStageHandler({});  // it holds main()'s objects
    usdcc::usd::UsdaViewPanel::setNewPanelHandler({});
    // The window is closed (and its layout saved) but its panels still exist.
    panelTypes.savePanels(window.dockManager());
    plugins.deactivate(python);
    if (restartRequested) {
        // The same app, with what it was started with that still applies.
        QStringList arguments = restartArguments;
        for (const QString& folder : commandLine.values(pluginPathOption)) arguments << QStringLiteral("--plugin-path") << folder;
        if (commandLine.isSet(noStartupScriptsOption)) arguments << QStringLiteral("--no-startup-scripts");
        USDCC_LOG_INFO("usdcc.app", "Restarting: {}", arguments.join(QLatin1Char(' ')).toStdString());
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments)) {
            USDCC_LOG_ERROR("usdcc.app", "Couldn't start the app again");
        }
    }
    // Restart Now on a downloaded update: Velopack's updater takes over once this process ends.
    updates.applyIfRequested();
    return result;
}
