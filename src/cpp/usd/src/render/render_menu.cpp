#include "usdcc/usd/render/render_menu.h"

#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/render/render_network.h"
#include "usdcc/usd/render/render_queue.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/validation_fix.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <QAction>
#include <QMessageBox>

namespace usdcc::render {

namespace {
std::function<void()>& showQueue() {
    static std::function<void()> show;
    return show;
}
std::function<void()>& showSwarm() {
    static std::function<void()> show;
    return show;
}
}  // namespace

void setShowRenderQueue(std::function<void()> show) { showQueue() = std::move(show); }
void setShowSwarm(std::function<void()> show) { showSwarm() = std::move(show); }

RenderJobPtr submitRender(const usdcc::usd::StageRefPtr& stage, const PXR_NS::SdfPath& settings, bool animation,
                          QWidget* parent, std::string* error, const std::string& rendererOverride) {
    // Validation/beforeRender (docs/VALIDATION.md §3.3): only where there's someone to ask.
    if (parent && !usdcc::usd::validation::confirmValidated(stage, usdcc::usd::validation::Gate::Render, parent)) {
        if (error) *error = "Not rendered, for the stage's validation errors.";
        return nullptr;
    }
    std::string why;
    RenderJobPtr job;
    if (!stage) {
        why = "There's no stage to render.";
    } else {
        JobRequest request;
        request.stage = stage->usdStage();
        request.settings = settings;
        request.frames = animation ? JobRequest::Frames::Range : JobRequest::Frames::Current;
        request.currentTime = stage->time();
        request.rendererOverride = rendererOverride;
        if (usdcc::usd::ViewportViewPanel* viewport = usdcc::usd::ViewportViewPanel::activeViewport();
            viewport && viewport->currentStage() == stage) {
            request.viewportCamera = usdcc::usd::viewportRenderCamera(*viewport);
        }
        job = makeRenderJob(request, &why);
    }
    if (!job) {
        if (error) *error = why;
        if (parent) QMessageBox::warning(parent, QObject::tr("Render"), QString::fromStdString(why));
        return nullptr;
    }
    // Render on a queue: the job is the queue's, and this instance doesn't schedule it.
    if (job->spec.options.distribute == "queue") {
        RenderQueueClient* queues = RenderQueueClient::instance();
        const RenderNetwork* network = RenderNetwork::instance();
        const QString queue = network ? QString::fromStdString(network->dispatch().queue) : QString();
        QString queueError;
        if (!queues || !queues->submit(job, queue, &queueError)) {
            if (queueError.isEmpty()) queueError = QObject::tr("there's no render queue here");
            if (error) *error = queueError.toStdString();
            if (parent) QMessageBox::warning(parent, QObject::tr("Render"), QObject::tr("Couldn't send the job: %1").arg(queueError));
            return nullptr;
        }
        if (showQueue()) showQueue()();
        return job;
    }
    RenderScheduler::instance().submit(job);
    if (showQueue()) showQueue()();
    return job;
}

QMenu* createRenderMenu(usdcc::usd::StageManager* stageManager, usdcc::ui::ViewPanelRegistry* panelTypes,
                        ads::CDockManager* dockManager, QWidget* parent) {
    auto* menu = new QMenu(QObject::tr("&Render"), parent);
    QAction* frame = menu->addAction(QObject::tr("Render &Frame"), parent, [stageManager, parent]() {
        submitRender(stageManager->currentStage(), {}, false, parent);
    });
    frame->setShortcut(QKeySequence(Qt::Key_F12));
    frame->setToolTip(QObject::tr("Render the frame the timeline shows, with the stage's render settings"));
    QAction* animation = menu->addAction(QObject::tr("Render &Animation"), parent, [stageManager, parent]() {
        submitRender(stageManager->currentStage(), {}, true, parent);
    });
    animation->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F12));
    animation->setToolTip(QObject::tr("Render the render settings' frame range, or the stage's"));
    menu->addSeparator();
    menu->addAction(QObject::tr("Render &Settings"), parent, [stageManager, panelTypes, dockManager, parent]() {
        panelTypes->open(QStringLiteral("renderSettings"), stageManager->currentStage(), dockManager, parent);
    });
    menu->addAction(QObject::tr("Render &Queue"), parent, []() {
        if (showQueue()) showQueue()();
    });
    QAction* swarm = menu->addAction(QObject::tr("S&warm"), parent, []() {
        if (showSwarm()) showSwarm()();
    });
    swarm->setToolTip(QObject::tr("The render queue rooms, who is in them, what they render with and what they are rendering"));
    // Nothing to render without a stage.
    QObject::connect(menu, &QMenu::aboutToShow, menu, [stageManager, frame, animation]() {
        const bool any = stageManager->currentStage() != nullptr;
        frame->setEnabled(any);
        animation->setEnabled(any);
    });
    return menu;
}

}  // namespace usdcc::render
