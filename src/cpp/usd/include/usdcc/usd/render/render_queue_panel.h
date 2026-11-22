#pragma once

// The Render Queue side panel (docs/FINAL_RENDER.md §5.2): the scheduler's jobs and their
// tasks, with who renders each and how far it is, and below them a frame through the view
// transform: the newest one as it's written, or the one double-clicked, read from its file.
// Below the scheduler's jobs, each render queue server's jobs under its name
// (docs/RENDER_NODE.md §4.4), whose Cancel, Retry and Remove go to the queue. Jobs and
// frames have a context menu: their folder and files, a viewer, and Share to a chat room.

#include "usdcc/core/export.h"
#include "usdcc/ui/side_panel.h"

#include <QImage>
#include <QPointer>
#include <QString>
#include <QStringList>

class QCheckBox;
class QLabel;
class QMenu;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::peers {
class Room;
}

namespace usdcc::ui {
class PeersController;
}

namespace usdcc::usd {
class RenderImageView;
}

namespace usdcc::render {

class RenderQueueClient;
class RenderScheduler;
struct RenderJob;

// A frame's file as the preview shows it: an EXR's linear values, or a PNG/JPEG as it is.
struct USDCC_USD_UI_API LoadedFrame {
    QImage image;         // Format_RGBA32FPx4 when linear, else as read
    bool linear = false;  // goes through the view transform before it's shown
    QString error;
};
// Reads `path`, scaled to fit `maxSide` (0: as it is). Safe on any thread.
USDCC_USD_UI_API LoadedFrame loadFrameFile(const QString& path, int maxSide);
// `frame` as it looks: through the view transform when it's linear, 8 bits a channel.
USDCC_USD_UI_API QImage displayImage(const LoadedFrame& frame);
// Whether `path` is on a network drive or share, where others may open it too.
USDCC_USD_UI_API bool isOnSharedDrive(const QString& path);
// The Open in Viewer command (FinalRender/imageViewer) for `file`, `sequence` being its
// frames with #### for the number: the program, then its arguments. Empty without a viewer.
USDCC_USD_UI_API QStringList viewerCommand(const QString& setting, const QString& file, const QString& sequence);

class USDCC_USD_UI_API RenderQueueSidePanel : public usdcc::ui::SidePanel {
    Q_OBJECT

public:
    // `queues`: the render queue servers' jobs; null: RenderQueueClient::instance(), if any.
    explicit RenderQueueSidePanel(RenderScheduler* scheduler, QWidget* parent = nullptr, RenderQueueClient* queues = nullptr);

    QTreeWidget* tree() const { return m_tree; }
    // The render queue servers' jobs, when the client comes after the panel.
    void setQueueClient(RenderQueueClient* queues);
    // The chat rooms Share sends to; without it, no Share.
    void setPeers(usdcc::ui::PeersController* peers);
    usdcc::usd::RenderImageView* preview() const { return m_preview; }
    QLabel* previewLabel() const { return m_previewLabel; }

    // Shows frame `frameIndex` of a job from its file (as a double-click does), and stops
    // following the newest frame. The image comes once it's read.
    void showFrameFile(const QString& jobId, int frameIndex);
    // Whether new frames replace the one shown.
    bool followsNewest() const;
    void setFollowNewest(bool follow);
    // The context menu of `item`, filled (for tests; the tree's own opens it).
    void fillMenu(QMenu* menu, QTreeWidgetItem* item);

    // While a render runs, asks whether to stop it.
    bool canClose() override;

Q_SIGNALS:
    // A frame read from its file is shown (or couldn't be read: `error`).
    void frameFileShown(const QString& jobId, int frameIndex, const QString& error);

private:
    void rebuild();
    void updateItems();
    void showFrame(const QString& jobId, int frameIndex, const QImage& linear);
    void showImage(const QImage& display);
    void updateButtons();
    QString selectedJob() const;
    // The map key of the queue job picked, or empty.
    QString selectedQueueJob() const;
    void rebuildSoon();
    void onDoubleClicked(QTreeWidgetItem* item);
    void fillJobMenu(QMenu* menu, const QString& jobId);
    void fillQueueJobMenu(QMenu* menu, const QString& key);
    void fillFrameMenu(QMenu* menu, const QString& jobId, int frameIndex);
    void fillShareMenu(QMenu* menu, const QString& jobId, int frameIndex);
    void removeAndDeleteFiles(const QString& jobId);
    void openInViewer(const RenderJob& job, int frameIndex, const QString& file);
    // Asks what to send (the Share dialog), then sends it: to `to` in `room`, or everyone.
    void share(const QString& jobId, int frameIndex, usdcc::peers::Room* room, const QString& to);
    // The newest frame of a job that's written, or -1.
    int newestWrittenFrame(const RenderJob& job) const;

    RenderScheduler* m_scheduler;
    QTreeWidget* m_tree;
    usdcc::usd::RenderImageView* m_preview;
    QLabel* m_previewLabel;
    QCheckBox* m_follow;
    QPushButton* m_cancel;
    QPushButton* m_retry;
    QPushButton* m_remove;
    QPushButton* m_clear;
    QPushButton* m_open;
    QTimer* m_updateTimer;
    QTimer* m_rebuildTimer;
    RenderQueueClient* m_queues = nullptr;
    QPointer<usdcc::ui::PeersController> m_peers;
    int m_loadSerial = 0;  // the newest read asked for; older ones are dropped
};

}  // namespace usdcc::render
