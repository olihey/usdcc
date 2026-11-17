#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/stage.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <string>
#include <vector>

class QProgressDialog;
class QThread;
class QWidget;

namespace usdcc::usd {

class StageManager;

// Opens a stage from the UI without freezing it: UsdStage::Open() runs on a
// worker thread while a progress dialog is shown, then the stage is registered
// with the StageManager on the UI thread, and the dialog stays up through the
// "Preparing viewport..." phase until the new viewport has drawn its first
// frame (that first Render() is where Hydra builds its scene, and it blocks the
// UI thread too — it can't be moved off it, as it needs the GL context).
//
// The dialog only appears once the open has taken a while, so small stages
// don't flash it. USD reports no progress while opening, so it is a busy
// indicator, and there is no Cancel: Open() can't be interrupted.
//
// One open at a time: open() is ignored while busy() — listen to busyChanged()
// to disable whatever triggers it.
//
// Startup opens (the stage named on the command line) deliberately don't use
// this: they happen before there is a window, so StageManager::openStage()
// just blocks.
class USDCC_USD_UI_API StageOpener : public QObject {
    Q_OBJECT

public:
    // `dialogParent` is the window the progress dialog is modal to.
    StageOpener(StageManager* stageManager, QWidget* dialogParent);
    ~StageOpener() override;

    bool busy() const { return m_busy; }

    // File > Open Stage with Options... (docs/WORKSPACES.md §8).
    struct Options {
        // Off: opened with UsdStage::LoadNone, its payloads unloaded.
        bool loadPayloads = true;
        // Default: Open Stage's way (its viewport in front and current, the other panels
        // as they are). Everywhere: as Stages > Make Current, every panel on screen
        // shows it. No: its viewport goes behind, and the current stage stays.
        enum class MakeCurrent { Default, Everywhere, No };
        MakeCurrent makeCurrent = MakeCurrent::Default;
        // A workspace to switch to once it's open; empty to keep the layout.
        QString workspace;
        // The asset environment it opens in (docs/REMOTE_ASSETS.md §4); empty for the default.
        std::string assetEnvironment;
    };

    // Starts opening `path` (a file path or resolvable USD asset path).
    // Failure is reported to the user in a message box.
    void open(const QString& path);
    void open(const QString& path, const Options& options);

    // While a stage it opened is being registered (stageOpened is firing): the
    // options it was opened with. Null otherwise.
    const Options* adopting() const { return m_adopting ? &m_options : nullptr; }

Q_SIGNALS:
    void busyChanged(bool busy);
    // The stage is open and registered, before its first frame. `wasOpen`: the file
    // was open already, so it came forward and `options.loadPayloads` didn't apply.
    void opened(const usdcc::usd::StageRefPtr& stage, const usdcc::usd::StageOpener::Options& options, bool wasOpen);
    // The failure dialog's Show Log button was clicked.
    void showLogRequested();

private:
    void onLoaded(const PXR_NS::UsdStageRefPtr& usdStage, const std::vector<std::string>& errors);
    void waitForFirstFrame(const StageRefPtr& stage);
    void showDialog();
    void finish();
    void setBusy(bool busy);

    StageManager* m_stageManager;
    QWidget* m_dialogParent;

    bool m_busy = false;
    bool m_adopting = false;
    Options m_options;
    QString m_path;
    QElapsedTimer m_elapsed;  // since open() was called
    QThread* m_thread = nullptr;
    QPointer<QProgressDialog> m_dialog;
    QTimer m_showTimer;     // reveals the dialog if the open is slow
    QTimer m_firstFrameTimeout;  // gives up waiting for a first frame that never comes
    QMetaObject::Connection m_firstFrameConnection;
};

}  // namespace usdcc::usd
