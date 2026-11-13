#include <QApplication>

#include <filesystem>
#include <functional>
#include <memory>

#include "usdcc/ui/about_dialog.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/current_stage_tracker.h"
#include "usdcc/ui/log_side_panel.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/qt_message_bridge.h"
#include "usdcc/ui/settings_dialog.h"
#include "usdcc/ui/theme.h"
#include "usdcc/ui/toast_manager.h"
#include "usdcc/ui/view_panel_registry.h"

#include <DockManager.h>

#include <QAction>
#include <QCommandLineParser>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSurfaceFormat>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include "usdcc/core/console_log_sink.h"
#include "usdcc/core/file_log_sink.h"
#include "usdcc/core/log.h"
#include "usdcc/core/native_stream_capture.h"
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
#include "usdcc/usd/close_stage_dialog.h"
#include "usdcc/usd/create_menu.h"
#include "usdcc/usd/current_stage_ui.h"
#include "usdcc/usd/dependencies_view_panel.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/layer_editor_view_panel.h"
#include "usdcc/usd/prim_tree_dialog.h"
#include "usdcc/usd/recent_files.h"
#include "usdcc/usd/renderer_settings.h"
#include "usdcc/usd/save_stage_as.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/usda_view_panel.h"
#include "usdcc/usd/variant_editor_dialog.h"
#include "usdcc/usd/log_test_messages.h"
#include "usdcc/usd/material_editor_view_panel.h"
#include "usdcc/usd/shader_nodes_dialog.h"
#include "usdcc/usd/shading_preview_widget.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/properties/data_type_gallery_dialog.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"
#include "usdcc/usd/stage_opener.h"
#include "usdcc/usd/stages_menu.h"
#include "usdcc/usd/tf_diagnostic_bridge.h"
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

namespace {
// Detaches a sink from the hub when it goes out of scope. Declared right after
// QApplication in main(), so it outlives the window and the stage manager and
// the log file still gets what they log while being destroyed.
struct ScopedLogSink {
    std::shared_ptr<usdcc::core::LogSink> sink;
    ~ScopedLogSink() {
        if (sink) usdcc::core::LogHub::instance().removeSink(sink.get());
    }
};
}  // namespace

int main(int argc, char** argv) {
    // Logging comes first, so nothing logged during start-up is lost: records
    // wait in the hub's ring buffer until the log file (below) attaches and
    // replays them. USDCC_LOG sets levels, e.g. "debug" or
    // "info,usdcc.viewport=debug,usd=warning" (docs/LOGGING.md §3.1).
    usdcc::core::setThreadName("main");
    if (const QString spec = qEnvironmentVariable("USDCC_LOG"); !spec.isEmpty()) {
        if (!usdcc::core::LogHub::instance().configureLevels(spec.toStdString())) {
            USDCC_LOG_WARNING("usdcc.app", "Ignored parts of USDCC_LOG=\"{}\" that aren't valid", spec.toStdString());
        }
    }
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
    ScopedLogSink console{logToConsole ? std::make_shared<usdcc::core::ConsoleLogSink>() : nullptr};
    if (console.sink) {
        usdcc::core::LogHub::instance().addSink(console.sink);
    }

    // Must be set before QApplication is constructed (QSurfaceFormat docs).
    // HgiGL_ScopedStateHolder (Hydra/Storm's save-and-restore-caller's-GL-
    // state wrapper, used so Storm can share a GL context with a foreign
    // renderer like Qt's) queries/pushes legacy state that doesn't exist in
    // a Core profile context — confirmed by "GL error: invalid enum" from
    // HgiGLPostPendingGLErrors when a Core profile was requested here.
    // Compatibility profile is what it actually expects.
    QSurfaceFormat format;
    format.setVersion(4, 5);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    // USDCC_GL_DEBUG=1 asks for a debug context: the viewport then logs each
    // GL error the driver reports, at the call that caused it, with a stack.
    if (qEnvironmentVariableIntValue("USDCC_GL_DEBUG") != 0) {
        format.setOption(QSurfaceFormat::DebugContext);
    }
    QSurfaceFormat::setDefaultFormat(format);

    // The viewport is a native child window (QWidget::createWindowContainer),
    // and by default Qt then makes its siblings native too — including the
    // custom title bar, which then receives the mouse itself instead of the
    // main window's hit test, so dragging it no longer moves the window.
    // QWindowKit requires this attribute, set before QApplication exists.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

    QApplication app(argc, argv);

    // Integration tests need isolation before settings, plugins, Python and
    // layouts load. APPDATA alone does not redirect Qt's Windows known folders.
    const auto testProfile = qEnvironmentVariable("USDCC_TEST_PROFILE");
    if (!testProfile.isEmpty() && !testProfile.contains('/') && !testProfile.contains('\\')) {
        QStandardPaths::setTestModeEnabled(true);
        app.setApplicationName("usdcc-test-" + testProfile);
    }

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

    // Needs the application object for its folder (AppLocalDataLocation).
    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
    ScopedLogSink fileLog{usdcc::core::FileLogSink::openInDirectory(logDir.toStdU16String())};
    QString logFilePath;
    if (fileLog.sink) {
        usdcc::core::LogHub::instance().addSink(fileLog.sink);
        // First, so every log file says which build wrote it.
        USDCC_LOG_INFO("usdcc.app", "usdcc {}{} starting", usdcc::core::version(),
                       usdcc::core::isReleaseBuild() ? "" : " (development build)");
        const auto& path = static_cast<usdcc::core::FileLogSink&>(*fileLog.sink).path();
        logFilePath = QString::fromStdU16String(path.generic_u16string());
        USDCC_LOG_INFO("usdcc.app", "Logging to {}", logFilePath.toStdString());
    } else {
        USDCC_LOG_WARNING("usdcc.app", "Could not create a log file in {}", logDir.toStdString());
    }

    // Preferences (docs/SETTINGS.md) load from usdcc.ini from here on; every
    // setting defined later (the theme's first) starts from its saved value.
    usdcc::ui::installAppSettings();
    usdcc::ui::theme::apply(app);
    // Read by the plugin manager and Python, just below.
    usdcc::scripting::definePythonSettings();
    usdcc::scripting::definePluginSettings();
    usdcc::scripting::SessionController::defineSettings();
    // Settings > Viewport's grid and background, there before any viewport opens.
    usdcc::usd::defineViewportDisplaySettings();
    usdcc::usd::defineMaterialPreviewSettings();

    // Reads the plugins' manifests and registers their USD files, running none
    // of their code. Before anything can make a stage: USD's schema registry
    // doesn't take in a plugin's schemas once it exists (docs/PLUGINS.md §3.1).
    std::vector<std::filesystem::path> extraPluginFolders;
    for (const QString& folder : commandLine.values(pluginPathOption)) {
        // Absolute, so the plugins' paths (their libraries, USD folders, Python
        // modules) don't depend on the working directory later on.
        extraPluginFolders.emplace_back(QFileInfo(folder).absoluteFilePath().toStdU16String());
    }
    usdcc::scripting::PluginManager plugins(
        usdcc::scripting::PluginManager::defaultOptions(extraPluginFolders, !commandLine.isSet(noPluginsOption)));
    plugins.scan();
    // Settings > Renderers: each render delegate's own settings, all of them listed only
    // when the dialog opens, as some take a while (a viewport defines its renderer's itself).
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
    // F: fit the active viewport to the selection, or to the whole stage.
    usdcc::usd::ViewportViewPanel::addFitToViewAction(&window);
    // The active viewport's HUD on or off (no key until one is set).
    usdcc::usd::ViewportViewPanel::addToggleHudAction(&window);

    // Scripts reach the app's stages, undo history, tools, window and view
    // panel types through these (usdcc.usd / usdcc.app / usdcc.tools,
    // docs/SCRIPTING.md §4, §8) until this goes out of scope, before the tool
    // set does. ScriptRunner makes each run one undo step per stage it
    // changes. Python view panel types registered in panelTypes outlive the
    // interpreter; they let go of their Python class without it.
    // Sessions (docs/SESSIONS.md). While one loads, a stage opened doesn't get a
    // viewport of its own (below): the session says which panels there are.
    bool sessionLoading = false;
    // While File > Save Stage As puts the new file in the old stage's panels, the
    // stage it opens gets no viewport of its own.
    bool stageSwitching = false;
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
        {&stageManager, undoManager.get(), toolSet.get(), &window, &panelTypes, &plugins, &sessions});
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
    panelTypes.add(QStringLiteral("dependencies"), QObject::tr("Dependencies"),
                   [&stageManager, undoManager](const usdcc::usd::StageRefPtr& stage, const QString& title,
                                                 QWidget* parent) -> usdcc::ui::ViewPanel* {
                       return new usdcc::usd::DependenciesViewPanel(&stageManager, stage, undoManager, title, parent);
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
    auto viewportOnStageOpened = stageManager.stageOpened.connect(
        [&window, &panelTypes, &stageManager, &sessionLoading, &stageSwitching](const usdcc::usd::StageRefPtr& stage) {
            if (sessionLoading || stageSwitching) return;
            auto focus = [&](usdcc::ui::ViewPanel* panel) {
                if (panel) window.dockManager()->setDockWidgetFocused(panel);
                stageManager.setCurrentStage(stage);
            };
            usdcc::usd::ViewportViewPanel* empty = nullptr;
            for (ads::CDockWidget* dockWidget : window.dockManager()->dockWidgetsMap()) {
                auto* existing = qobject_cast<usdcc::usd::ViewportViewPanel*>(dockWidget);
                if (!existing) {
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
                empty->setAsCurrentTab();
                focus(empty);
                return;
            }

            focus(panelTypes.open(QStringLiteral("viewport"), stage, window.dockManager(), &window));
        });

    QMenu* fileMenu = window.mainMenuBar()->addMenu(QObject::tr("&File"));
    QAction* openStageAction = fileMenu->addAction(QObject::tr("&Open Stage..."));
    // Opens off the UI thread behind a progress dialog (the stage named on the
    // command line, above, deliberately just blocks instead: there is no window
    // yet). Owned by `window`.
    auto* stageOpener = new usdcc::usd::StageOpener(&stageManager, &window);
    QObject::connect(stageOpener, &usdcc::usd::StageOpener::busyChanged, openStageAction,
                     [openStageAction](bool busy) { openStageAction->setEnabled(!busy); });
    QObject::connect(stageOpener, &usdcc::usd::StageOpener::showLogRequested, window.logPanel(),
                     &usdcc::ui::LogSidePanel::showLatest);
    QObject::connect(openStageAction, &QAction::triggered, &window, [stageOpener, &window]() {
        const QString path = QFileDialog::getOpenFileName(&window, QObject::tr("Open USD Stage"), QString(),
                                                            QObject::tr("USD Files (*.usd *.usda *.usdc *.usdz)"));
        if (!path.isEmpty()) {
            stageOpener->open(path);
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

    // File > Save Stage: every file layer of the current stage with unsaved
    // changes (docs/LAYER_EDITOR.md §5). Ctrl+Shift+S, as Ctrl+S is the
    // Script Editor's and the Layers panel's own Save.
    QAction* saveStageAction = fileMenu->addAction(QObject::tr("&Save Stage"));
    saveStageAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    saveStageAction->setStatusTip(QObject::tr("Save every layer of the current stage that has unsaved changes"));
    QObject::connect(fileMenu, &QMenu::aboutToShow, saveStageAction, [&stageManager, saveStageAction]() {
        saveStageAction->setEnabled(!usdcc::usd::layer_actions::unsavedLayers(stageManager.currentStage()).empty());
    });
    QObject::connect(saveStageAction, &QAction::triggered, &window, [&stageManager, &window]() {
        QString error;
        if (!usdcc::usd::layer_actions::saveAll(stageManager.currentStage(), &error)) {
            QMessageBox::warning(&window, QObject::tr("Save Stage"), error);
        }
    });
    // File > Save Stage As: the current stage's root layer to another file, which
    // the app then goes on in, in the old stage's place (saveStageAsAndSwitch()). It
    // goes in Open Recent.
    QAction* saveStageAsAction = fileMenu->addAction(QObject::tr("Save Stage &As..."));
    saveStageAsAction->setStatusTip(QObject::tr("Save the current stage to another file, and go on in that file"));
    QObject::connect(fileMenu, &QMenu::aboutToShow, saveStageAsAction, [&stageManager, saveStageAsAction]() {
        saveStageAsAction->setEnabled(stageManager.currentStage() != nullptr);
    });
    QObject::connect(saveStageAsAction, &QAction::triggered, &window, [&stageManager, &window, &stageSwitching]() {
        const usdcc::usd::StageRefPtr stage = stageManager.currentStage();
        if (!stage) return;
        const std::string real = stage->usdStage()->GetRootLayer()->GetRealPath();
        QString path = QFileDialog::getSaveFileName(
            &window, QObject::tr("Save Stage As"),
            real.empty() ? QString() : QFileInfo(QString::fromStdString(real)).absolutePath(),
            QObject::tr("USD Text (*.usda);;USD Binary (*.usdc);;USD (*.usd)"));
        if (path.isEmpty()) return;
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".usda");
        QString error;
        stageSwitching = true;
        const usdcc::usd::StageRefPtr saved =
            usdcc::usd::saveStageAsAndSwitch(stageManager, stage, path.toStdString(), &error);
        stageSwitching = false;
        if (!saved) {
            QMessageBox::warning(&window, QObject::tr("Save Stage As"), error);
            return;
        }
        usdcc::usd::addRecentFile(path);
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
    QObject::connect(showUsdaAction, &QAction::triggered, &window, [&window]() {
        const QString path = QFileDialog::getOpenFileName(&window, QObject::tr("Show USDA"), QString(),
                                                            QObject::tr("USD Files (*.usd *.usda *.usdc *.usdz)"));
        if (!path.isEmpty()) usdcc::usd::showUsdaPreview(path, &window);
    });
    // File > Preview > Show Prims...: as a tree of its prims (docs/SHOW_PRIMS.md).
    QAction* showPrimsAction = previewMenu->addAction(QObject::tr("Show &Prims..."));
    QObject::connect(showPrimsAction, &QAction::triggered, &window, [&window]() {
        const QString path = QFileDialog::getOpenFileName(&window, QObject::tr("Show Prims"), QString(),
                                                            QObject::tr("USD Files (*.usd *.usda *.usdc *.usdz)"));
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

    // File > Close Stage > one entry per open stage, built when it opens.
    QMenu* closeStageMenu = fileMenu->addMenu(QObject::tr("&Close Stage"));
    closeStageMenu->setToolTipsVisible(true);
    QObject::connect(fileMenu, &QMenu::aboutToShow, closeStageMenu,
                     [&stageManager, closeStageMenu]() {
                         closeStageMenu->menuAction()->setEnabled(!stageManager.stages().empty());
                     });
    QObject::connect(closeStageMenu, &QMenu::aboutToShow, &window, [&stageManager, &window, closeStageMenu]() {
        closeStageMenu->clear();
        const auto& stages = stageManager.stages();
        const std::vector<std::string> names = usdcc::usd::shortStageNames(stages);
        for (std::size_t i = 0; i < stages.size(); ++i) {
            const usdcc::usd::StageRefPtr stage = stages[i];
            QAction* action = closeStageMenu->addAction(QString::fromStdString(names[i]));
            action->setToolTip(QString::fromStdString(stage->usdStage()->GetRootLayer()->GetIdentifier()));
            // Queued: the dialog must not run from inside the menu's own event
            // handling, and the stage is held by value until it does.
            QObject::connect(action, &QAction::triggered, &window, [&stageManager, &window, stage]() {
                QMetaObject::invokeMethod(
                    &window, [&stageManager, &window, stage]() {
                        usdcc::usd::closeStageInteractively(&stageManager, stage, &window);
                    },
                    Qt::QueuedConnection);
            });
        }
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
    QAction* settingsAction = editMenu->addAction(QObject::tr("&Settings..."), &window,
                                                  [&window]() { usdcc::ui::SettingsDialog::showDialog(&window); });
    settingsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    settingsAction->setMenuRole(QAction::PreferencesRole);

    // Create > a prim of any type, in the current stage (docs/CREATE_MENU.md).
    window.mainMenuBar()->addMenu(new usdcc::usd::PrimCreateMenu(&stageManager, undoManager, &window));

    // Built after the File menu above so "File" stays the first menu-bar
    // entry (QMenuBar orders entries by insertion order).
    window.buildViewMenu();

    // Stages > <stage> > a new panel of each type, or Close Stage.
    window.mainMenuBar()->addMenu(
        new usdcc::usd::StagesMenu(&stageManager, &panelTypes, window.dockManager(), &window));

    // Developer tools; always shown until a setting can hide it (M10).
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

    // Last in the menu bar: menus plugins add go in before Debug.
    QMenu* helpMenu = window.mainMenuBar()->addMenu(QObject::tr("&Help"));
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
    QObject::connect(pluginPanel, &usdcc::scripting::PluginManagerSidePanel::reloadPluginRequested, scriptEditor,
                     &usdcc::scripting::ScriptEditorSidePanel::reloadPlugin);
    QObject::connect(scriptEditor, &usdcc::scripting::ScriptEditorSidePanel::pluginReloaded, pluginPanel,
                     &usdcc::scripting::PluginManagerSidePanel::refresh);

    // The plugins' code, with the menus and registries they add to in place,
    // and before the layout is restored, so their panels come back like the
    // built-in ones (docs/PLUGINS.md §3.3).
    plugins.activate(python);
    pluginPanel->refresh();
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
    window.restoreLayout();
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
        if (!positional.isEmpty()) stageManager.openStage(positional.front().toStdString());
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

    window.show();

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
    return result;
}
