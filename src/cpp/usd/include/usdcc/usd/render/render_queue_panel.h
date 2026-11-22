#pragma once

// The Render Queue side panel (docs/FINAL_RENDER.md §5.2): the scheduler's jobs and their
// tasks, with who renders each and how far it is, and the newest finished frame through the
// view transform. Below them, each render queue server's jobs under its name
// (docs/RENDER_NODE.md §4.4), whose Cancel, Retry and Remove go to the queue.

#include "usdcc/core/export.h"
#include "usdcc/ui/side_panel.h"

#include <QImage>
#include <QString>

class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::usd {
class RenderImageView;
}

namespace usdcc::render {

class RenderQueueClient;
class RenderScheduler;

class USDCC_USD_UI_API RenderQueueSidePanel : public usdcc::ui::SidePanel {
    Q_OBJECT

public:
    // `queues`: the render queue servers' jobs; null: RenderQueueClient::instance(), if any.
    explicit RenderQueueSidePanel(RenderScheduler* scheduler, QWidget* parent = nullptr, RenderQueueClient* queues = nullptr);

    QTreeWidget* tree() const { return m_tree; }
    // The render queue servers' jobs, when the client comes after the panel.
    void setQueueClient(RenderQueueClient* queues);
    usdcc::usd::RenderImageView* preview() const { return m_preview; }

    // While a render runs, asks whether to stop it.
    bool canClose() override;

private:
    void rebuild();
    void updateItems();
    void showFrame(const QString& jobId, int frameIndex, const QImage& linear);
    void updateButtons();
    QString selectedJob() const;
    // The map key of the queue job picked, or empty.
    QString selectedQueueJob() const;
    void rebuildSoon();

    RenderScheduler* m_scheduler;
    QTreeWidget* m_tree;
    usdcc::usd::RenderImageView* m_preview;
    QLabel* m_previewLabel;
    QPushButton* m_cancel;
    QPushButton* m_retry;
    QPushButton* m_remove;
    QPushButton* m_clear;
    QPushButton* m_open;
    QTimer* m_updateTimer;
    QTimer* m_rebuildTimer;
    RenderQueueClient* m_queues = nullptr;
};

}  // namespace usdcc::render
