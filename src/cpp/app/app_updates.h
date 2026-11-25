#pragma once

// Updates for an installed usdcc (docs/UPDATES.md §4): Velopack asks the GitHub
// releases for a newer version, downloads it in the background, and replaces
// the install when usdcc restarts. Help > Check for Updates, a check a few
// seconds after the start (Settings > Updates), and the notifications that go
// with them.
//
// An install is what usdcc's installer made on Windows, usdcc.app on macOS, or the
// AppImage (InstallLocation). Anything else (the 7z, the tar.xz, a build without
// Velopack) can't update itself: for it the same check asks GitHub's list of releases,
// and points to the newest one's page (docs/UPDATES.md §9).

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QAction;

namespace usdcc::ui {
class MainWindow;
}

namespace usdcc::app {

class AppUpdates : public QObject {
public:
    // `closeWindow` closes the main window as File > Exit does, asking what
    // closing asks, and returns whether it closed.
    AppUpdates(ui::MainWindow* window, std::function<bool()> closeWindow);
    ~AppUpdates() override;

    // Velopack's own start, for the first lines of main() on macOS and Linux: installs an
    // update downloaded earlier (and doesn't return), or notes that this start follows one.
    static void runStartup();

    // Settings > Updates. Before the Settings dialog is first opened.
    static void defineSettings();

    // Whether this usdcc is an install that can update itself.
    bool canUpdate() const;

    // Once the window is up: "Updated to <version>" when the launcher says this
    // start follows an update, and the check a few seconds later, if the setting
    // asks for it.
    void start();
    // Help > Check for Updates: as the startup check, and also says when there's
    // nothing newer or the check failed.
    void checkNow();
    // Makes `action` the Help menu's item: Check for Updates, or once an update
    // was found, Install Update to <version>, then its download's progress, then
    // Restart to Update to <version>. `action` outlives the AppUpdates.
    void setMenuAction(QAction* action);

    // At the end of main(), once the app has quit: when Restart Now closed it,
    // hands the downloaded update to Velopack's updater, which waits for this
    // process to end, installs it and starts usdcc again.
    void applyIfRequested();

private:
    struct State;
    void check(bool interactive);
    void checkReleases(bool interactive);
    void releasesArrived(const QByteArray& json, const QString& failure, bool interactive);
    void checkFailed(const QString& error, bool interactive);
    bool mayRestartForUpdate();
    void download();
    void restartNow();
    void showReady();
    void updateMenuAction();
    void menuActionTriggered();
    QString releasesUrl() const;
    QString releasePage() const;

    ui::MainWindow* m_window;
    std::function<bool()> m_closeWindow;
    QAction* m_menuAction = nullptr;
    QString m_releaseVersion;  // the newer release a copy that can't update itself found, and its tag
    QString m_releaseTag;
    int m_availableToast = 0;  // "usdcc <version> is available"; 0 is none
    std::shared_ptr<State> m_state;
};

}  // namespace usdcc::app
