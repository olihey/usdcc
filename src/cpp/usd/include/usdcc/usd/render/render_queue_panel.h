#pragma once

// The Render Queue side panel (docs/FINAL_RENDER.md §5.2): the scheduler's jobs and their
// tasks, with who renders each and how far it is, and the newest finished frame through the
// view transform.

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

class RenderScheduler;

class USDCC_USD_UI_API RenderQueueSidePanel : public usdcc::ui::SidePanel {
    Q_OBJECT

public:
    explicit RenderQueueSidePanel(RenderScheduler* scheduler, QWidget* parent = nullptr);

    QTreeWidget* tree() const { return m_tree; }
    usdcc::usd::RenderImageView* preview() const { return m_preview; }

    // While a render runs, asks whether to stop it.
    bool canClose() override;

private:
    void rebuild();
    void updateItems();
    void showFrame(const QString& jobId, int frameIndex, const QImage& linear);
    void updateButtons();
    QString selectedJob() const;

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
};

}  // namespace usdcc::render
