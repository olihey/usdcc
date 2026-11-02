#include "usdcc/usd/stage_opener.h"

#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/viewport_view_panel.h"

#include "usdcc/core/log.h"

#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStringList>
#include <QThread>

#include <climits>
#include <exception>
#include <memory>
#include <string>

namespace usdcc::usd {

namespace {
// How long an open runs before the progress dialog appears.
constexpr int kShowDialogAfterMs = 300;
// If loading alone took this long, the stage is big enough that building its
// viewport will stall the UI too, so "Preparing viewport..." is shown at once.
// (A stall can't reveal the dialog itself: its show timer needs the event loop.)
constexpr int kShowPreparingAfterMs = 100;
// Safety net: a viewport that is never drawn (e.g. the window is minimized)
// must not leave the dialog up forever.
constexpr int kFirstFrameTimeoutMs = 10000;
}  // namespace

StageOpener::StageOpener(StageManager* stageManager, QWidget* dialogParent)
    : QObject(dialogParent), m_stageManager(stageManager), m_dialogParent(dialogParent) {
    m_showTimer.setSingleShot(true);
    m_showTimer.setInterval(kShowDialogAfterMs);
    connect(&m_showTimer, &QTimer::timeout, this, &StageOpener::showDialog);

    m_firstFrameTimeout.setSingleShot(true);
    m_firstFrameTimeout.setInterval(kFirstFrameTimeoutMs);
    connect(&m_firstFrameTimeout, &QTimer::timeout, this, &StageOpener::finish);
}

StageOpener::~StageOpener() {
    // The worker only calls StageManager::loadUsdStage() (static), so letting it
    // run to completion is all that is needed for a clean shutdown.
    if (m_thread) {
        m_thread->wait();
    }
}

void StageOpener::open(const QString& path) {
    if (m_busy) {
        return;
    }
    setBusy(true);
    m_path = path;
    m_elapsed.start();

    m_dialog = new QProgressDialog(tr("Opening %1...").arg(QFileInfo(path).fileName()), QString(), 0, 0,
                                    m_dialogParent);
    m_dialog->setWindowTitle(tr("Open USD Stage"));
    m_dialog->setWindowModality(Qt::WindowModal);
    m_dialog->setCancelButton(nullptr);
    // No close button either: closing the dialog would only hide it, leaving
    // the UI interactive in the middle of the open.
    m_dialog->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    m_dialog->setAutoClose(false);
    m_dialog->setAutoReset(false);
    // QProgressDialog has its own show-after-a-delay timer; ours is the one in
    // charge (it also has to be forceable), so stop Qt's from firing first.
    m_dialog->setMinimumDuration(INT_MAX);
    m_showTimer.start();

    // QThread::create() rather than QtConcurrent: no extra Qt module needed.
    // The result travels in a shared_ptr the lambda keeps alive.
    struct Result {
        PXR_NS::UsdStageRefPtr stage;
        std::vector<std::string> errors;
    };
    auto result = std::make_shared<Result>();
    const std::string identifier = path.toStdString();
    m_thread = QThread::create([result, identifier]() {
        usdcc::core::setThreadName("stage-loader");
        try {
            result->stage = StageManager::loadUsdStage(identifier, &result->errors);
        } catch (const std::exception& e) {
            USDCC_LOG_ERROR("usdcc.stage", "Opening {} threw an exception: {}", identifier, e.what());
            result->errors.push_back(e.what());
            result->stage.Reset();
        } catch (...) {
            USDCC_LOG_ERROR("usdcc.stage", "Opening {} threw an exception", identifier);
            result->stage.Reset();
        }
    });
    m_thread->setParent(this);
    connect(m_thread, &QThread::finished, this, [this, result]() {
        m_thread->deleteLater();
        m_thread = nullptr;
        onLoaded(result->stage, result->errors);
    });
    m_thread->start();
}

void StageOpener::onLoaded(const PXR_NS::UsdStageRefPtr& usdStage, const std::vector<std::string>& errors) {
    if (!usdStage) {
        const QString path = m_path;
        finish();
        // The first error says why; all of them (also in the log) under Show Details.
        QMessageBox box(QMessageBox::Warning, tr("Open USD Stage"), tr("Could not open %1.").arg(path),
                        QMessageBox::Ok, m_dialogParent);
        if (!errors.empty()) {
            box.setInformativeText(QString::fromStdString(errors.front()));
        }
        if (errors.size() > 1) {
            QStringList all;
            for (const std::string& error : errors) all << QString::fromStdString(error);
            box.setDetailedText(all.join(QStringLiteral("\n\n")));
        }
        QPushButton* showLog = box.addButton(tr("Show Log"), QMessageBox::ActionRole);
        box.exec();
        if (box.clickedButton() == showLog) {
            Q_EMIT showLogRequested();
        }
        return;
    }

    USDCC_LOG_DEBUG("usdcc.stage", "Loaded {} in {} ms", m_path.toStdString(), m_elapsed.elapsed());

    // From here on the UI thread is going to be busy (registering the stage,
    // building its panels, then the viewport's first frame), so make sure the
    // new label is actually on screen before it starts.
    m_dialog->setLabelText(tr("Preparing viewport..."));
    if (m_elapsed.elapsed() >= kShowPreparingAfterMs) {
        showDialog();
    }
    if (m_dialog->isVisible()) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    const StageRefPtr stage = m_stageManager->adoptStage(usdStage);
    waitForFirstFrame(stage);
}

void StageOpener::waitForFirstFrame(const StageRefPtr& stage) {
    // adoptStage() fired stageOpened, which has given the stage a viewport by
    // now (see main.cpp). A new panel hasn't drawn yet: that happens once its
    // window is exposed, later in the event loop. An empty viewport that was
    // already on screen and picked the stage up has drawn it already.
    HydraViewportWindow* viewport = nullptr;
    for (QWidget* widget : QApplication::allWidgets()) {
        auto* panel = qobject_cast<ViewportViewPanel*>(widget);
        if (panel && panel->currentStage() == stage) {
            viewport = panel->hydraWindow();
            break;
        }
    }
    if (!viewport || viewport->hasRenderedFirstFrame()) {
        finish();
        return;
    }
    m_firstFrameConnection = connect(viewport, &HydraViewportWindow::firstFrameRendered, this, &StageOpener::finish);
    m_firstFrameTimeout.start();
}

void StageOpener::showDialog() {
    if (m_dialog && !m_dialog->isVisible()) {
        m_dialog->show();
    }
}

void StageOpener::finish() {
    if (!m_busy) {
        return;
    }
    m_showTimer.stop();
    m_firstFrameTimeout.stop();
    disconnect(m_firstFrameConnection);
    m_firstFrameConnection = {};
    // Deleted at once, not deleteLater(): a failed open goes on to show a modal
    // message box, whose event loop doesn't run deferred deletes, and
    // QProgressDialog's own force-show timer would bring the hidden dialog back
    // over the message box after a few seconds, blocking it.
    delete m_dialog.data();
    setBusy(false);
}

void StageOpener::setBusy(bool busy) {
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    Q_EMIT busyChanged(busy);
}

}  // namespace usdcc::usd
