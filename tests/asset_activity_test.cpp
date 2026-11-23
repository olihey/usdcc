// What the status bar shows of downloads (docs/REMOTE_ASSETS.md §9): one summary, not a
// message per file. Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/assets/activity.h"
#include "usdcc/usd/asset_settings.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QStatusBar>
#include <QString>
#include <QThread>

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace usdcc::assets;

namespace {
void runEvents(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

struct Installed {
    QMainWindow window;
    Installed() { usdcc::usd::installAssetActivity(&window); }
    ~Installed() {
        setActivityListener(nullptr);
        setHostCallback(nullptr);
        setBucketRefusedCallback(nullptr);
    }
};
}  // namespace

USDCC_TEST(asset_activity_many_files_few_messages) {
    Installed installed;
    int messages = 0;
    QObject::connect(installed.window.statusBar(), &QStatusBar::messageChanged, &installed.window, [&messages]() { ++messages; });
    const std::shared_ptr<ActivityListener> listener = activityListener();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([listener, t]() {
            for (int i = 0; i < 500; ++i) {
                const std::string url = "http://host/f" + std::to_string(t) + "_" + std::to_string(i) + ".usd";
                listener->started(url);
                listener->advanced(url, 524288, 1048576);
                listener->advanced(url, 1048576, 1048576);
                listener->finished(url, Status::Ok);
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    runEvents(500);
    const QString running = installed.window.statusBar()->currentMessage();
    CHECK(running.contains("2000 files done") && running.contains("2000.0 MB"), "one summary of all of them");
    runEvents(1200);
    CHECK(installed.window.statusBar()->currentMessage().startsWith("Downloaded 2000 files"), "and the total at the end");
    CHECK(messages <= 5, "a few messages, not one per file");
}

USDCC_TEST(asset_activity_one_file_failure_and_next_batch) {
    Installed installed;
    QStatusBar* bar = installed.window.statusBar();
    const std::shared_ptr<ActivityListener> listener = activityListener();
    listener->started("http://host/a.usd");
    listener->advanced("http://host/a.usd", 1048576, 4194304);
    runEvents(500);
    CHECK(bar->currentMessage() == "Downloading a.usd (1.0 of 4.0 MB)", "one file by itself has its size");
    listener->finished("http://host/a.usd", Status::Unreachable);
    runEvents(1500);
    CHECK(bar->currentMessage() == "Couldn't download a.usd", "a failure is shown");

    listener->started("http://host/b.usd");
    runEvents(500);
    CHECK(bar->currentMessage() == "Downloading b.usd", "the next batch starts afresh");
    listener->finished("http://host/b.usd", Status::Ok);
    runEvents(1500);
    CHECK(bar->currentMessage() == "Downloaded b.usd", "and ends with its name");
}
