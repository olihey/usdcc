#pragma once

// The render queue (docs/FINAL_RENDER.md §4): jobs in the order they came, and the workers
// that render their tasks, one task per worker at a time. A finished task's images go into
// its frame; once every tile of a frame is in, the frame is written to its files. A task that
// fails is tried again, twice, possibly by another worker.
//
// This instance is one worker (LocalWorker). Peers will be others (docs/FINAL_RENDER.md §7).

#include "usdcc/core/export.h"
#include "usdcc/usd/render/render_job.h"

#include <QImage>
#include <QObject>
#include <QString>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace usdcc::render {

// Something that renders tasks: this instance, or (later) a peer.
class USDCC_USD_UI_API Worker {
public:
    virtual ~Worker() = default;
    virtual std::string name() const = 0;
    // Whether it can render this job's tasks at all (it has the renderer, and the renderer can
    // render a tile when the job has tiles).
    virtual bool canRun(const RenderJob& job) const = 0;
    virtual std::unique_ptr<RenderTaskHandle> start(const RenderJob& job, const JobTask& task, TaskCallbacks callbacks) = 0;
};

// This instance, with the renderers of FinalRenderers.
class USDCC_USD_UI_API LocalWorker : public Worker {
public:
    std::string name() const override;
    bool canRun(const RenderJob& job) const override;
    std::unique_ptr<RenderTaskHandle> start(const RenderJob& job, const JobTask& task, TaskCallbacks callbacks) override;
};

class USDCC_USD_UI_API RenderScheduler : public QObject {
    Q_OBJECT

public:
    // Tries a task gets before it fails its job.
    static constexpr int kMaxAttempts = 3;

    // Without workers; the app's (instance()) has a LocalWorker.
    explicit RenderScheduler(QObject* parent = nullptr);
    ~RenderScheduler() override;

    static RenderScheduler& instance();

    void addWorker(std::shared_ptr<Worker> worker);
    void removeWorker(const std::shared_ptr<Worker>& worker);
    const std::vector<std::shared_ptr<Worker>>& workers() const { return m_workers; }

    // Queues a job; it starts when a worker is free.
    void submit(RenderJobPtr job);
    // Stops its running tasks; what's done stays written.
    void cancel(const std::string& jobId);
    // Queues its failed and cancelled tasks again.
    void retry(const std::string& jobId);
    // Takes a job that has ended off the list.
    void remove(const std::string& jobId);
    // Takes every job that has ended off the list.
    void clearEnded();

    const std::vector<RenderJobPtr>& jobs() const { return m_jobs; }
    RenderJobPtr job(const std::string& jobId) const;
    // Whether anything is queued or running.
    bool busy() const;

    // Where a job's frames go: the folder of its first product's first file.
    static std::string outputFolder(const RenderJob& job);

Q_SIGNALS:
    // A job was added or removed.
    void jobsChanged();
    // A job's status or a task's progress changed.
    void jobChanged(const QString& jobId);
    // A frame was put together and written: its color AOV, linear (Format_RGBA32FPx4).
    void frameRendered(const QString& jobId, int frameIndex, const QImage& color);
    // A job ended: done, failed or cancelled.
    void jobEnded(const QString& jobId);

private:
    struct Running {
        RenderJobPtr job;
        int task = -1;
        std::unique_ptr<RenderTaskHandle> handle;
        unsigned serial = 0;  // tells its callbacks from a cancelled one's
    };
    struct FrameBuffers {
        std::map<std::string, FloatImage> aovs;
        int tilesIn = 0;
    };

    void schedule();
    void startTask(Worker* worker, const RenderJobPtr& job, int taskIndex);
    void onProgress(Worker* worker, unsigned serial, double fraction, const std::string& state);
    void onFinished(Worker* worker, unsigned serial, std::map<std::string, FloatImage> images);
    void onFailed(Worker* worker, unsigned serial, const std::string& error);
    Running* runningFor(Worker* worker, unsigned serial);
    void writeFrame(const RenderJobPtr& job, int frameIndex);
    void endJob(const RenderJobPtr& job, RenderJob::Status status, const std::string& error = {});
    void cancelRunning(const std::string& jobId);

    std::vector<std::shared_ptr<Worker>> m_workers;
    std::vector<RenderJobPtr> m_jobs;
    std::map<Worker*, Running> m_running;
    // By job id, then frame index.
    std::map<std::string, std::map<int, FrameBuffers>> m_frames;
    unsigned m_nextSerial = 1;
    bool m_scheduling = false;
};

// Writes one frame of `job`: each product's file(s), from its AOVs' images (the whole frame
// each). Returns the files written, or an empty list with `error` set.
USDCC_USD_UI_API std::vector<std::string> writeFrameFiles(const RenderJob& job, int frameIndex,
                                                          const std::map<std::string, FloatImage>& aovs,
                                                          std::string* error);

// A float image as a QImage (Format_RGBA32FPx4), for the Render Queue's preview.
USDCC_USD_UI_API QImage toQImage(const FloatImage& image);

}  // namespace usdcc::render
