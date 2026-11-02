#include <QApplication>

#include <memory>

#include "usdcc/ui/log_side_panel.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/qt_message_bridge.h"
#include "usdcc/ui/theme.h"
#include "usdcc/ui/view_panel_registry.h"

#include <DockManager.h>

#include <QAction>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QStandardPaths>
#include <QSurfaceFormat>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include "usdcc/core/console_log_sink.h"
#include "usdcc/core/file_log_sink.h"
#include "usdcc/core/log.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/close_stage_dialog.h"
#include "usdcc/usd/log_test_messages.h"
#include "usdcc/usd/outliner_view_panel.h"
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
    // Then the bridges, before anything can emit: OpenUSD's diagnostics and
    // Qt's messages (docs/LOGGING.md §6). Both outlive everything below.
    usdcc::usd::TfDiagnosticBridge usdDiagnostics;
    usdcc::ui::QtMessageBridge qtMessages;
    // With the bridges in, neither OpenUSD nor Qt prints anything itself any
    // more; this keeps it visible in a terminal and the debugger.
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
    QSurfaceFormat::setDefaultFormat(format);

    // The viewport is a native child window (QWidget::createWindowContainer),
    // and by default Qt then makes its siblings native too — including the
    // custom title bar, which then receives the mouse itself instead of the
    // main window's hit test, so dragging it no longer moves the window.
    // QWindowKit requires this attribute, set before QApplication exists.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

    QApplication app(argc, argv);

    // Needs the application object for its folder (AppLocalDataLocation).
    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
    ScopedLogSink fileLog{usdcc::core::FileLogSink::openInDirectory(logDir.toStdU16String())};
    QString logFilePath;
    if (fileLog.sink) {
        usdcc::core::LogHub::instance().addSink(fileLog.sink);
        const auto& path = static_cast<usdcc::core::FileLogSink&>(*fileLog.sink).path();
        logFilePath = QString::fromStdU16String(path.generic_u16string());
        USDCC_LOG_INFO("usdcc.app", "Logging to {}", logFilePath.toStdString());
    } else {
        USDCC_LOG_WARNING("usdcc.app", "Could not create a log file in {}", logDir.toStdString());
    }

    usdcc::ui::theme::apply(app);

    // Declared before `window` (and so destroyed after it, per C++'s
    // reverse-construction-order rule) since the viewport panel created
    // below is parented into `window` and holds usdcc::core::Signal
    // connections to this (see stage_manager.h). Owned here (the app's
    // composition root) rather than by MainWindow, which stays decoupled
    // from any one specific StageManager instance.
    usdcc::usd::StageManager stageManager;
    auto undoManager = std::make_shared<usdcc::usd::UndoManager>();
    // The ViewPanel types the Stages menu (and the viewport for a newly opened
    // stage) can create; filled in below, once the tool set exists. Declared
    // before `window` so it outlives the menu that reads it.
    usdcc::ui::ViewPanelRegistry panelTypes;

    usdcc::ui::MainWindow window;
    window.logPanel()->setLogFile(logFilePath);

    // The editing tools and which is active, shared by every viewport and
    // driven by the tool rail / options bar in the main window's layout (the
    // toolbars are owned by `window`).
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    new usdcc::usd::ViewportToolBars(&window, toolSet, &stageManager);

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

    // The stage passed on the command line, if any. Without one (or if it
    // failed to open) the app starts with no stage: the panels below are
    // empty, with their Stage dropdowns disabled, until one is opened.
    const QStringList args = app.arguments();
    usdcc::usd::StageRefPtr initialStage;
    if (args.size() > 1) {
        initialStage = stageManager.openStage(args.at(1).toStdString());
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
    auto viewportOnStageOpened = stageManager.stageOpened.connect(
        [&window, &panelTypes](const usdcc::usd::StageRefPtr& stage) {
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
                return;
            }

            panelTypes.open(QStringLiteral("viewport"), stage, window.dockManager(), &window);
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
    // A closed stage's undo history goes with it (it also holds the stage).
    auto undoOnStageClosed = stageManager.stageClosed.connect(
        [undoManager](const usdcc::usd::StageRefPtr& stage) { undoManager->removeStage(stage); });

    QMenu* editMenu = window.mainMenuBar()->addMenu(QObject::tr("&Edit"));
    QAction* undoAction = editMenu->addAction(QObject::tr("&Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    QObject::connect(undoAction, &QAction::triggered, &window,
                     [&stageManager, undoManager]() {
                         if (auto stack = undoManager->stack(stageManager.currentStage())) stack->undo();
                     });
    QAction* redoAction = editMenu->addAction(QObject::tr("&Redo"));
    redoAction->setShortcut(QKeySequence::Redo);
    QObject::connect(redoAction, &QAction::triggered, &window,
                     [&stageManager, undoManager]() {
                         if (auto stack = undoManager->stack(stageManager.currentStage())) stack->redo();
                     });

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
    // Checks for the Log panel (docs/LOGGING.md §7): one message of every
    // level from every source, and a large burst.
    debugMenu->addSeparator();
    QAction* testMessagesAction = debugMenu->addAction(QObject::tr("Emit &Test Log Messages"));
    QObject::connect(testMessagesAction, &QAction::triggered, &window, []() { usdcc::usd::emitLogTestMessages(); });
    QAction* burstAction = debugMenu->addAction(QObject::tr("Emit 10 000 Log &Messages"));
    QObject::connect(burstAction, &QAction::triggered, &window, []() { usdcc::usd::emitLogBurst(10000); });

    // Every panel for this session must exist before restoring — see
    // MainWindow::restoreLayout()'s declaration for why.
    window.restoreLayout();

    window.show();

    return app.exec();
}
