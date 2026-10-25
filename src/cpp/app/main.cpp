#include <QApplication>

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
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/stage_manager.h"
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

    QApplication app(argc, argv);
    usdcc::ui::theme::apply(app);

    // Declared before `window` (and so destroyed after it, per C++'s
    // reverse-construction-order rule) since the viewport panel created
    // below is parented into `window` and holds usdcc::core::Signal
    // connections to this (see stage_manager.h). Owned here (the app's
    // composition root) rather than by MainWindow, which stays decoupled
    // from any one specific StageManager instance.
    usdcc::usd::StageManager stageManager;

    usdcc::ui::MainWindow window;

    // A ViewPanel always has an associated Stage (see docs/PLAN.md §6
    // "ViewPanel: mandatory Stage association") — resolve one *before*
    // constructing any panel below. Prefer the stage passed on the command
    // line; fall back to a fresh, empty "Untitled" in-memory stage (matching
    // common DCC startup UX) if none was given, or if that identifier failed
    // to open.
    const QStringList args = app.arguments();
    usdcc::usd::StageRefPtr initialStage;
    if (args.size() > 1) {
        initialStage = stageManager.openStage(args.at(1).toStdString());
    }
    if (!initialStage) {
        initialStage = stageManager.createStage();
    }

    // The center dock area is established here (rather than by MainWindow)
    // since it's the first USD-dependent panel — see MainWindow's comment on
    // why it no longer seeds a stand-in there itself.
    auto* viewport =
        new usdcc::usd::ViewportViewPanel(&stageManager, initialStage, "Viewport", &window);
    auto* centerArea = window.dockManager()->addDockWidget(ads::CenterDockWidgetArea, viewport);

    auto* outliner = new usdcc::usd::OutlinerViewPanel(&stageManager, initialStage, "Outliner", &window);
    window.dockManager()->addDockWidgetTabToArea(outliner, centerArea);

    auto* attributes = new usdcc::usd::AttributesViewPanel(&stageManager, initialStage, "Attributes", &window);
    window.dockManager()->addDockWidgetTabToArea(attributes, centerArea);

    // Every stage opened from here on (e.g. via File > Open Stage below) gets
    // its own ViewportViewPanel, tabbed alongside the initial one above. Safe
    // from double-creating one for initialStage itself: its stageOpened
    // already fired synchronously above, before this connection existed.
    // Kept alive for the whole app lifetime by living in main()'s own scope.
    //
    // Deliberately re-queries `viewport`'s current dock area at call time
    // (rather than capturing `centerArea` itself) since restoreLayout()
    // below calls ADS's restoreState(), which can rebuild the dock area/
    // container tree from the saved layout and replace CDockAreaWidget
    // instances — a CDockAreaWidget* captured before that point can be
    // dangling by the time this connection fires. `viewport` is a QPointer
    // since ViewPanel self-closes if initialStage itself is later closed;
    // when that's happened, just re-establish a fresh center area.
    QPointer<usdcc::usd::ViewportViewPanel> initialViewport = viewport;
    auto viewportOnStageOpened = stageManager.stageOpened.connect(
        [&stageManager, &window, initialViewport](const usdcc::usd::StageRefPtr& stage) {
            auto* newViewport =
                new usdcc::usd::ViewportViewPanel(&stageManager, stage, viewportTitleForStage(stage), &window);
            // Must be unique and set before the dock manager registers this
            // widget; the stage's cache id is unique per open stage.
            newViewport->setObjectName(QStringLiteral("Viewport-%1").arg(stage->cacheId()));
            if (initialViewport) {
                window.dockManager()->addDockWidgetTabToArea(newViewport, initialViewport->dockAreaWidget());
            } else {
                window.dockManager()->addDockWidget(ads::CenterDockWidgetArea, newViewport);
            }
        });

    QMenu* fileMenu = window.menuBar()->addMenu(QObject::tr("&File"));
    QAction* openStageAction = fileMenu->addAction(QObject::tr("&Open Stage..."));
    QObject::connect(openStageAction, &QAction::triggered, &window, [&stageManager, &window]() {
        const QString path = QFileDialog::getOpenFileName(&window, QObject::tr("Open USD Stage"), QString(),
                                                            QObject::tr("USD Files (*.usd *.usda *.usdc *.usdz)"));
        if (!path.isEmpty()) {
            stageManager.openStage(path.toStdString());
        }
    });

    // Built after the File menu above so "File" stays the first menu-bar
    // entry (QMenuBar orders entries by insertion order).
    window.buildViewMenu();

    // Every panel for this session must exist before restoring — see
    // MainWindow::restoreLayout()'s declaration for why.
    window.restoreLayout();

    window.show();

    return app.exec();
}
