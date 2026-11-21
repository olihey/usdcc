#include "usdcc/usd/render/render_queue_panel.h"

#include "usdcc/usd/color_management.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render_view_panel.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <chrono>

namespace usdcc::render {

namespace {

constexpr int kUpdateMs = 200;
constexpr int kPreviewMaxSide = 1600;
constexpr int kJobRole = Qt::UserRole;
constexpr int kTaskRole = Qt::UserRole + 1;

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

QString taskName(const RenderJob& job, const JobTask& task) {
    QString name = QObject::tr("Frame %1").arg(QString::fromStdString(frameText(task.task.frame)));
    if (job.tilesPerFrame() > 1) {
        const int tile = task.task.index % job.tilesPerFrame();
        name += QObject::tr(" · tile %1/%2").arg(tile + 1).arg(job.tilesPerFrame());
    }
    return name;
}

}  // namespace

RenderQueueSidePanel::RenderQueueSidePanel(RenderScheduler* scheduler, QWidget* parent)
    : usdcc::ui::SidePanel(tr("Render Queue"), parent), m_scheduler(scheduler) {
    setObjectName(QStringLiteral("RenderQueue"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* split = new QSplitter(Qt::Vertical, content);
    m_tree = new QTreeWidget(split);
    m_tree->setObjectName(QStringLiteral("renderQueueTree"));
    m_tree->setHeaderLabels({tr("Job"), tr("Status"), tr("Progress"), tr("Worker"), tr("Time")});
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    for (int c = StatusColumn; c <= TimeColumn; ++c) m_tree->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);

    auto* previewBox = new QWidget(split);
    auto* previewLayout = new QVBoxLayout(previewBox);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    m_previewLabel = new QLabel(tr("The newest frame shows here."), previewBox);
    m_previewLabel->setObjectName(QStringLiteral("renderQueuePreviewLabel"));
    previewLayout->addWidget(m_previewLabel);
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
    connect(m_cancel, &QPushButton::clicked, this, [this]() { m_scheduler->cancel(selectedJob().toStdString()); });
    connect(m_retry, &QPushButton::clicked, this, [this]() { m_scheduler->retry(selectedJob().toStdString()); });
    connect(m_remove, &QPushButton::clicked, this, [this]() { m_scheduler->remove(selectedJob().toStdString()); });
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

QString RenderQueueSidePanel::selectedJob() const {
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    if (selected.isEmpty()) {
        // Nothing picked: the newest job.
        return m_tree->topLevelItemCount() ? m_tree->topLevelItem(m_tree->topLevelItemCount() - 1)->data(NameColumn, kJobRole).toString()
                                           : QString();
    }
    return selected.front()->data(NameColumn, kJobRole).toString();
}

void RenderQueueSidePanel::rebuild() {
    const QString selected = m_tree->selectedItems().isEmpty() ? QString() : m_tree->selectedItems().front()->data(NameColumn, kJobRole).toString();
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
        for (size_t t = 0; t < job->tasks.size(); ++t) {
            auto* child = new QTreeWidgetItem(item);
            child->setData(NameColumn, kJobRole, id);
            child->setData(NameColumn, kTaskRole, static_cast<int>(t));
            child->setText(NameColumn, taskName(*job, job->tasks[t]));
        }
        item->setExpanded(expanded.contains(id));
        if (id == selected) item->setSelected(true);
    }
    updateItems();
    updateButtons();
    if (m_scheduler->busy()) m_updateTimer->start();
}

void RenderQueueSidePanel::updateItems() {
    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        const RenderJobPtr job = m_scheduler->job(item->data(NameColumn, kJobRole).toString().toStdString());
        if (!job) continue;
        item->setText(StatusColumn, jobStatus(*job));
        item->setText(ProgressColumn, QStringLiteral("%1/%2 · %3").arg(job->doneTasks()).arg(job->tasks.size()).arg(percent(job->progress())));
        const double seconds = std::chrono::duration<double>(job->finished.value_or(now) - job->submitted).count();
        item->setText(TimeColumn, QStringLiteral("%1 s").arg(seconds, 0, 'f', 1));
        item->setToolTip(StatusColumn, QString::fromStdString(job->error));
        QSet<QString> workers;
        const bool active = job->status == RenderJob::Status::Running || job->status == RenderJob::Status::Queued;
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
        item->setText(WorkerColumn, active ? QStringList(workers.values()).join(QStringLiteral(", ")) : QString());
    }
}

void RenderQueueSidePanel::updateButtons() {
    const RenderJobPtr job = m_scheduler->job(selectedJob().toStdString());
    const bool active = job && (job->status == RenderJob::Status::Queued || job->status == RenderJob::Status::Running);
    m_cancel->setEnabled(active);
    m_retry->setEnabled(job && (job->status == RenderJob::Status::Failed || job->status == RenderJob::Status::Cancelled));
    m_remove->setEnabled(job != nullptr);
    m_open->setEnabled(job && !job->files.empty());
    m_clear->setEnabled(!m_scheduler->jobs().empty());
}

void RenderQueueSidePanel::showFrame(const QString& jobId, int frameIndex, const QImage& linear) {
    const RenderJobPtr job = m_scheduler->job(jobId.toStdString());
    if (job && frameIndex >= 0 && frameIndex < static_cast<int>(job->frames.size())) {
        m_previewLabel->setText(tr("%1, frame %2").arg(QString::fromStdString(job->title),
                                                       QString::fromStdString(frameText(job->frames[static_cast<size_t>(frameIndex)]))));
    }
    // Small enough to go through the view transform here, at once.
    QImage image = linear;
    if (image.width() > kPreviewMaxSide || image.height() > kPreviewMaxSide) {
        image = image.scaled(kPreviewMaxSide, kPreviewMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    image = image.convertToFormat(QImage::Format_RGBA32FPx4);
    if (const auto transform = usdcc::usd::ColorManagement::instance().imageTransform()) {
        for (int y = 0; y < image.height(); ++y) transform->apply(reinterpret_cast<float*>(image.scanLine(y)), static_cast<size_t>(image.width()));
    }
    m_preview->setMessage(QString());
    m_preview->setImage(image.convertToFormat(QImage::Format_RGBA8888));
    m_open->setEnabled(true);
}

}  // namespace usdcc::render
