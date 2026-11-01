#include <QApplication>

#include <memory>

#include "usdcc/ui/main_window.h"
#include "usdcc/ui/theme.h"

#include <DockManager.h>

#include <QAction>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QSurfaceFormat>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/close_stage_dialog.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/properties/data_type_gallery_dialog.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"
#include "usdcc/usd/stage_opener.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_tool_bars.h"
#include "usdcc/usd/viewport_view_panel.h"

namespace {
// Display title for a ViewportViewPanel opened later for `stage` (the startup
// panel is just "Viewport"). Deliberately only the *title*: ADS uses a dock
// widget's objectName — which defaults to the title and must be unique — as
// its identity when saving/restoring layouts, so the startup panel keeps the
// stable name "Viewport" that saved layouts refer to, and later panels get
// their own unique objectName separately (see the stageOpened handler).
QString viewportTitleForStage(const usdcc::usd::StageRefPtr& stage) {
    return QStringLiteral("Viewport - %1")
        .arg(QString::fromStdString(stage->usdStage()->GetRootLayer()->GetDisplayName()));
}
}  // namespace

int main(int argc, char** argv) {
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
    usdcc::ui::theme::apply(app);

    // Declared before `window` (and so destroyed after it, per C++'s
    // reverse-construction-order rule) since the viewport panel created
    // below is parented into `window` and holds usdcc::core::Signal
    // connections to this (see stage_manager.h). Owned here (the app's
    // composition root) rather than by MainWindow, which stays decoupled
    // from any one specific StageManager instance.
    usdcc::usd::StageManager stageManager;
    auto undoManager = std::make_shared<usdcc::usd::UndoManager>();

    usdcc::ui::MainWindow window;

    // The editing tools and which is active, shared by every viewport and
    // driven by the tool rail / options bar in the main window's layout (the
    // toolbars are owned by `window`).
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    new usdcc::usd::ViewportToolBars(&window, toolSet, &stageManager);

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
    // reopened), otherwise a new ViewportViewPanel tabbed next to a viewport
    // on screen. Viewports leave this choice to this handler (see
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
        [&stageManager, &window, toolSet, undoManager](const usdcc::usd::StageRefPtr& stage) {
            usdcc::usd::ViewportViewPanel* empty = nullptr;
            usdcc::usd::ViewportViewPanel* onScreen = nullptr;  // to tab a new viewport next to
            for (ads::CDockWidget* dockWidget : window.dockManager()->dockWidgetsMap()) {
                auto* existing = qobject_cast<usdcc::usd::ViewportViewPanel*>(dockWidget);
                if (!existing) {
                    continue;
                }
                if (!existing->isClosed() && !onScreen) {
                    onScreen = existing;
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

            auto* newViewport = new usdcc::usd::ViewportViewPanel(&stageManager, stage, toolSet, undoManager,
                                                                  viewportTitleForStage(stage), &window);
            // Must be unique and set before the dock manager registers this
            // widget; the stage's cache id is unique per open stage.
            newViewport->setObjectName(QStringLiteral("Viewport-%1").arg(stage->cacheId()));
            if (onScreen && onScreen->dockAreaWidget()) {
                window.dockManager()->addDockWidgetTabToArea(newViewport, onScreen->dockAreaWidget());
            } else {
                window.dockManager()->addDockWidget(ads::CenterDockWidgetArea, newViewport);
            }
            newViewport->setAsCurrentTab();
        });

    QMenu* fileMenu = window.mainMenuBar()->addMenu(QObject::tr("&File"));
    QAction* openStageAction = fileMenu->addAction(QObject::tr("&Open Stage..."));
    // Opens off the UI thread behind a progress dialog (the stage named on the
    // command line, above, deliberately just blocks instead: there is no window
    // yet). Owned by `window`.
    auto* stageOpener = new usdcc::usd::StageOpener(&stageManager, &window);
    QObject::connect(stageOpener, &usdcc::usd::StageOpener::busyChanged, openStageAction,
                     [openStageAction](bool busy) { openStageAction->setEnabled(!busy); });
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

    // Every panel for this session must exist before restoring — see
    // MainWindow::restoreLayout()'s declaration for why.
    window.restoreLayout();

    window.show();

    return app.exec();
}
