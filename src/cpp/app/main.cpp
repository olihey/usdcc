#include <QApplication>

#include "usdcc/ui/main_window.h"

#ifdef USDCC_USD_AVAILABLE
#include <DockManager.h>

#include <QAction>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QSurfaceFormat>

#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/viewport_view_panel.h"
#endif

int main(int argc, char** argv) {
#ifdef USDCC_USD_AVAILABLE
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
#endif

    QApplication app(argc, argv);

#ifdef USDCC_USD_AVAILABLE
    // Declared before `window` (and so destroyed after it, per C++'s
    // reverse-construction-order rule) since the viewport panel created
    // below is parented into `window` and holds usdcc::core::Signal
    // connections to this (see stage_manager.h). Owned here rather than by
    // MainWindow so usdcc_ui itself stays USD-agnostic and buildable without
    // USD_INSTALL (see docs/PLAN.md milestone M0).
    usdcc::usd::StageManager stageManager;
#endif

    usdcc::ui::MainWindow window;

#ifdef USDCC_USD_AVAILABLE
    // The center dock area is established here (rather than by MainWindow)
    // since it's the first USD-dependent panel — see MainWindow's comment on
    // why it no longer seeds a stand-in there itself.
    auto* viewport = new usdcc::usd::ViewportViewPanel(&stageManager, "Viewport", &window);
    auto* centerArea = window.dockManager()->addDockWidget(ads::CenterDockWidgetArea, viewport);

    auto* outliner = new usdcc::usd::OutlinerViewPanel(&stageManager, "Outliner", &window);
    window.dockManager()->addDockWidgetTabToArea(outliner, centerArea);

    auto* attributes = new usdcc::usd::AttributesViewPanel(&stageManager, "Attributes", &window);
    window.dockManager()->addDockWidgetTabToArea(attributes, centerArea);

    QMenu* fileMenu = window.menuBar()->addMenu(QObject::tr("&File"));
    QAction* openStageAction = fileMenu->addAction(QObject::tr("&Open Stage..."));
    QObject::connect(openStageAction, &QAction::triggered, &window, [&stageManager, &window]() {
        const QString path = QFileDialog::getOpenFileName(&window, QObject::tr("Open USD Stage"), QString(),
                                                            QObject::tr("USD Files (*.usd *.usda *.usdc *.usdz)"));
        if (!path.isEmpty()) {
            stageManager.openStage(path.toStdString());
        }
    });

    // Optional: open a stage passed on the command line, so a stage can be
    // viewed without going through File > Open Stage first.
    const QStringList args = app.arguments();
    if (args.size() > 1) {
        stageManager.openStage(args.at(1).toStdString());
    }
#endif

    // Every panel for this session must exist before restoring — see
    // MainWindow::restoreLayout()'s declaration for why.
    window.restoreLayout();

    window.show();

    return app.exec();
}
