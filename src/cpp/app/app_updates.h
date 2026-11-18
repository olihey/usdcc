#pragma once

// Updates for an installed usdcc (docs/UPDATES.md §4): Velopack asks the GitHub
// releases for a newer version, downloads it in the background, and replaces
// the install when usdcc restarts. Help > Check for Updates, a check a few
// seconds after the start (Settings > Updates), and the notifications that go
// with them.
//
// An install is what usdcc's installer made: bin\usdcc.exe under a "current"
// folder with Velopack's Update.exe beside it. Anything else (the 7z, a build)
// can't update itself, and Check for Updates points to the releases page.
// Built without Velopack (Linux, or offline at configure time), it's the same.

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

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

    // At the end of main(), once the app has quit: when Restart Now closed it,
    // hands the downloaded update to Velopack's updater, which waits for this
    // process to end, installs it and starts usdcc again.
    void applyIfRequested();

private:
    struct State;
    void check(bool interactive);
    void download();
    void restartNow();
    void showReady();
    QString releasesUrl() const;

    ui::MainWindow* m_window;
    std::function<bool()> m_closeWindow;
    std::shared_ptr<State> m_state;
};

}  // namespace usdcc::app
