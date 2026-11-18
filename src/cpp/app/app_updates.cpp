#include "app_updates.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/core/toast.h"
#include "usdcc/core/version.h"
#include "usdcc/ui/about_dialog.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/toast_manager.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#ifdef USDCC_HAS_VELOPACK
#include "Velopack.hpp"
#endif

namespace usdcc::app {

namespace {

constexpr auto kCategory = "usdcc.updates";
constexpr auto kCheckOnStartupKey = "Updates/checkOnStartup";
constexpr auto kPrereleasesKey = "Updates/prereleases";
// After the start, so the check doesn't compete with loading the stage, plugins and Python.
constexpr int kStartupCheckDelayMs = 10000;

using core::ToastKind;
using core::ToastRequest;

#ifdef USDCC_HAS_VELOPACK
// The folder of the Velopack install this program runs from (it holds
// Update.exe and current\bin\usdcc.exe), or empty when it isn't installed. As
// Velopack's own locator finds it, without asking Velopack, which would log an
// error for every copy that isn't installed.
QString installRoot() {
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const qsizetype at = exe.lastIndexOf(QStringLiteral("\\current\\"));
    if (at <= 0) return {};
    const QString root = exe.left(at);
    return QFileInfo::exists(root + QStringLiteral("\\Update.exe")) ? root : QString();
}

QString megabytes(quint64 bytes) {
    return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 0);
}

// Velopack's own messages, into usdcc's log.
void logFromVelopack(void*, const char* level, const char* message) {
    const std::string_view name = level ? level : "";
    const core::LogLevel mapped = name == "error"                     ? core::LogLevel::Error
                                  : name == "warn" || name == "warning" ? core::LogLevel::Warning
                                  : name == "info"                      ? core::LogLevel::Info
                                                                        : core::LogLevel::Debug;
    core::LogHub::instance().log(mapped, "usdcc", kCategory, message ? message : "");
}
#endif

}  // namespace

// Shared with the background threads that check and download: they post their
// results back through post(), which does nothing once the AppUpdates is gone.
struct AppUpdates::State {
    std::mutex mutex;
    AppUpdates* owner = nullptr;  // under `mutex`

    // The rest on the UI thread only.
    bool busy = false;
    bool ready = false;  // downloaded, waiting for a restart
    bool restartRequested = false;
#ifdef USDCC_HAS_VELOPACK
    std::shared_ptr<Velopack::UpdateManager> manager;
    std::optional<Velopack::UpdateInfo> update;
#endif
    int lastProgress = -1;  // the download's background thread only

    // Runs `then` on the UI thread with the AppUpdates, unless it's gone by then.
    void post(std::function<void(AppUpdates&)> then) {
        std::lock_guard lock(mutex);
        if (!owner) return;
        AppUpdates* target = owner;
        // Events posted to an object are dropped when it is deleted, and it
        // can't be deleted while this holds the mutex.
        QMetaObject::invokeMethod(
            target, [target, then = std::move(then)]() { then(*target); }, Qt::QueuedConnection);
    }
};

AppUpdates::AppUpdates(ui::MainWindow* window, std::function<bool()> closeWindow)
    : m_window(window), m_closeWindow(std::move(closeWindow)), m_state(std::make_shared<State>()) {
    m_state->owner = this;
#ifdef USDCC_HAS_VELOPACK
    static std::once_flag logger;
    std::call_once(logger, []() { Velopack::VelopackApp::Build().SetLogger(&logFromVelopack, nullptr); });
#endif
}

AppUpdates::~AppUpdates() {
    std::lock_guard lock(m_state->mutex);
    m_state->owner = nullptr;
}

void AppUpdates::defineSettings() {
#ifdef USDCC_HAS_VELOPACK
    auto& settings = core::Settings::instance();
    settings.define({.key = kCheckOnStartupKey,
                     .category = "Updates",
                     .group = "Updates",
                     .label = "Check for updates when usdcc starts",
                     .description = "A few seconds after the start, usdcc asks GitHub whether there is a newer "
                                    "release, and shows a notification when there is. Only an installed usdcc "
                                    "can update itself; Help > Check for Updates checks at any time.",
                     .type = core::SettingType::Bool,
                     .defaultValue = true});
    settings.define({.key = kPrereleasesKey,
                     .category = "Updates",
                     .group = "Updates",
                     .label = "Include prereleases",
                     .description = "Also offer betas and other test releases, which may not be finished.",
                     .type = core::SettingType::Bool,
                     .defaultValue = false});
#endif
}

bool AppUpdates::canUpdate() const {
#ifdef USDCC_HAS_VELOPACK
    return !installRoot().isEmpty();
#else
    return false;
#endif
}

QString AppUpdates::releasesUrl() const {
    return QString::fromLatin1(ui::AboutDialog::kRepositoryUrl) + QStringLiteral("/releases");
}

void AppUpdates::start() {
    // Set by the launcher when Velopack restarted it after installing an update
    // (src/cpp/launcher). Not for the programs usdcc starts.
    if (qEnvironmentVariableIsSet("USDCC_UPDATED")) {
        qunsetenv("USDCC_UPDATED");
        USDCC_LOG_INFO(kCategory, "Updated to usdcc {}", core::version());
        m_window->toasts()->show({.kind = ToastKind::Success,
                                  .title = tr("Updated to usdcc %1").arg(QString::fromLatin1(core::version())).toStdString(),
                                  .body = tr("What's new is on the releases page.").toStdString()},
                                 {{tr("Release Notes"), [this]() { QDesktopServices::openUrl(QUrl(releasesUrl())); }}});
    }
    if (!canUpdate()) return;
    if (!core::Settings::instance().get<bool>(kCheckOnStartupKey, true)) return;
    QTimer::singleShot(kStartupCheckDelayMs, this, [this]() { check(false); });
}

void AppUpdates::checkNow() { check(true); }

void AppUpdates::check(bool interactive) {
    ui::ToastManager* toasts = m_window->toasts();
    if (!canUpdate()) {
        if (!interactive) return;
        const QString why =
#ifdef USDCC_HAS_VELOPACK
            tr("This usdcc wasn't installed with usdcc's installer, so it can't update itself. ");
#else
            tr("This build of usdcc can't update itself. ");
#endif
        toasts->show({.kind = ToastKind::Info,
                      .title = tr("Updates come with the installer").toStdString(),
                      .body = (why + tr("The releases page has the latest version (you have %1).")
                                         .arg(QString::fromLatin1(core::version())))
                                  .toStdString()},
                     {{tr("Releases Page"), [this]() { QDesktopServices::openUrl(QUrl(releasesUrl())); }}});
        return;
    }
#ifdef USDCC_HAS_VELOPACK
    if (m_state->ready) {
        showReady();
        return;
    }
    // The startup check, after Check for Updates already found one.
    if (!interactive && m_state->update) return;
    if (m_state->busy) {
        if (interactive) {
            toasts->show({.kind = ToastKind::Info, .title = tr("Already checking for updates").toStdString()});
        }
        return;
    }

    // A new manager each time, so a change to Include prereleases counts. A
    // custom feed (a folder or a URL of vpk's output) for testing, in
    // USDCC_UPDATE_SOURCE (docs/UPDATES.md §6).
    std::shared_ptr<Velopack::UpdateManager> manager;
    try {
        const QString custom = qEnvironmentVariable("USDCC_UPDATE_SOURCE");
        if (!custom.isEmpty()) {
            manager = std::make_shared<Velopack::UpdateManager>(custom.toStdString());
        } else {
            const bool prereleases = core::Settings::instance().get<bool>(kPrereleasesKey, false);
            manager = std::make_shared<Velopack::UpdateManager>(std::make_unique<Velopack::GithubSource>(
                ui::AboutDialog::kRepositoryUrl, std::string(), prereleases));
        }
    } catch (const std::exception& error) {
        USDCC_LOG_WARNING(kCategory, "Can't check for updates: {}", error.what());
        if (interactive) {
            toasts->show({.kind = ToastKind::Warning,
                          .title = tr("Couldn't check for updates").toStdString(),
                          .body = error.what()});
        }
        return;
    }

    m_state->busy = true;
    if (interactive) m_window->statusBar()->showMessage(tr("Checking for updates..."));
    USDCC_LOG_INFO(kCategory, "Checking for updates (usdcc {})", core::version());
    std::thread([state = m_state, manager, interactive]() {
        std::optional<Velopack::UpdateInfo> update;
        std::string error;
        try {
            update = manager->CheckForUpdates();
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        state->post([state, manager, interactive, update = std::move(update), error](AppUpdates& self) {
            state->busy = false;
            if (interactive) self.m_window->statusBar()->clearMessage();
            ui::ToastManager* toasts = self.m_window->toasts();
            if (!error.empty()) {
                USDCC_LOG_WARNING(kCategory, "Couldn't check for updates: {}", error);
                if (interactive) {
                    toasts->show({.kind = ToastKind::Warning,
                                  .title = tr("Couldn't check for updates").toStdString(),
                                  .body = error});
                }
                return;
            }
            if (!update) {
                USDCC_LOG_INFO(kCategory, "usdcc {} is the latest version", core::version());
                if (interactive) {
                    toasts->show({.kind = ToastKind::Success,
                                  .title = tr("usdcc is up to date").toStdString(),
                                  .body = tr("%1 is the latest version.")
                                              .arg(QString::fromLatin1(core::version()))
                                              .toStdString()});
                }
                return;
            }
            state->manager = manager;
            state->update = update;
            const Velopack::VelopackAsset& target = update->TargetFullRelease;
            const QString version = QString::fromStdString(target.Version);
            // A delta is much smaller than the full package, when there is one.
            quint64 size = 0;
            for (const Velopack::VelopackAsset& delta : update->DeltasToTarget) size += delta.Size;
            if (size == 0) size = target.Size;
            USDCC_LOG_INFO(kCategory, "usdcc {} is available ({} bytes to download)", target.Version, size);
            toasts->show({.kind = ToastKind::Info,
                          .title = tr("usdcc %1 is available").arg(version).toStdString(),
                          .body = tr("You have %1. The update downloads in the background (%2 MB); then "
                                     "usdcc restarts to install it.")
                                      .arg(QString::fromLatin1(core::version()), megabytes(size))
                                      .toStdString(),
                          .durationMs = ToastRequest::kSticky},
                         // The AppUpdates by pointer: `self` is this call's parameter.
                         {{tr("Download and Install"), [updates = &self]() { updates->download(); }},
                          {tr("Release Notes"),
                           [updates = &self]() { QDesktopServices::openUrl(QUrl(updates->releasesUrl())); }}});
        });
    }).detach();
#endif
}

void AppUpdates::download() {
#ifdef USDCC_HAS_VELOPACK
    if (m_state->busy || m_state->ready || !m_state->manager || !m_state->update) return;
    m_state->busy = true;
    m_state->lastProgress = -1;
    const QString version = QString::fromStdString(m_state->update->TargetFullRelease.Version);
    m_window->statusBar()->showMessage(tr("Downloading usdcc %1...").arg(version));
    USDCC_LOG_INFO(kCategory, "Downloading usdcc {}", version.toStdString());
    std::thread([state = m_state, manager = m_state->manager, update = *m_state->update, version]() {
        std::string error;
        try {
            manager->DownloadUpdates(
                update,
                [](void* data, size_t percent) {
                    auto* state = static_cast<State*>(data);
                    if (static_cast<int>(percent) == state->lastProgress) return;
                    state->lastProgress = static_cast<int>(percent);
                    state->post([percent](AppUpdates& self) {
                        if (!self.m_state->busy || !self.m_state->update) return;
                        self.m_window->statusBar()->showMessage(
                            tr("Downloading usdcc %1: %2%")
                                .arg(QString::fromStdString(self.m_state->update->TargetFullRelease.Version))
                                .arg(percent));
                    });
                },
                state.get());
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        state->post([state, version, error](AppUpdates& self) {
            state->busy = false;
            self.m_window->statusBar()->clearMessage();
            if (!error.empty()) {
                USDCC_LOG_ERROR(kCategory, "Couldn't download usdcc {}: {}", version.toStdString(), error);
                self.m_window->toasts()->show({.kind = ToastKind::Error,
                                               .title = tr("Couldn't download usdcc %1").arg(version).toStdString(),
                                               .body = error});
                return;
            }
            USDCC_LOG_INFO(kCategory, "Downloaded usdcc {}", version.toStdString());
            state->ready = true;
            self.showReady();
        });
    }).detach();
#endif
}

void AppUpdates::showReady() {
#ifdef USDCC_HAS_VELOPACK
    if (!m_state->update) return;
    const QString version = QString::fromStdString(m_state->update->TargetFullRelease.Version);
    m_window->toasts()->show({.kind = ToastKind::Success,
                              .title = tr("usdcc %1 is ready").arg(version).toStdString(),
                              .body = tr("Restart usdcc to finish updating. Otherwise it updates the next time "
                                         "usdcc starts.")
                                          .toStdString(),
                              .durationMs = ToastRequest::kSticky},
                             {{tr("Restart Now"), [this]() { restartNow(); }}});
#endif
}

void AppUpdates::restartNow() {
    m_state->restartRequested = true;
    // Asks about unsaved changes as closing does; cancelled, nothing happens.
    if (!m_closeWindow()) m_state->restartRequested = false;
}

void AppUpdates::applyIfRequested() {
#ifdef USDCC_HAS_VELOPACK
    if (!m_state->restartRequested || !m_state->ready || !m_state->manager || !m_state->update) return;
    USDCC_LOG_INFO(kCategory, "Restarting to install usdcc {}", m_state->update->TargetFullRelease.Version);
    try {
        // Velopack's updater waits for this process to end (up to a minute),
        // installs the update with a progress window, and starts usdcc again.
        m_state->manager->WaitExitThenApplyUpdates(*m_state->update, false, true);
    } catch (const std::exception& error) {
        USDCC_LOG_ERROR(kCategory, "Couldn't start the updater: {}", error.what());
    }
#endif
}

}  // namespace usdcc::app
