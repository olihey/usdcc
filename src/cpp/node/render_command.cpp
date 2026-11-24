// usdcc_node render and renderers (docs/RENDER_NODE.md §4.1): one stage, its frames, the
// app's queue with only this machine as its worker, then exit.

#include "node.h"
#include "render_options.h"

#include "usdcc/core/log.h"
#include "usdcc/usd/asset_servers.h"
#include "usdcc/usd/render/final_renderer.h"
#include "usdcc/usd/render/hydra_final_renderer.h"
#include "usdcc/usd/render/render_job.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render/render_spec.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdRender/settings.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <map>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::node {

namespace {
using namespace usdcc::render;

QString q(const std::string& text) { return QString::fromStdString(text); }

QString json(const QJsonObject& object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }

const char* statusName(JobTask::Status status) {
    switch (status) {
    case JobTask::Status::Pending: return "pending";
    case JobTask::Status::Running: return "running";
    case JobTask::Status::Done: return "done";
    case JobTask::Status::Failed: return "failed";
    case JobTask::Status::Cancelled: return "cancelled";
    }
    return "";
}

const char* statusName(RenderJob::Status status) {
    switch (status) {
    case RenderJob::Status::Queued: return "queued";
    case RenderJob::Status::Running: return "running";
    case RenderJob::Status::Done: return "done";
    case RenderJob::Status::Failed: return "failed";
    case RenderJob::Status::Cancelled: return "cancelled";
    }
    return "";
}

// A usage error: why, and exit 2.
int usage(const QString& why) {
    printError(why);
    return kExitUsage;
}
}  // namespace

int runRender(const QCommandLineParser& parser) {
    const QString stagePath = parser.positionalArguments().value(1);
    if (stagePath.isEmpty()) return usage(QStringLiteral("render: which stage? usdcc_node render <stage> (--help)."));
    if (parser.positionalArguments().size() > 2) return usage(QStringLiteral("render: one stage at a time."));

    RenderArguments arguments;
    if (const QString error = readRenderArguments(parser, QDir::currentPath(), &arguments); !error.isEmpty()) {
        return usage(QStringLiteral("render: ") + error);
    }

    // The stage from its file, with its resolver context: what's on disk is what renders.
    const std::string assetEnvironment = parser.value(QStringLiteral("asset-env")).toStdString();
    if (parser.isSet(QStringLiteral("asset-env")) && !usdcc::usd::hasAssetEnvironment(assetEnvironment)) {
        return usage(QStringLiteral("render: the configuration has no asset environment '%1'.").arg(q(assetEnvironment)));
    }
    const std::string identifier = QFileInfo(stagePath).exists() ? QFileInfo(stagePath).absoluteFilePath().toStdString()
                                                                  : stagePath.toStdString();
    const UsdStageRefPtr stage = UsdStage::Open(identifier, usdcc::usd::stageResolverContext(identifier, assetEnvironment));
    if (!stage) return usage(QStringLiteral("render: couldn't open the stage %1.").arg(stagePath));

    std::string error;
    const std::optional<JobRequest> request = prepareRenderRequest(stage, arguments, &error);
    if (!request) return usage(QStringLiteral("render: ") + q(error));
    const RenderJobPtr job = makeRenderJob(*request, &error);
    if (!job) return usage(QStringLiteral("render: ") + q(error));

    const bool asJson = parser.isSet(QStringLiteral("json"));
    const int taskCount = static_cast<int>(job->tasks.size());
    if (asJson) {
        printLine(json({{"event", "job"},
                        {"title", q(job->title)},
                        {"renderer", q(job->rendererId)},
                        {"frames", static_cast<int>(job->frames.size())},
                        {"tasks", taskCount},
                        {"output", q(RenderScheduler::outputFolder(*job))}}));
    } else {
        printLine(QStringLiteral("%1: %2 frame(s), %3 task(s) with %4, to %5")
                      .arg(q(job->title))
                      .arg(job->frames.size())
                      .arg(taskCount)
                      .arg(q(job->rendererName), QDir::toNativeSeparators(q(RenderScheduler::outputFolder(*job)))));
    }

    // The app's queue, with this machine as its only worker.
    RenderScheduler scheduler;
    scheduler.addWorker(std::make_shared<LocalWorker>());
    QEventLoop loop;
    std::map<int, JobTask::Status> printed;
    size_t filesPrinted = 0;
    const auto report = [&]() {
        for (const JobTask& task : job->tasks) {
            const int index = task.task.index;
            auto it = printed.find(index);
            if (it != printed.end() && it->second == task.status) continue;
            if (it == printed.end() && task.status == JobTask::Status::Pending) continue;
            printed[index] = task.status;
            const double frame = job->frames[static_cast<size_t>(task.frameIndex)];
            if (asJson) {
                QJsonObject line{{"event", "task"},  {"index", index},         {"frame", frame},
                                 {"status", statusName(task.status)}, {"attempts", task.attempts}};
                if (!task.error.empty()) line["error"] = q(task.error);
                printLine(json(line));
            } else {
                QString line = QStringLiteral("task %1/%2, frame %3: %4")
                                   .arg(index + 1)
                                   .arg(taskCount)
                                   .arg(q(frameText(frame)), QLatin1String(statusName(task.status)));
                if (!task.error.empty()) line += QStringLiteral(" (%1)").arg(q(task.error));
                printLine(line);
            }
        }
        for (; filesPrinted < job->files.size(); ++filesPrinted) {
            const QString file = QDir::toNativeSeparators(q(job->files[filesPrinted]));
            printLine(asJson ? json({{"event", "file"}, {"path", file}}) : QStringLiteral("wrote %1").arg(file));
        }
    };
    QObject::connect(&scheduler, &RenderScheduler::jobChanged, &loop, [&](const QString&) { report(); });
    QObject::connect(&scheduler, &RenderScheduler::jobEnded, &loop, [&](const QString&) {
        report();
        loop.quit();
    });
    bool cancelled = false;
    installStopHandler([&]() {
        cancelled = true;
        printError(QStringLiteral("Cancelling..."));
        scheduler.cancel(job->id);
    });

    scheduler.submit(job);
    if (job->status != RenderJob::Status::Done && job->status != RenderJob::Status::Failed &&
        job->status != RenderJob::Status::Cancelled) {
        loop.exec();
    }
    report();

    if (asJson) {
        QJsonArray files;
        for (const std::string& file : job->files) files.append(QDir::toNativeSeparators(q(file)));
        QJsonObject line{{"event", "end"}, {"status", statusName(job->status)}, {"files", files}};
        if (!job->error.empty()) line["error"] = q(job->error);
        printLine(json(line));
    }
    if (cancelled || job->status == RenderJob::Status::Cancelled) return kExitCancelled;
    if (job->status != RenderJob::Status::Done) {
        printError(QStringLiteral("render: failed: %1").arg(job->error.empty() ? QStringLiteral("a task failed") : q(job->error)));
        return kExitFailed;
    }
    if (!asJson) printLine(QStringLiteral("done: %1 file(s)").arg(job->files.size()));
    return kExitDone;
}

int runRenderers(const QCommandLineParser& parser) {
    const bool asJson = parser.isSet(QStringLiteral("json"));
    for (const auto& renderer : FinalRenderers::instance().all()) {
        const std::string problem = renderer->setupProblem();
        if (asJson) {
            QJsonObject line{{"id", q(renderer->id())},
                             {"name", q(renderer->name())},
                             {"tiles", renderer->capabilities().tiles},
                             {"ready", problem.empty()}};
            if (!problem.empty()) line["problem"] = q(problem);
            printLine(json(line));
        } else {
            printLine(QStringLiteral("%1  %2%3")
                          .arg(q(renderer->id()), q(renderer->name()),
                               problem.empty() ? QString() : QStringLiteral("  (can't render here: %1)").arg(q(problem))));
        }
    }
    return kExitDone;
}

}  // namespace usdcc::node
