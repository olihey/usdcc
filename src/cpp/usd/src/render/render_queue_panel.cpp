#include "usdcc/usd/render/render_queue_panel.h"

#include "usdcc/core/settings.h"
#include "usdcc/peers/room.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/usd/color_management.h"
#include "usdcc/usd/linear_exr.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/render/render_queue.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render_view_panel.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QStorageInfo>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <chrono>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace usdcc::render {

namespace {

constexpr int kUpdateMs = 200;
constexpr int kPreviewMaxSide = 1600;
constexpr int kSharePreviewMaxSide = 2048;
constexpr int kJobRole = Qt::UserRole;
constexpr int kTaskRole = Qt::UserRole + 1;
// A queue's job: its key in the queue's map.
constexpr int kQueueJobRole = Qt::UserRole + 2;

enum Column { NameColumn, StatusColumn, ProgressColumn, WorkerColumn, TimeColumn };

QString jobStatus(const RenderJob& job) {
    switch (job.status) {
    case RenderJob::Status::Queued: return QObject::tr("Queued");
    case RenderJob::Status::Running: return QObject::tr("Rendering");
    case RenderJob::Status::Done: return QObject::tr("Done");
    case RenderJob::Status::Failed: return QObject::tr("Failed");
    case RenderJob::Status::Cancelled: return QObject::tr("Cancelled");
    }
    return {};
}

QString taskStatus(const JobTask& task) {
    switch (task.status) {
    case JobTask::Status::Pending: return task.attempts > 0 ? QObject::tr("Waiting to try again") : QObject::tr("Waiting");
    case JobTask::Status::Running: return QString::fromStdString(task.state.empty() ? "Rendering" : task.state);
    case JobTask::Status::Done: return QObject::tr("Done");
    case JobTask::Status::Failed: return QObject::tr("Failed");
    case JobTask::Status::Cancelled: return QObject::tr("Cancelled");
    }
    return {};
}

QString percent(double fraction) {
    if (fraction < 0) return QStringLiteral("…");
    return QStringLiteral("%1%").arg(static_cast<int>(fraction * 100.0 + 0.5));
}

QString queueStatus(const QString& status) {
    if (status == u"running") return QObject::tr("Rendering");
    if (status == u"done") return QObject::tr("Done");
    if (status == u"failed") return QObject::tr("Failed");
    if (status == u"cancelled") return QObject::tr("Cancelled");
    return QObject::tr("Queued");
}

QString taskName(const RenderJob& job, const JobTask& task) {
    QString name = QObject::tr("Frame %1").arg(QString::fromStdString(frameText(task.task.frame)));
    if (job.tilesPerFrame() > 1) {
        const int tile = task.task.index % job.tilesPerFrame();
        name += QObject::tr(" · tile %1/%2").arg(tile + 1).arg(job.tilesPerFrame());
    }
    return name;
}

bool active(const RenderJob& job) {
    return job.status == RenderJob::Status::Queued || job.status == RenderJob::Status::Running;
}

QString sizeText(qint64 bytes) {
    if (bytes >= qint64(1024) * 1024 * 1024) return QObject::tr("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    if (bytes >= 1024 * 1024) return QObject::tr("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QObject::tr("%1 KB").arg(std::max<qint64>(1, bytes / 1024));
}

QString frameLabel(const RenderJob& job, int frameIndex) {
    return QObject::tr("%1, frame %2")
        .arg(QString::fromStdString(job.title), QString::fromStdString(frameText(job.frames[static_cast<size_t>(frameIndex)])));
}

// A frame's files that are there, in the order they're written.
QStringList existingFrameFiles(const RenderJob& job, int frameIndex) {
    QStringList files;
    for (const std::string& file : frameFiles(job, frameIndex)) {
        const QString path = QString::fromStdString(file);
        if (QFileInfo::exists(path)) files.append(path);
    }
    return files;
}

void copyText(const QString& text) {
    QApplication::clipboard()->setText(text);
}

}  // namespace

// --- Frames from their files

LoadedFrame loadFrameFile(const QString& path, int maxSide) {
    LoadedFrame result;
    if (QFileInfo(path).suffix().compare(QStringLiteral("exr"), Qt::CaseInsensitive) == 0) {
        std::vector<float> rgba;
        int width = 0;
        int height = 0;
        std::string error;
        if (!usdcc::usd::readLinearExr(QFileInfo(path).absoluteFilePath().toStdString(), rgba, width, height, &error)) {
            result.error = QString::fromStdString(error);
            return result;
        }
        QImage image(width, height, QImage::Format_RGBA32FPx4);
        if (image.isNull()) {
            result.error = QObject::tr("the image is too large");
            return result;
        }
        for (int y = 0; y < height; ++y) {
            std::memcpy(image.scanLine(y), rgba.data() + size_t(y) * size_t(width) * 4, size_t(width) * 4 * sizeof(float));
        }
        result.image = std::move(image);
        result.linear = true;
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        result.image = reader.read();
        if (result.image.isNull()) {
            result.error = reader.errorString();
            return result;
        }
    }
    if (maxSide > 0 && (result.image.width() > maxSide || result.image.height() > maxSide)) {
        result.image = result.image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return result;
}

QImage displayImage(const LoadedFrame& frame) {
    if (frame.image.isNull()) return {};
    if (!frame.linear) return frame.image.convertToFormat(QImage::Format_RGBA8888);
    QImage image = frame.image.convertToFormat(QImage::Format_RGBA32FPx4);
    if (const auto transform = usdcc::usd::ColorManagement::instance().imageTransform()) {
        for (int y = 0; y < image.height(); ++y) transform->apply(reinterpret_cast<float*>(image.scanLine(y)), static_cast<size_t>(image.width()));
    }
    return image.convertToFormat(QImage::Format_RGBA8888);
}

bool isOnSharedDrive(const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const QString native = QDir::toNativeSeparators(absolute);
    if (native.startsWith(QStringLiteral("\\\\")) || absolute.startsWith(QStringLiteral("//"))) return true;  // UNC
#ifdef _WIN32
    if (native.size() >= 2 && native[1] == u':') {
        const std::wstring root = (native.left(2) + QStringLiteral("\\")).toStdWString();
        return GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
    }
    return false;
#else
    static const QList<QByteArray> network = {"nfs", "nfs4", "cifs", "smbfs", "smb3", "fuse.sshfs", "afpfs", "webdav", "davfs"};
    const QStorageInfo storage(absolute);
    return storage.isValid() && network.contains(storage.fileSystemType());
#endif
}

QStringList viewerCommand(const QString& setting, const QString& file, const QString& sequence) {
    QStringList parts = QProcess::splitCommand(setting.trimmed());
    if (parts.isEmpty()) return {};
    bool placed = false;
    for (qsizetype i = 1; i < parts.size(); ++i) {
        if (parts[i].contains(QStringLiteral("{file}")) || parts[i].contains(QStringLiteral("{sequence}"))) {
            parts[i].replace(QStringLiteral("{file}"), QDir::toNativeSeparators(file));
            parts[i].replace(QStringLiteral("{sequence}"), QDir::toNativeSeparators(sequence));
            placed = true;
        }
    }
    if (!placed) parts.append(QDir::toNativeSeparators(file));
    return parts;
}

// --- The panel

RenderQueueSidePanel::RenderQueueSidePanel(RenderScheduler* scheduler, QWidget* parent, RenderQueueClient* queues)
    : usdcc::ui::SidePanel(tr("Render Queue"), parent), m_scheduler(scheduler),
      m_queues(queues ? queues : RenderQueueClient::instance()) {
    setObjectName(QStringLiteral("RenderQueue"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* split = new QSplitter(Qt::Vertical, content);
    m_tree = new QTreeWidget(split);
    m_tree->setObjectName(QStringLiteral("renderQueueTree"));
    m_tree->setHeaderLabels({tr("Job"), tr("Status"), tr("Progress"), tr("Worker"), tr("Time")});
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setExpandsOnDoubleClick(false);  // a double-click shows the frame
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    for (int c = StatusColumn; c <= TimeColumn; ++c) m_tree->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);

    auto* previewBox = new QWidget(split);
    auto* previewLayout = new QVBoxLayout(previewBox);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    auto* previewHeader = new QHBoxLayout();
    m_previewLabel = new QLabel(tr("The newest frame shows here; double-click a frame to see it."), previewBox);
    m_previewLabel->setObjectName(QStringLiteral("renderQueuePreviewLabel"));
    m_follow = new QCheckBox(tr("Follow newest"), previewBox);
    m_follow->setObjectName(QStringLiteral("renderQueueFollow"));
    m_follow->setToolTip(tr("Show each frame as it's written. A double-clicked frame stops it."));
    m_follow->setChecked(true);
    previewHeader->addWidget(m_previewLabel, 1);
    previewHeader->addWidget(m_follow);
    previewLayout->addLayout(previewHeader);
    m_preview = new usdcc::usd::RenderImageView(previewBox);
    m_preview->setObjectName(QStringLiteral("renderQueuePreview"));
    m_preview->setMinimumHeight(120);
    previewLayout->addWidget(m_preview, 1);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);

    auto* buttons = new QHBoxLayout();
    m_cancel = new QPushButton(tr("Cancel"), content);
    m_retry = new QPushButton(tr("Retry"), content);
    m_remove = new QPushButton(tr("Remove"), content);
    m_clear = new QPushButton(tr("Clear Ended"), content);
    m_open = new QPushButton(tr("Open Folder"), content);
    m_cancel->setObjectName(QStringLiteral("renderQueueCancel"));
    m_retry->setObjectName(QStringLiteral("renderQueueRetry"));
    m_retry->setToolTip(tr("Render the failed and cancelled frames again"));
    m_open->setToolTip(tr("Show the folder the job's images are in"));
    for (QPushButton* button : {m_cancel, m_retry, m_remove, m_clear, m_open}) buttons->addWidget(button);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    setWidget(content);

    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(50);
    connect(m_rebuildTimer, &QTimer::timeout, this, [this]() { rebuild(); });
    if (m_queues) connect(m_queues, &RenderQueueClient::jobsChanged, this, &RenderQueueSidePanel::rebuildSoon);

    m_updateTimer = new QTimer(this);
    m_updateTimer->setInterval(kUpdateMs);
    connect(m_updateTimer, &QTimer::timeout, this, [this]() {
        updateItems();
        if (!m_scheduler->busy()) m_updateTimer->stop();
    });

    connect(m_scheduler, &RenderScheduler::jobsChanged, this, [this]() { rebuild(); });
    connect(m_scheduler, &RenderScheduler::jobChanged, this, [this](const QString&) {
        if (!m_updateTimer->isActive()) {
            updateItems();
            m_updateTimer->start();
        }
    });
    connect(m_scheduler, &RenderScheduler::jobEnded, this, [this](const QString&) {
        updateItems();
        updateButtons();
    });
    connect(m_scheduler, &RenderScheduler::frameRendered, this, &RenderQueueSidePanel::showFrame);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this]() { updateButtons(); });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) { onDoubleClicked(item); });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        QTreeWidgetItem* item = m_tree->itemAt(position);
        if (!item) return;
        m_tree->setCurrentItem(item);
        QMenu menu(this);
        fillMenu(&menu, item);
        if (!menu.isEmpty()) menu.exec(m_tree->viewport()->mapToGlobal(position));
    });
    // A queue's job: the queue does it, if it lets this profile.
    connect(m_cancel, &QPushButton::clicked, this, [this]() {
        if (const QString key = selectedQueueJob(); !key.isEmpty()) return m_queues->cancel(key);
        m_scheduler->cancel(selectedJob().toStdString());
    });
    connect(m_retry, &QPushButton::clicked, this, [this]() {
        if (const QString key = selectedQueueJob(); !key.isEmpty()) return m_queues->retry(key);
        m_scheduler->retry(selectedJob().toStdString());
    });
    connect(m_remove, &QPushButton::clicked, this, [this]() {
        if (const QString key = selectedQueueJob(); !key.isEmpty()) return m_queues->remove(key);
        m_scheduler->remove(selectedJob().toStdString());
    });
    connect(m_clear, &QPushButton::clicked, this, [this]() { m_scheduler->clearEnded(); });
    connect(m_open, &QPushButton::clicked, this, [this]() {
        const RenderJobPtr job = m_scheduler->job(selectedJob().toStdString());
        if (!job) return;
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(RenderScheduler::outputFolder(*job))));
    });
    rebuild();
}

bool RenderQueueSidePanel::canClose() {
    if (!m_scheduler->busy()) return true;
    return QMessageBox::question(window(), tr("Render"), tr("A render is running. Stop it and quit?"),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}

void RenderQueueSidePanel::setQueueClient(RenderQueueClient* queues) {
    if (m_queues == queues) return;
    if (m_queues) disconnect(m_queues, nullptr, this, nullptr);
    m_queues = queues;
    if (m_queues) {
        connect(m_queues, &RenderQueueClient::jobsChanged, this, &RenderQueueSidePanel::rebuildSoon);
    }
    rebuild();
}

void RenderQueueSidePanel::setPeers(usdcc::ui::PeersController* peers) {
    m_peers = peers;
}

bool RenderQueueSidePanel::followsNewest() const {
    return m_follow->isChecked();
}

void RenderQueueSidePanel::setFollowNewest(bool follow) {
    m_follow->setChecked(follow);
}

void RenderQueueSidePanel::rebuildSoon() {
    if (!m_rebuildTimer->isActive()) m_rebuildTimer->start();
}

QString RenderQueueSidePanel::selectedQueueJob() const {
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    return selected.isEmpty() || !m_queues ? QString() : selected.front()->data(NameColumn, kQueueJobRole).toString();
}

QString RenderQueueSidePanel::selectedJob() const {
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    if (selected.isEmpty()) {
        // Nothing picked: the newest job.
        return m_scheduler->jobs().empty() ? QString() : QString::fromStdString(m_scheduler->jobs().back()->id);
    }
    return selected.front()->data(NameColumn, kJobRole).toString();
}

void RenderQueueSidePanel::rebuild() {
    const QString selected = m_tree->selectedItems().isEmpty() ? QString() : m_tree->selectedItems().front()->data(NameColumn, kJobRole).toString();
    const int selectedTask = m_tree->selectedItems().isEmpty() ? -1 : m_tree->selectedItems().front()->data(NameColumn, kTaskRole).toInt();
    const QString selectedQueue = selectedQueueJob();
    QSet<QString> expanded;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        if (m_tree->topLevelItem(i)->isExpanded()) expanded.insert(m_tree->topLevelItem(i)->data(NameColumn, kJobRole).toString());
    }
    m_tree->clear();
    for (const RenderJobPtr& job : m_scheduler->jobs()) {
        const QString id = QString::fromStdString(job->id);
        auto* item = new QTreeWidgetItem(m_tree);
        item->setData(NameColumn, kJobRole, id);
        item->setData(NameColumn, kTaskRole, -1);
        item->setText(NameColumn, QStringLiteral("%1 — %2").arg(QString::fromStdString(job->title), QString::fromStdString(job->rendererName)));
        item->setToolTip(NameColumn, QDir::toNativeSeparators(QString::fromStdString(RenderScheduler::outputFolder(*job))));
        for (size_t t = 0; t < job->tasks.size(); ++t) {
            auto* child = new QTreeWidgetItem(item);
            child->setData(NameColumn, kJobRole, id);
            child->setData(NameColumn, kTaskRole, static_cast<int>(t));
            child->setText(NameColumn, taskName(*job, job->tasks[t]));
            child->setToolTip(NameColumn, tr("Double-click to see it; right-click for its files"));
            if (id == selected && static_cast<int>(t) == selectedTask) child->setSelected(true);
        }
        item->setExpanded(expanded.contains(id));
        if (id == selected && selectedTask < 0) item->setSelected(true);
    }
    // The queues' jobs, under each queue's name: they're the queue's, not this instance's.
    if (m_queues) {
        std::map<QString, QTreeWidgetItem*> headings;
        for (const QueuedJobInfo& info : m_queues->jobs()) {
            QTreeWidgetItem*& heading = headings[info.queueName];
            if (!heading) {
                heading = new QTreeWidgetItem(m_tree);
                heading->setText(NameColumn, tr("On %1").arg(info.queueName));
                heading->setData(NameColumn, kJobRole, QStringLiteral("queue:") + info.queueName);
                heading->setFlags(Qt::ItemIsEnabled);
                QFont font = heading->font(NameColumn);
                font.setBold(true);
                heading->setFont(NameColumn, font);
            }
            auto* child = new QTreeWidgetItem(heading);
            child->setData(NameColumn, kJobRole, QString());
            child->setData(NameColumn, kTaskRole, -1);
            child->setData(NameColumn, kQueueJobRole, info.key);
            child->setText(NameColumn, QStringLiteral("%1 — %2").arg(info.title, info.renderer));
            child->setToolTip(NameColumn, tr("%1's job, on %2").arg(info.submitter, info.queueName));
            if (info.key == selectedQueue) child->setSelected(true);
        }
        for (auto& [name, heading] : headings) heading->setExpanded(true);
    }
    updateItems();
    updateButtons();
    if (m_scheduler->busy()) m_updateTimer->start();
}

void RenderQueueSidePanel::updateItems() {
    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        // A queue's heading: its jobs as the queue says.
        if (m_queues && item->data(NameColumn, kJobRole).toString().startsWith(QStringLiteral("queue:"))) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            for (int c = 0; c < item->childCount(); ++c) {
                QTreeWidgetItem* child = item->child(c);
                const std::optional<QueuedJobInfo> info = m_queues->job(child->data(NameColumn, kQueueJobRole).toString());
                if (!info) continue;
                child->setText(StatusColumn, queueStatus(info->status));
                child->setToolTip(StatusColumn, info->error);
                child->setText(ProgressColumn, QStringLiteral("%1/%2 · %3").arg(info->done).arg(info->total).arg(percent(info->progress)));
                child->setText(WorkerColumn, info->nodes.join(QStringLiteral(", ")));
                const qint64 end = info->finishedMs ? info->finishedMs : nowMs;
                child->setText(TimeColumn, info->submittedMs ? QStringLiteral("%1 s").arg((end - info->submittedMs) / 1000.0, 0, 'f', 1)
                                                             : QString());
            }
            continue;
        }
        const RenderJobPtr job = m_scheduler->job(item->data(NameColumn, kJobRole).toString().toStdString());
        if (!job) continue;
        item->setText(StatusColumn, jobStatus(*job));
        item->setText(ProgressColumn, QStringLiteral("%1/%2 · %3").arg(job->doneTasks()).arg(job->tasks.size()).arg(percent(job->progress())));
        const double seconds = std::chrono::duration<double>(job->finished.value_or(now) - job->submitted).count();
        item->setText(TimeColumn, QStringLiteral("%1 s").arg(seconds, 0, 'f', 1));
        item->setToolTip(StatusColumn, QString::fromStdString(job->error));
        QSet<QString> workers;
        for (int c = 0; c < item->childCount(); ++c) {
            QTreeWidgetItem* child = item->child(c);
            const int index = child->data(NameColumn, kTaskRole).toInt();
            if (index < 0 || index >= static_cast<int>(job->tasks.size())) continue;
            const JobTask& task = job->tasks[static_cast<size_t>(index)];
            child->setText(StatusColumn, taskStatus(task));
            child->setToolTip(StatusColumn, QString::fromStdString(task.error));
            child->setText(ProgressColumn, task.status == JobTask::Status::Done      ? QStringLiteral("100%")
                                     : task.status == JobTask::Status::Running ? percent(task.progress)
                                                                               : QString());
            child->setText(WorkerColumn, QString::fromStdString(task.worker));
            if (task.status == JobTask::Status::Running) workers.insert(QString::fromStdString(task.worker));
        }
        item->setText(WorkerColumn, active(*job) ? QStringList(workers.values()).join(QStringLiteral(", ")) : QString());
    }
}

void RenderQueueSidePanel::updateButtons() {
    if (const QString key = selectedQueueJob(); !key.isEmpty()) {
        const std::optional<QueuedJobInfo> info = m_queues->job(key);
        const bool activeJob = info && (info->status == u"queued" || info->status == u"running");
        m_cancel->setEnabled(activeJob);
        m_retry->setEnabled(info && (info->status == u"failed" || info->status == u"cancelled"));
        m_remove->setEnabled(info.has_value());
        m_open->setEnabled(false);
        m_clear->setEnabled(!m_scheduler->jobs().empty());
        return;
    }
    const RenderJobPtr job = m_scheduler->job(selectedJob().toStdString());
    m_cancel->setEnabled(job && active(*job));
    m_retry->setEnabled(job && (job->status == RenderJob::Status::Failed || job->status == RenderJob::Status::Cancelled));
    m_remove->setEnabled(job != nullptr);
    m_open->setEnabled(job && !job->files.empty());
    m_clear->setEnabled(!m_scheduler->jobs().empty());
}

void RenderQueueSidePanel::showImage(const QImage& display) {
    m_preview->setMessage(QString());
    m_preview->setImage(display);
}

void RenderQueueSidePanel::showFrame(const QString& jobId, int frameIndex, const QImage& linear) {
    m_open->setEnabled(true);
    // A frame picked by hand stays until Follow newest is on again.
    if (!m_follow->isChecked()) return;
    const RenderJobPtr job = m_scheduler->job(jobId.toStdString());
    if (job && frameIndex >= 0 && frameIndex < static_cast<int>(job->frames.size())) {
        m_previewLabel->setText(frameLabel(*job, frameIndex));
    }
    // Small enough to go through the view transform here, at once.
    LoadedFrame frame{linear, true, {}};
    if (frame.image.width() > kPreviewMaxSide || frame.image.height() > kPreviewMaxSide) {
        frame.image = frame.image.scaled(kPreviewMaxSide, kPreviewMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    showImage(displayImage(frame));
}

int RenderQueueSidePanel::newestWrittenFrame(const RenderJob& job) const {
    for (int i = static_cast<int>(job.frames.size()) - 1; i >= 0; --i) {
        const std::vector<std::string> files = frameFiles(job, i);
        if (!files.empty() && QFileInfo::exists(QString::fromStdString(files.front()))) return i;
    }
    return -1;
}

void RenderQueueSidePanel::onDoubleClicked(QTreeWidgetItem* item) {
    if (!item || !item->data(NameColumn, kQueueJobRole).toString().isEmpty()) return;
    const QString jobId = item->data(NameColumn, kJobRole).toString();
    const RenderJobPtr job = m_scheduler->job(jobId.toStdString());
    if (!job) return;
    const int task = item->data(NameColumn, kTaskRole).toInt();
    if (task >= 0 && task < static_cast<int>(job->tasks.size())) {
        showFrameFile(jobId, job->tasks[static_cast<size_t>(task)].frameIndex);
        return;
    }
    // A job: its newest frame that's written.
    const int newest = newestWrittenFrame(*job);
    if (newest >= 0) {
        showFrameFile(jobId, newest);
    } else {
        item->setExpanded(!item->isExpanded());
    }
}

void RenderQueueSidePanel::showFrameFile(const QString& jobId, int frameIndex) {
    const RenderJobPtr job = m_scheduler->job(jobId.toStdString());
    if (!job || frameIndex < 0 || frameIndex >= static_cast<int>(job->frames.size())) return;
    setFollowNewest(false);
    const QString label = frameLabel(*job, frameIndex);
    const QStringList files = existingFrameFiles(*job, frameIndex);
    const int serial = ++m_loadSerial;
    if (files.isEmpty()) {
        const QString error = tr("Frame %1 isn't written yet").arg(QString::fromStdString(frameText(job->frames[static_cast<size_t>(frameIndex)])));
        m_previewLabel->setText(label);
        m_preview->setImage(QImage());
        m_preview->setMessage(error);
        Q_EMIT frameFileShown(jobId, frameIndex, error);
        return;
    }
    m_previewLabel->setText(tr("%1 · reading…").arg(label));
    const QString path = files.front();
    // Read on another thread (a large EXR takes a while); only the newest read is shown.
    QPointer<RenderQueueSidePanel> guard(this);
    QThreadPool::globalInstance()->start([guard, serial, path, jobId, frameIndex, label]() {
        LoadedFrame frame = loadFrameFile(path, kPreviewMaxSide);
        QMetaObject::invokeMethod(
            qApp,
            [guard, serial, frame = std::move(frame), path, jobId, frameIndex, label]() {
                if (!guard || serial != guard->m_loadSerial) return;
                guard->m_previewLabel->setText(label);
                guard->m_previewLabel->setToolTip(QDir::toNativeSeparators(path));
                if (frame.image.isNull()) {
                    const QString error = tr("Couldn't read %1: %2").arg(QFileInfo(path).fileName(), frame.error);
                    guard->m_preview->setImage(QImage());
                    guard->m_preview->setMessage(error);
                    Q_EMIT guard->frameFileShown(jobId, frameIndex, error);
                    return;
                }
                guard->showImage(displayImage(frame));
                Q_EMIT guard->frameFileShown(jobId, frameIndex, QString());
            },
            Qt::QueuedConnection);
    });
}

// --- Context menus

void RenderQueueSidePanel::fillMenu(QMenu* menu, QTreeWidgetItem* item) {
    if (!item) return;
    if (const QString key = item->data(NameColumn, kQueueJobRole).toString(); !key.isEmpty()) {
        fillQueueJobMenu(menu, key);
        return;
    }
    const QString jobId = item->data(NameColumn, kJobRole).toString();
    const RenderJobPtr job = m_scheduler->job(jobId.toStdString());
    if (!job) return;  // a queue's heading
    const int task = item->data(NameColumn, kTaskRole).toInt();
    if (task >= 0 && task < static_cast<int>(job->tasks.size())) {
        fillFrameMenu(menu, jobId, job->tasks[static_cast<size_t>(task)].frameIndex);
    } else {
        fillJobMenu(menu, jobId);
    }
}

void RenderQueueSidePanel::fillJobMenu(QMenu* menu, const QString& jobId) {
    const RenderJobPtr job = m_scheduler->job(jobId.toStdString());
    if (!job) return;
    const QString folder = QString::fromStdString(RenderScheduler::outputFolder(*job));
    const int newest = newestWrittenFrame(*job);

    QAction* show = menu->addAction(tr("Show Newest Frame"), this, [this, jobId, newest]() { showFrameFile(jobId, newest); });
    show->setEnabled(newest >= 0);
    menu->addSeparator();
    QAction* open = menu->addAction(tr("Open in Explorer"), this,
                                    [folder]() { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
    open->setEnabled(!folder.isEmpty() && QFileInfo(folder).isDir());
    open->setToolTip(QDir::toNativeSeparators(folder));
    menu->addAction(tr("Copy Folder Path"), this, [folder]() { copyText(QDir::toNativeSeparators(folder)); })
        ->setEnabled(!folder.isEmpty());
    menu->addSeparator();
    menu->addAction(tr("Cancel"), this, [this, jobId]() { m_scheduler->cancel(jobId.toStdString()); })->setEnabled(active(*job));
    menu->addAction(tr("Retry"), this, [this, jobId]() { m_scheduler->retry(jobId.toStdString()); })
        ->setEnabled(job->status == RenderJob::Status::Failed || job->status == RenderJob::Status::Cancelled);
    menu->addAction(tr("Remove"), this, [this, jobId]() { m_scheduler->remove(jobId.toStdString()); })->setEnabled(!active(*job));
    menu->addAction(tr("Remove and Delete Files…"), this, [this, jobId]() { removeAndDeleteFiles(jobId); })
        ->setEnabled(!active(*job) && !job->files.empty());
    menu->addSeparator();
    menu->addAction(tr("Clear Ended"), this, [this]() { m_scheduler->clearEnded(); });
}

void RenderQueueSidePanel::fillQueueJobMenu(QMenu* menu, const QString& key) {
    if (!m_queues) return;
    const std::optional<QueuedJobInfo> info = m_queues->job(key);
    if (!info) return;
    const bool running = info->status == u"queued" || info->status == u"running" || info->status == u"receiving";
    const QString output = info->output;
    QAction* open = menu->addAction(tr("Open Output Folder"), this,
                                    [output]() { QDesktopServices::openUrl(QUrl::fromLocalFile(output)); });
    // The queue's folder: open only when this machine reaches it (a shared output root).
    open->setEnabled(!output.isEmpty() && QFileInfo(output).isDir());
    open->setToolTip(QDir::toNativeSeparators(output));
    menu->addAction(tr("Copy Output Path"), this, [output]() { copyText(QDir::toNativeSeparators(output)); })
        ->setEnabled(!output.isEmpty());
    menu->addSeparator();
    menu->addAction(tr("Cancel"), this, [this, key]() { m_queues->cancel(key); })->setEnabled(running);
    menu->addAction(tr("Retry"), this, [this, key]() { m_queues->retry(key); })
        ->setEnabled(info->status == u"failed" || info->status == u"cancelled");
    menu->addAction(tr("Remove"), this, [this, key]() { m_queues->remove(key); });
}

void RenderQueueSidePanel::fillFrameMenu(QMenu* menu, const QString& jobId, int frameIndex) {
    const RenderJobPtr job = m_scheduler->job(jobId.toStdString());
    if (!job || frameIndex < 0 || frameIndex >= static_cast<int>(job->frames.size())) return;
    const QStringList files = existingFrameFiles(*job, frameIndex);
    const bool written = !files.isEmpty();
    const QString main = written ? files.front() : QString();

    menu->addAction(tr("Show in Preview"), this, [this, jobId, frameIndex]() { showFrameFile(jobId, frameIndex); })
        ->setEnabled(written);
    menu->addSeparator();
    menu->addAction(tr("Open in Explorer"), this, [main]() { usdcc::usd::revealFile(main); })->setEnabled(written);

    // With several files (products, PNG AOVs), each by its name.
    const auto perFile = [this, menu, &files, written](const QString& title, const std::function<void(const QString&)>& run) {
        if (files.size() <= 1) {
            const QString file = written ? files.front() : QString();
            menu->addAction(title, this, [run, file]() { run(file); })->setEnabled(written);
            return;
        }
        QMenu* sub = menu->addMenu(title);
        for (const QString& file : files) {
            sub->addAction(QFileInfo(file).fileName(), this, [run, file]() { run(file); });
        }
    };
    perFile(tr("Open in Default App"), [](const QString& file) { QDesktopServices::openUrl(QUrl::fromLocalFile(file)); });
    const QString viewer = QString::fromStdString(usdcc::core::Settings::instance().get<std::string>("FinalRender/imageViewer"));
    if (!viewer.trimmed().isEmpty()) {
        const QString program = QFileInfo(QProcess::splitCommand(viewer).value(0)).completeBaseName();
        RenderJobPtr held = job;
        perFile(tr("Open in %1").arg(program.isEmpty() ? tr("Viewer") : program),
                [this, held, frameIndex](const QString& file) { openInViewer(*held, frameIndex, file); });
    } else {
        QAction* none = menu->addAction(tr("Open in Viewer"));
        none->setEnabled(false);
        none->setToolTip(tr("Set Settings > Final Render > Image viewer first"));
    }
    perFile(tr("Copy Path"), [](const QString& file) { copyText(QDir::toNativeSeparators(file)); });
    menu->addAction(tr("Copy Image"), this, [main]() {
          const QImage image = displayImage(loadFrameFile(main, 0));
          if (!image.isNull()) QApplication::clipboard()->setImage(image);
      })->setEnabled(written);
    menu->addSeparator();
    QMenu* share = menu->addMenu(tr("Share"));
    share->setObjectName(QStringLiteral("renderQueueShareMenu"));
    if (!written) {
        share->setEnabled(false);
    } else {
        fillShareMenu(share, jobId, frameIndex);
    }
}

void RenderQueueSidePanel::fillShareMenu(QMenu* menu, const QString& jobId, int frameIndex) {
    using usdcc::peers::Peer;
    using usdcc::peers::Room;
    QList<Room*> rooms;
    if (m_peers) {
        for (Room* room : m_peers->chatRooms()) {
            if (room->state() == Room::State::Open) rooms.append(room);
        }
    }
    if (rooms.isEmpty()) {
        QAction* none = menu->addAction(tr("Join a chat room in the Peers panel to share"));
        none->setEnabled(false);
        return;
    }
    for (Room* room : rooms) {
        QMenu* sub = rooms.size() == 1 ? menu : menu->addMenu(room->name());
        if (rooms.size() == 1) sub->addSection(room->name());
        QPointer<Room> guard(room);
        sub->addAction(tr("Everyone in %1…").arg(room->name()), this, [this, jobId, frameIndex, guard]() {
            if (guard) share(jobId, frameIndex, guard, QString());
        });
        bool first = true;
        for (const Peer& peer : room->peers()) {
            if (peer.id == room->me().id || peer.kind == Peer::Kind::Guest) continue;
            if (first) sub->addSeparator();
            first = false;
            const QString to = peer.id;
            sub->addAction(tr("%1…").arg(peer.name), this, [this, jobId, frameIndex, guard, to]() {
                if (guard) share(jobId, frameIndex, guard, to);
            });
        }
    }
}

void RenderQueueSidePanel::openInViewer(const RenderJob& job, int frameIndex, const QString& file) {
    // The sequence: the file with its frame number as ####, when the job has several frames.
    QString sequence = file;
    if (job.frames.size() > 1) {
        const QString number = QString::fromStdString(frameText(job.frames[static_cast<size_t>(frameIndex)]));
        const QFileInfo info(file);
        QString name = info.fileName();
        const qsizetype at = name.lastIndexOf(number);
        if (at >= 0) {
            name.replace(at, number.size(), QStringLiteral("####"));
            sequence = info.dir().filePath(name);
        }
    }
    const QString setting = QString::fromStdString(usdcc::core::Settings::instance().get<std::string>("FinalRender/imageViewer"));
    const QStringList command = viewerCommand(setting, file, sequence);
    if (command.isEmpty()) return;
    if (!QProcess::startDetached(command.front(), command.mid(1))) {
        QMessageBox::warning(this, tr("Open in Viewer"),
                             tr("Couldn't start %1. Check Settings > Final Render > Image viewer.").arg(command.front()));
    }
}

void RenderQueueSidePanel::removeAndDeleteFiles(const QString& jobId) {
    const RenderJobPtr job = m_scheduler->job(jobId.toStdString());
    if (!job || active(*job)) return;
    const QString folder = QString::fromStdString(RenderScheduler::outputFolder(*job));
    QStringList files;
    for (const std::string& file : job->files) {
        if (QFileInfo::exists(QString::fromStdString(file))) files.append(QString::fromStdString(file));
    }
    const auto answer = QMessageBox::question(
        this, tr("Remove and Delete Files"),
        tr("Move the %n image file(s) of %1 to the recycle bin, and take the job off the list?\n\n%2", nullptr, int(files.size()))
            .arg(QString::fromStdString(job->title), QDir::toNativeSeparators(folder)),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    QStringList failed;
    for (const QString& file : files) {
        if (!QFile::moveToTrash(file)) failed.append(QFileInfo(file).fileName());
    }
    // Its own folder goes too, once it's empty.
    if (!job->folderName.empty() && QFileInfo(folder).fileName() == QString::fromStdString(job->folderName)) {
        QDir dir(folder);
        if (dir.exists() && dir.isEmpty()) dir.rmdir(folder);
    }
    m_scheduler->remove(jobId.toStdString());
    if (!failed.isEmpty()) {
        QMessageBox::warning(this, tr("Remove and Delete Files"),
                             tr("These couldn't be moved to the recycle bin:\n%1").arg(failed.join(u'\n')));
    }
}

void RenderQueueSidePanel::share(const QString& jobId, int frameIndex, usdcc::peers::Room* room, const QString& to) {
    using usdcc::peers::Peer;
    const RenderJobPtr job = m_scheduler->job(jobId.toStdString());
    if (!job || !room || !m_peers) return;
    const QStringList files = existingFrameFiles(*job, frameIndex);
    if (files.isEmpty()) return;
    const QString file = files.front();
    const qint64 bytes = QFileInfo(file).size();
    int people = 1;
    QString whom = room->peer(to).name;
    if (to.isEmpty()) {
        people = 0;
        for (const Peer& peer : room->peers()) {
            if (peer.id != room->me().id && peer.kind != Peer::Kind::Guest) ++people;
        }
        whom = tr("Everyone in %1").arg(room->name());
    }
    const bool shared = isOnSharedDrive(file);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Share Frame"));
    auto* form = new QFormLayout(&dialog);
    form->addRow(tr("To:"), new QLabel(whom.toHtmlEscaped(), &dialog));
    auto* caption = new QLineEdit(frameLabel(*job, frameIndex), &dialog);
    caption->setObjectName(QStringLiteral("shareCaption"));
    form->addRow(tr("Message:"), caption);
    form->addRow(QString(), new QLabel(tr("A preview goes with it (JPEG, at most %1 px).").arg(kSharePreviewMaxSide), &dialog));
    auto* original = new QCheckBox(tr("Include the original file, %1 (%2)").arg(QFileInfo(file).fileName(), sizeText(bytes)), &dialog);
    original->setObjectName(QStringLiteral("shareOriginal"));
    form->addRow(QString(), original);
    auto* note = new QLabel(&dialog);
    note->setWordWrap(true);
    form->addRow(QString(), note);
    const auto updateNote = [&]() {
        QStringList lines;
        if (original->isChecked()) {
            lines << (people > 1 ? tr("Each of the %1 people gets it from you when they ask: up to %2 in all.")
                                       .arg(people)
                                       .arg(sizeText(bytes * people))
                                 : tr("They get it from you when they ask for it; it stays offered while usdcc runs."));
        }
        if (shared) lines << tr("It's on a shared drive: its path goes with it, for whoever reaches that drive.");
        note->setText(lines.join(u'\n'));
        note->setVisible(!lines.isEmpty());
    };
    connect(original, &QCheckBox::toggled, &dialog, updateNote);
    updateNote();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    buttons->addButton(tr("Share"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    QPointer<usdcc::peers::Room> guard(room);
    if (dialog.exec() != QDialog::Accepted || !guard || !m_peers) return;

    const LoadedFrame frame = loadFrameFile(file, kSharePreviewMaxSide);
    if (frame.image.isNull()) {
        QMessageBox::warning(this, tr("Share Frame"), tr("Couldn't read %1: %2").arg(QFileInfo(file).fileName(), frame.error));
        return;
    }
    usdcc::ui::PeersController::ShareRequest request;
    request.room = guard;
    request.to = to;
    request.caption = caption->text();
    request.preview = displayImage(frame);
    request.previewName = QFileInfo(file).completeBaseName() + QStringLiteral(".jpg");
    if (original->isChecked()) request.original = file;
    if (shared) request.sharedPath = QFileInfo(file).absoluteFilePath();
    QString error;
    if (!m_peers->shareImage(request, &error)) QMessageBox::warning(this, tr("Share Frame"), error);
}

}  // namespace usdcc::render
