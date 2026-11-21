#include "usdcc/usd/render/render_scheduler.h"

#include "usdcc/core/log.h"
#include "usdcc/usd/color_management.h"
#include "usdcc/usd/linear_exr.h"

#include <pxr/usd/sdf/types.h>

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <cstring>

namespace usdcc::render {

namespace {

// Whether an AOV of this USD type has one channel.
bool scalarType(const PXR_NS::TfToken& dataType) {
    static const std::vector<std::string> scalars = {"float", "double", "half", "int", "uint", "int64", "uint64", "uchar", "bool"};
    return std::find(scalars.begin(), scalars.end(), dataType.GetString()) != scalars.end();
}

bool writePng(const std::string& path, const FloatImage& image, bool displayColors, std::string* error) {
    QImage linear = toQImage(image);
    if (displayColors) {
        const auto transform = usdcc::usd::ColorManagement::instance().imageTransform();
        if (transform) {
            for (int y = 0; y < linear.height(); ++y) {
                transform->apply(reinterpret_cast<float*>(linear.scanLine(y)), static_cast<size_t>(linear.width()));
            }
        }
    }
    const QImage out = linear.convertToFormat(QImage::Format_RGBA8888);
    if (!out.save(QString::fromStdString(path), "PNG")) {
        if (error) *error = "couldn't write " + path;
        return false;
    }
    return true;
}

}  // namespace

QImage toQImage(const FloatImage& image) {
    QImage result(image.width, image.height, QImage::Format_RGBA32FPx4);
    if (result.isNull()) return result;
    for (int y = 0; y < image.height; ++y) {
        std::memcpy(result.scanLine(y), image.row(y), static_cast<size_t>(image.width) * 4 * sizeof(float));
    }
    return result;
}

std::vector<std::string> writeFrameFiles(const RenderJob& job, int frameIndex, const std::map<std::string, FloatImage>& aovs,
                                         std::string* error) {
    std::vector<std::string> files;
    const double frame = job.frames.at(static_cast<size_t>(frameIndex));
    const int width = job.spec.resolution[0];
    const int height = job.spec.resolution[1];
    const bool png = job.spec.options.format == "png";
    for (const ProductSpec& product : job.spec.products) {
        if (png) {
            for (size_t i = 0; i < product.aovs.size(); ++i) {
                const RenderAov& aov = product.aovs[i];
                const auto image = aovs.find(aov.name);
                if (image == aovs.end()) continue;
                const std::string path = outputPath(job, product, aov.name, frame);
                QDir().mkpath(QFileInfo(QString::fromStdString(path)).absolutePath());
                // The color through the view transform; data AOVs as they are.
                if (!writePng(path, image->second, i == 0 && !scalarType(aov.dataType), error)) return {};
                files.push_back(path);
            }
            continue;
        }
        std::vector<usdcc::usd::LinearExrLayer> layers;
        for (const RenderAov& aov : product.aovs) {
            const auto image = aovs.find(aov.name);
            if (image == aovs.end() || image->second.width != width || image->second.height != height) continue;
            layers.push_back({layers.empty() ? std::string() : aov.name, image->second.rgba.data(), scalarType(aov.dataType) ? 1 : 4});
        }
        if (layers.empty()) continue;
        const std::string path = outputPath(job, product, std::string(), frame);
        QDir().mkpath(QFileInfo(QString::fromStdString(path)).absolutePath());
        std::string why;
        if (!usdcc::usd::writeLinearExrLayers(path, layers, width, height, &why)) {
            if (error) *error = "Couldn't write " + path + ": " + why;
            return {};
        }
        files.push_back(path);
    }
    if (files.empty() && error) *error = "The renderer gave none of the products' AOVs";
    return files;
}

// --- LocalWorker

std::string LocalWorker::name() const { return "This instance"; }

bool LocalWorker::canRun(const RenderJob& job) const {
    const std::shared_ptr<FinalRenderer> renderer = FinalRenderers::instance().find(job.rendererId);
    if (!renderer || !renderer->setupProblem().empty()) return false;
    return job.tilesPerFrame() == 1 || renderer->capabilities().tiles;
}

std::unique_ptr<RenderTaskHandle> LocalWorker::start(const RenderJob& job, const JobTask& task, TaskCallbacks callbacks) {
    const std::shared_ptr<FinalRenderer> renderer = FinalRenderers::instance().find(job.rendererId);
    if (!renderer) {
        if (callbacks.failed) callbacks.failed("There's no renderer " + job.rendererId + " here");
        return nullptr;
    }
    return renderer->start(task.task, std::move(callbacks));
}

// --- RenderScheduler

RenderScheduler::RenderScheduler(QObject* parent) : QObject(parent) {}

RenderScheduler::~RenderScheduler() {
    for (auto& [worker, running] : m_running) {
        if (running.handle) running.handle->cancel();
    }
}

RenderScheduler& RenderScheduler::instance() {
    static RenderScheduler* scheduler = [] {
        auto* s = new RenderScheduler();
        s->addWorker(std::make_shared<LocalWorker>());
        return s;
    }();
    return *scheduler;
}

void RenderScheduler::addWorker(std::shared_ptr<Worker> worker) {
    if (!worker) return;
    m_workers.push_back(std::move(worker));
    schedule();
}

void RenderScheduler::removeWorker(const std::shared_ptr<Worker>& worker) {
    const auto found = m_running.find(worker.get());
    if (found != m_running.end()) {
        Running running = std::move(found->second);
        m_running.erase(found);
        if (running.handle) running.handle->cancel();
        if (running.job && running.task >= 0) {
            JobTask& task = running.job->tasks[static_cast<size_t>(running.task)];
            task.status = JobTask::Status::Pending;  // someone else's now
            task.progress = 0.0;
            Q_EMIT jobChanged(QString::fromStdString(running.job->id));
        }
    }
    m_workers.erase(std::remove(m_workers.begin(), m_workers.end(), worker), m_workers.end());
    schedule();
}

void RenderScheduler::submit(RenderJobPtr job) {
    if (!job) return;
    job->status = RenderJob::Status::Queued;
    m_jobs.push_back(job);
    USDCC_LOG_INFO("usdcc.render", "Queued {}: {} with {}, {} frame(s), {} task(s)", job->id, job->title, job->rendererName,
                   job->frames.size(), job->tasks.size());
    Q_EMIT jobsChanged();
    schedule();
}

RenderJobPtr RenderScheduler::job(const std::string& jobId) const {
    const auto found = std::find_if(m_jobs.begin(), m_jobs.end(), [&jobId](const RenderJobPtr& j) { return j->id == jobId; });
    return found != m_jobs.end() ? *found : nullptr;
}

bool RenderScheduler::busy() const {
    return std::any_of(m_jobs.begin(), m_jobs.end(), [](const RenderJobPtr& j) {
        return j->status == RenderJob::Status::Queued || j->status == RenderJob::Status::Running;
    });
}

std::string RenderScheduler::outputFolder(const RenderJob& job) {
    if (job.spec.products.empty() || job.frames.empty()) return {};
    const ProductSpec& product = job.spec.products.front();
    const std::string path = outputPath(job, product, product.aovs.empty() ? std::string() : product.aovs.front().name, job.frames.front());
    return QFileInfo(QString::fromStdString(path)).absolutePath().toStdString();
}

void RenderScheduler::cancelRunning(const std::string& jobId) {
    for (auto it = m_running.begin(); it != m_running.end();) {
        if (it->second.job && it->second.job->id == jobId) {
            if (it->second.handle) it->second.handle->cancel();
            it = m_running.erase(it);
        } else {
            ++it;
        }
    }
}

void RenderScheduler::cancel(const std::string& jobId) {
    const RenderJobPtr found = job(jobId);
    if (!found || (found->status != RenderJob::Status::Queued && found->status != RenderJob::Status::Running)) return;
    cancelRunning(jobId);
    for (JobTask& task : found->tasks) {
        if (task.status == JobTask::Status::Pending || task.status == JobTask::Status::Running) {
            task.status = JobTask::Status::Cancelled;
        }
    }
    m_frames.erase(jobId);
    endJob(found, RenderJob::Status::Cancelled);
    schedule();
}

void RenderScheduler::retry(const std::string& jobId) {
    const RenderJobPtr found = job(jobId);
    if (!found || found->status == RenderJob::Status::Queued || found->status == RenderJob::Status::Running) return;
    bool any = false;
    for (JobTask& task : found->tasks) {
        if (task.status == JobTask::Status::Failed || task.status == JobTask::Status::Cancelled) {
            task.status = JobTask::Status::Pending;
            task.attempts = 0;
            task.progress = 0.0;
            task.error.clear();
            any = true;
        }
    }
    if (!any) return;
    // A frame's tiles that were done went with the frame's buffers: render them again.
    for (JobTask& task : found->tasks) {
        const bool frameIncomplete = std::any_of(found->tasks.begin(), found->tasks.end(), [&task](const JobTask& other) {
            return other.frameIndex == task.frameIndex && other.status == JobTask::Status::Pending;
        });
        if (frameIncomplete && task.status == JobTask::Status::Done) task.status = JobTask::Status::Pending;
    }
    found->status = RenderJob::Status::Queued;
    found->error.clear();
    found->finished.reset();
    Q_EMIT jobChanged(QString::fromStdString(jobId));
    schedule();
}

void RenderScheduler::remove(const std::string& jobId) {
    const RenderJobPtr found = job(jobId);
    if (!found) return;
    if (found->status == RenderJob::Status::Queued || found->status == RenderJob::Status::Running) cancel(jobId);
    m_frames.erase(jobId);
    m_jobs.erase(std::remove(m_jobs.begin(), m_jobs.end(), found), m_jobs.end());
    Q_EMIT jobsChanged();
}

void RenderScheduler::clearEnded() {
    const auto ended = [](const RenderJobPtr& j) {
        return j->status != RenderJob::Status::Queued && j->status != RenderJob::Status::Running;
    };
    if (std::none_of(m_jobs.begin(), m_jobs.end(), ended)) return;
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(), ended), m_jobs.end());
    Q_EMIT jobsChanged();
}

void RenderScheduler::schedule() {
    if (m_scheduling) return;  // a worker failing at once calls back in
    m_scheduling = true;
    for (const std::shared_ptr<Worker>& worker : m_workers) {
        if (m_running.count(worker.get())) continue;
        bool started = false;
        for (const RenderJobPtr& job : m_jobs) {
            if (job->status != RenderJob::Status::Queued && job->status != RenderJob::Status::Running) continue;
            if (!worker->canRun(*job)) continue;
            for (size_t i = 0; i < job->tasks.size(); ++i) {
                if (job->tasks[i].status != JobTask::Status::Pending) continue;
                startTask(worker.get(), job, static_cast<int>(i));
                started = true;
                break;
            }
            if (started) break;
        }
    }
    m_scheduling = false;
}

void RenderScheduler::startTask(Worker* worker, const RenderJobPtr& job, int taskIndex) {
    JobTask& task = job->tasks[static_cast<size_t>(taskIndex)];
    task.status = JobTask::Status::Running;
    task.worker = worker->name();
    task.progress = 0.0;
    task.state = "Starting";
    ++task.attempts;
    if (job->status == RenderJob::Status::Queued) job->status = RenderJob::Status::Running;

    const unsigned serial = m_nextSerial++;
    Running& running = m_running[worker];
    running.job = job;
    running.task = taskIndex;
    running.serial = serial;

    // Every report comes back through the event loop, on this thread: a renderer may call
    // from its own threads, and one that reports from inside start() mustn't re-enter here.
    QPointer<RenderScheduler> self(this);
    TaskCallbacks callbacks;
    callbacks.progress = [self, worker, serial](double fraction, const std::string& state) {
        QMetaObject::invokeMethod(self, [self, worker, serial, fraction, state]() {
            if (self) self->onProgress(worker, serial, fraction, state);
        }, Qt::QueuedConnection);
    };
    callbacks.finished = [self, worker, serial](std::map<std::string, FloatImage> images) {
        auto shared = std::make_shared<std::map<std::string, FloatImage>>(std::move(images));
        QMetaObject::invokeMethod(self, [self, worker, serial, shared]() {
            if (self) self->onFinished(worker, serial, std::move(*shared));
        }, Qt::QueuedConnection);
    };
    callbacks.failed = [self, worker, serial](const std::string& error) {
        QMetaObject::invokeMethod(self, [self, worker, serial, error]() {
            if (self) self->onFailed(worker, serial, error);
        }, Qt::QueuedConnection);
    };
    Q_EMIT jobChanged(QString::fromStdString(job->id));
    std::unique_ptr<RenderTaskHandle> handle = worker->start(*job, task, std::move(callbacks));
    if (Running* still = runningFor(worker, serial)) still->handle = std::move(handle);
}

RenderScheduler::Running* RenderScheduler::runningFor(Worker* worker, unsigned serial) {
    const auto found = m_running.find(worker);
    if (found == m_running.end() || found->second.serial != serial) return nullptr;
    return &found->second;
}

void RenderScheduler::onProgress(Worker* worker, unsigned serial, double fraction, const std::string& state) {
    Running* running = runningFor(worker, serial);
    if (!running) return;
    JobTask& task = running->job->tasks[static_cast<size_t>(running->task)];
    task.progress = fraction;
    task.state = state;
    Q_EMIT jobChanged(QString::fromStdString(running->job->id));
}

void RenderScheduler::onFinished(Worker* worker, unsigned serial, std::map<std::string, FloatImage> images) {
    Running* running = runningFor(worker, serial);
    if (!running) return;
    const RenderJobPtr job = running->job;
    const int taskIndex = running->task;
    m_running.erase(worker);  // deletes the handle: the renderer's done with it
    JobTask& task = job->tasks[static_cast<size_t>(taskIndex)];

    // Into its frame: the tile without its overlap.
    FrameBuffers& frame = m_frames[job->id][task.frameIndex];
    const int width = job->spec.resolution[0];
    const int height = job->spec.resolution[1];
    const PixelRect& region = task.task.region;
    const PixelRect& core = task.core;
    for (auto& [name, image] : images) {
        if (image.width != region.width || image.height != region.height) {
            USDCC_LOG_WARNING("usdcc.render", "{}'s AOV {} is {}×{}, not the task's {}×{}: left out", job->id, name,
                              image.width, image.height, region.width, region.height);
            continue;
        }
        FloatImage& whole = frame.aovs[name];
        if (whole.width != width || whole.height != height) whole = FloatImage(width, height);
        for (int y = 0; y < core.height; ++y) {
            const float* from = image.row(core.y - region.y + y) + static_cast<size_t>(core.x - region.x) * 4;
            float* to = whole.row(core.y + y) + static_cast<size_t>(core.x) * 4;
            std::memcpy(to, from, static_cast<size_t>(core.width) * 4 * sizeof(float));
        }
    }
    ++frame.tilesIn;
    task.status = JobTask::Status::Done;
    task.progress = 1.0;
    task.state = "Done";

    if (frame.tilesIn >= job->tilesPerFrame()) writeFrame(job, task.frameIndex);
    if (job->status == RenderJob::Status::Running &&
        std::all_of(job->tasks.begin(), job->tasks.end(), [](const JobTask& t) { return t.status == JobTask::Status::Done; })) {
        endJob(job, RenderJob::Status::Done);
    } else {
        Q_EMIT jobChanged(QString::fromStdString(job->id));
    }
    schedule();
}

void RenderScheduler::onFailed(Worker* worker, unsigned serial, const std::string& error) {
    Running* running = runningFor(worker, serial);
    if (!running) return;
    const RenderJobPtr job = running->job;
    const int taskIndex = running->task;
    m_running.erase(worker);
    JobTask& task = job->tasks[static_cast<size_t>(taskIndex)];
    task.error = error;
    USDCC_LOG_WARNING("usdcc.render", "{} task {} (frame {}) failed on {}, try {} of {}: {}", job->id, taskIndex,
                      task.task.frame, task.worker, task.attempts, kMaxAttempts, error);
    if (task.attempts < kMaxAttempts) {
        task.status = JobTask::Status::Pending;
        task.progress = 0.0;
        task.state = "Waiting to try again";
        Q_EMIT jobChanged(QString::fromStdString(job->id));
    } else {
        task.status = JobTask::Status::Failed;
        cancelRunning(job->id);
        for (JobTask& other : job->tasks) {
            if (other.status == JobTask::Status::Pending || other.status == JobTask::Status::Running) {
                other.status = JobTask::Status::Cancelled;
            }
        }
        m_frames.erase(job->id);
        endJob(job, RenderJob::Status::Failed, error);
    }
    schedule();
}

void RenderScheduler::writeFrame(const RenderJobPtr& job, int frameIndex) {
    auto& frames = m_frames[job->id];
    const auto found = frames.find(frameIndex);
    if (found == frames.end()) return;
    std::string error;
    const std::vector<std::string> files = writeFrameFiles(*job, frameIndex, found->second.aovs, &error);
    if (files.empty()) {
        USDCC_LOG_ERROR("usdcc.render", "{}: {}", job->id, error);
    } else {
        job->files.insert(job->files.end(), files.begin(), files.end());
        USDCC_LOG_INFO("usdcc.render", "{} frame {}: {}", job->id, frameText(job->frames[static_cast<size_t>(frameIndex)]), files.front());
    }
    // The preview: the first product's first AOV, else any.
    const FloatImage* color = nullptr;
    if (!job->spec.products.empty() && !job->spec.products.front().aovs.empty()) {
        const auto first = found->second.aovs.find(job->spec.products.front().aovs.front().name);
        if (first != found->second.aovs.end()) color = &first->second;
    }
    if (!color && !found->second.aovs.empty()) color = &found->second.aovs.begin()->second;
    if (color) Q_EMIT frameRendered(QString::fromStdString(job->id), frameIndex, toQImage(*color));
    frames.erase(found);
    if (files.empty()) {
        for (JobTask& task : job->tasks) {
            if (task.frameIndex == frameIndex) {
                task.status = JobTask::Status::Failed;
                task.error = error;
            }
        }
        cancelRunning(job->id);
        for (JobTask& other : job->tasks) {
            if (other.status == JobTask::Status::Pending || other.status == JobTask::Status::Running) {
                other.status = JobTask::Status::Cancelled;
            }
        }
        endJob(job, RenderJob::Status::Failed, error);
    }
}

void RenderScheduler::endJob(const RenderJobPtr& job, RenderJob::Status status, const std::string& error) {
    if (job->status == RenderJob::Status::Done || job->status == RenderJob::Status::Failed ||
        job->status == RenderJob::Status::Cancelled) {
        return;
    }
    job->status = status;
    job->error = error;
    job->finished = std::chrono::steady_clock::now();
    m_frames.erase(job->id);
    const double seconds = std::chrono::duration<double>(*job->finished - job->submitted).count();
    using usdcc::core::LogLevel;
    using usdcc::core::ToastKind;
    using usdcc::core::ToastRequest;
    if (status == RenderJob::Status::Done) {
        ToastRequest toast;
        toast.kind = ToastKind::Success;
        toast.title = "Render done";
        USDCC_LOG_TOAST_AS(LogLevel::Info, "usdcc.render", toast, "{}: {} frame(s) in {:.1f} s, in {}", job->title,
                           job->frames.size(), seconds, outputFolder(*job));
    } else if (status == RenderJob::Status::Failed) {
        ToastRequest toast = ToastRequest::sticky();
        toast.kind = ToastKind::Error;
        toast.title = "Render failed";
        USDCC_LOG_TOAST_AS(LogLevel::Error, "usdcc.render", toast, "{}: {}", job->title, error);
    } else {
        USDCC_LOG_INFO("usdcc.render", "{} cancelled after {:.1f} s", job->title, seconds);
    }
    Q_EMIT jobChanged(QString::fromStdString(job->id));
    Q_EMIT jobEnded(QString::fromStdString(job->id));
}

}  // namespace usdcc::render
