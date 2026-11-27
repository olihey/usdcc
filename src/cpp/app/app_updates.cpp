#include "app_updates.h"

#if USDCC_FEATURE_PACKS
#include "app_packs.h"
#endif
#include "release_check.h"
#include "running_programs.h"
#include "system_proxy.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/core/toast.h"
#include "usdcc/core/version.h"
#include "usdcc/ui/about_dialog.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/toast_manager.h"

#include <QAction>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#ifdef USDCC_HAS_VELOPACK
#include "Velopack.hpp"
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace usdcc::app {

namespace {

constexpr auto kCategory = "usdcc.updates";
constexpr auto kCheckOnStartupKey = "Updates/checkOnStartup";
constexpr auto kPrereleasesKey = "Updates/prereleases";
// After the start, so the check doesn't compete with loading the stage, plugins and Python.
constexpr int kStartupCheckDelayMs = 10000;
// GitHub allows 60 anonymous requests an hour for an IP address, which a studio shares.
constexpr auto kLastCheckKey = "Updates/lastReleaseCheck";
constexpr qint64 kReleaseCheckIntervalSecs = 24 * 60 * 60;
constexpr int kRequestTimeoutMs = 30000;

// The list of releases. Another one (a file: or http: URL) for testing, in
// USDCC_RELEASES_URL (docs/UPDATES.md §6).
QUrl releaseListUrl() {
    const QString custom = qEnvironmentVariable("USDCC_RELEASES_URL");
    if (!custom.isEmpty()) return QUrl(custom);
    QString url = QString::fromLatin1(ui::AboutDialog::kRepositoryUrl);
    url.replace(QLatin1String("https://github.com/"), QLatin1String("https://api.github.com/repos/"));
    return QUrl(url + QStringLiteral("/releases?per_page=100"));
}

// What this program is, for installing updates.
InstallLocation ownLocation() {
    return installLocation(QCoreApplication::applicationFilePath(), qEnvironmentVariable("APPIMAGE"));
}

std::filesystem::path ownFolder() {
    return std::filesystem::path(QCoreApplication::applicationDirPath().toStdU16String());
}

using core::ToastKind;
using core::ToastRequest;

#ifdef USDCC_HAS_VELOPACK
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
    bool downloading = false;  // busy with the download
    bool ready = false;        // downloaded, waiting for a restart
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

void AppUpdates::runStartup() {
#if defined(USDCC_HAS_VELOPACK) && !defined(_WIN32)
    // There's no QCoreApplication yet to ask for the program's path.
    QString executable;
#if defined(__APPLE__)
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) executable = QString::fromLocal8Bit(path);
#else
    std::error_code error;
    executable = QString::fromStdString(std::filesystem::read_symlink("/proc/self/exe", error).string());
#endif
    if (!installLocation(executable, qEnvironmentVariable("APPIMAGE")).canUpdate()) return;
    bool inUse = false;
    try {
        // macOS: another usdcc or a render node would lose its files (docs/UPDATES.md §9).
        const auto folder = std::filesystem::path(executable.toStdString()).parent_path();
        inUse = !runningPrograms(folder, "usdcc").empty() || !runningPrograms(folder, "usdcc_node").empty();
        Velopack::VelopackApp::Build()
            .SetAutoApplyOnStartup(!inUse)
            .OnRestarted([](void*, const char*) { qputenv("USDCC_UPDATED", "1"); })
            .Run();
    } catch (...) {
        // Never in the way of starting the app.
    }
#endif
}

void AppUpdates::defineSettings() {
    auto& settings = core::Settings::instance();
    settings.define({.key = kCheckOnStartupKey,
                     .category = "Updates",
                     .group = "Updates",
                     .label = "Check for updates when usdcc starts",
                     .description = "A few seconds after the start, usdcc asks GitHub whether there is a newer "
                                    "release, and shows a notification when there is. An installed usdcc can then "
                                    "update itself; another copy asks once a day, and only opens the release's "
                                    "page. Help > Check for Updates checks at any time.",
                     .type = core::SettingType::Bool,
                     .defaultValue = true});
    settings.define({.key = kPrereleasesKey,
                     .category = "Updates",
                     .group = "Updates",
                     .label = "Include prereleases",
                     .description = "Also offer betas and other test releases, which may not be finished.",
                     .type = core::SettingType::Bool,
                     .defaultValue = false});
}

bool AppUpdates::canUpdate() const {
#ifdef USDCC_HAS_VELOPACK
    return ownLocation().canUpdate();
#else
    return false;
#endif
}

QString AppUpdates::releasePage() const {
    if (m_releaseTag.isEmpty()) return releasesUrl();
    return releasesUrl() + QStringLiteral("/tag/") + QString::fromLatin1(QUrl::toPercentEncoding(m_releaseTag));
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
    if (!core::Settings::instance().get<bool>(kCheckOnStartupKey, true)) return;
    if (!canUpdate()) {
        // A development build isn't behind a release, and a test doesn't ask GitHub.
        const bool custom = qEnvironmentVariableIsSet("USDCC_RELEASES_URL");
        if (!custom && (!core::isReleaseBuild() || qEnvironmentVariableIsSet("USDCC_TEST_PROFILE"))) return;
        const qint64 last = ui::appSettings().value(QLatin1String(kLastCheckKey)).toLongLong();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (last <= now && now - last < kReleaseCheckIntervalSecs) return;
    }
    QTimer::singleShot(kStartupCheckDelayMs, this, [this]() { check(false); });
}

void AppUpdates::checkNow() { check(true); }

void AppUpdates::setMenuAction(QAction* action) {
    m_menuAction = action;
    connect(action, &QAction::triggered, this, [this]() { menuActionTriggered(); });
    updateMenuAction();
}

void AppUpdates::updateMenuAction() {
    if (!m_menuAction) return;
    m_menuAction->setEnabled(!m_state->downloading);
#ifdef USDCC_HAS_VELOPACK
    if (m_state->update) {
        const QString version = QString::fromStdString(m_state->update->TargetFullRelease.Version);
        if (m_state->ready) {
            m_menuAction->setText(tr("Restart to &Update to %1").arg(version));
            m_menuAction->setStatusTip(tr("Close usdcc, install the downloaded update and start usdcc again"));
        } else if (m_state->downloading) {
            m_menuAction->setText(tr("Downloading &Update to %1...").arg(version));
            m_menuAction->setStatusTip(tr("The update downloads in the background"));
        } else {
            m_menuAction->setText(tr("Install &Update to %1").arg(version));
            m_menuAction->setStatusTip(
                tr("Download the update in the background; then usdcc restarts to install it"));
        }
        return;
    }
#endif
    if (!m_releaseTag.isEmpty()) {
        m_menuAction->setText(tr("Download &Update to %1...").arg(m_releaseVersion));
        m_menuAction->setStatusTip(tr("Open the newer release's page in your browser"));
        return;
    }
    m_menuAction->setText(tr("Check for &Updates..."));
    m_menuAction->setStatusTip(canUpdate() ? tr("Look for a newer release of usdcc, and install it")
                                           : tr("Look for a newer release of usdcc"));
}

void AppUpdates::menuActionTriggered() {
#ifdef USDCC_HAS_VELOPACK
    if (m_state->ready) {
        restartNow();
        return;
    }
    if (m_state->update) {
        download();
        return;
    }
#endif
    if (!m_releaseTag.isEmpty()) {
        QDesktopServices::openUrl(QUrl(releasePage()));
        return;
    }
    checkNow();
}

void AppUpdates::check(bool interactive) {
    if (!canUpdate()) {
        checkReleases(interactive);
        return;
    }
#ifdef USDCC_HAS_VELOPACK
    ui::ToastManager* toasts = m_window->toasts();
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
            self.updateMenuAction();
            const Velopack::VelopackAsset& target = update->TargetFullRelease;
            const QString version = QString::fromStdString(target.Version);
            // A delta is much smaller than the full package, when there is one.
            quint64 size = 0;
            for (const Velopack::VelopackAsset& delta : update->DeltasToTarget) size += delta.Size;
            if (size == 0) size = target.Size;
            USDCC_LOG_INFO(kCategory, "usdcc {} is available ({} bytes to download)", target.Version, size);
            // The AppUpdates by pointer: `self` is this call's parameter. `packs`: what the
            // update means for the optional packs, said with it; `packBytes` are theirs.
            const auto announce = [updates = &self, version, size](const QString& packs, quint64 packBytes) {
                QString body = tr("You have %1. The update downloads in the background (%2 MB); then "
                                  "usdcc restarts to install it.")
                                   .arg(QString::fromLatin1(core::version()), megabytes(size + packBytes));
                if (!packs.isEmpty()) body += QLatin1Char(' ') + packs;
                updates->m_availableToast = updates->m_window->toasts()->show(
                    {.kind = ToastKind::Info,
                     .title = tr("usdcc %1 is available").arg(version).toStdString(),
                     .body = body.toStdString(),
                     .durationMs = ToastRequest::kSticky},
                    {{tr("Download and Install"), [updates]() { updates->download(); }},
                     {tr("Release Notes"), [updates]() { QDesktopServices::openUrl(QUrl(updates->releasesUrl())); }}});
            };
#if USDCC_FEATURE_PACKS
            // The packs the user chose come with it, where the new version has them.
            if (self.m_packs && self.m_packs->keepsPacks()) {
                self.m_packs->checkFor(version, [announce](const AppPacks::ForVersion& packs) {
                    QStringList notes;
                    if (!packs.fetch.isEmpty()) notes << tr("Your packs come with it: %1.").arg(packs.fetch.join(QStringLiteral(", ")));
                    if (!packs.missing.isEmpty()) {
                        notes << tr("Not available for it yet: %1.").arg(packs.missing.join(QStringLiteral(", ")));
                    }
                    announce(notes.join(QLatin1Char(' ')), static_cast<quint64>(packs.downloadBytes));
                });
                return;
            }
#endif
            announce({}, 0);
        });
    }).detach();
#endif
}

void AppUpdates::checkReleases(bool interactive) {
    if (!interactive && !m_releaseTag.isEmpty()) return;
    if (m_state->busy) {
        if (interactive) {
            m_window->toasts()->show({.kind = ToastKind::Info, .title = tr("Already checking for updates").toStdString()});
        }
        return;
    }
    if (releaseFileSuffix().isEmpty()) {
        checkFailed(tr("There are no releases for this platform."), interactive);
        return;
    }
    m_state->busy = true;
    if (interactive) m_window->statusBar()->showMessage(tr("Checking for updates..."));
    USDCC_LOG_INFO(kCategory, "Asking for the list of releases (usdcc {})", core::version());
    auto* network = new QNetworkAccessManager(this);
    network->setProxyFactory(new SystemProxy);
    QNetworkRequest request(releaseListUrl());
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("usdcc"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(kRequestTimeoutMs);
    QNetworkReply* reply = network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, network, reply, interactive]() {
        const QString failure = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        const QByteArray json = reply->readAll();
        network->deleteLater();  // with the reply
        releasesArrived(json, failure, interactive);
    });
}

void AppUpdates::releasesArrived(const QByteArray& json, const QString& failure, bool interactive) {
    m_state->busy = false;
    if (interactive) m_window->statusBar()->clearMessage();
    // A failed check counts too: a machine without the internet doesn't ask at each start.
    ui::appSettings().setValue(QLatin1String(kLastCheckKey), QDateTime::currentSecsSinceEpoch());
    QString error = failure;
    std::optional<FoundRelease> found;
    const QString current = QString::fromLatin1(core::version());
    if (error.isEmpty()) {
        const bool prereleases = core::Settings::instance().get<bool>(kPrereleasesKey, false);
        found = newestRelease(json, current, releaseFileSuffix(), prereleases, &error);
    }
    if (!error.isEmpty()) {
        checkFailed(error, interactive);
        return;
    }
    if (!found) {
        USDCC_LOG_INFO(kCategory, "No newer release than usdcc {}", core::version());
        if (interactive) {
            m_window->toasts()->show({.kind = ToastKind::Success,
                                      .title = tr("usdcc is up to date").toStdString(),
                                      .body = tr("There is no newer release than %1.").arg(current).toStdString()});
        }
        return;
    }
    USDCC_LOG_INFO(kCategory, "usdcc {} is available", found->version.toStdString());
    m_releaseVersion = found->version;
    m_releaseTag = found->tag;
    updateMenuAction();
    QString body = tr("You have %1.").arg(current) + QLatin1Char(' ');
    if (ownLocation().kind == InstallLocation::Kind::MacMoved) {
        body += tr("To let usdcc update itself, move it to the Applications folder and open it from there.");
    } else {
        body += tr("This copy can't update itself: download the new one.");
    }
    m_window->toasts()->show({.kind = ToastKind::Info,
                              .title = tr("usdcc %1 is available").arg(found->version).toStdString(),
                              .body = body.toStdString(),
                              .durationMs = ToastRequest::kSticky},
                             {{tr("Download"), [this]() { QDesktopServices::openUrl(QUrl(releasePage())); }},
                              {tr("Release Notes"), [this]() { QDesktopServices::openUrl(QUrl(releasesUrl())); }}});
}

void AppUpdates::checkFailed(const QString& error, bool interactive) {
    USDCC_LOG_WARNING(kCategory, "Couldn't check for updates: {}", error.toStdString());
    if (!interactive) return;
    m_window->toasts()->show({.kind = ToastKind::Warning,
                              .title = tr("Couldn't check for updates").toStdString(),
                              .body = error.toStdString()},
                             {{tr("Releases Page"), [this]() { QDesktopServices::openUrl(QUrl(releasesUrl())); }}});
}

void AppUpdates::download() {
#ifdef USDCC_HAS_VELOPACK
    if (m_state->busy || m_state->ready || !m_state->manager || !m_state->update) return;
    m_state->busy = true;
    m_state->downloading = true;
    // Started from the menu, the notification would stay with a button that does nothing.
    if (m_availableToast) m_window->toasts()->dismiss(m_availableToast);
    m_availableToast = 0;
    updateMenuAction();
    m_packsFetched.clear();
#if USDCC_FEATURE_PACKS
    // The packs first (docs/USD_PLUGINS.md §10): a downloaded update installs itself at the
    // next start, with nobody asked, so what there is to ask about them is asked before.
    if (m_packs && m_packs->keepsPacks()) {
        fetchPacksThenDownload();
        return;
    }
#endif
    downloadPackage();
#endif
}

void AppUpdates::stopDownloading() {
    m_state->busy = false;
    m_state->downloading = false;
    updateMenuAction();
    m_window->statusBar()->clearMessage();
}

AppUpdates::PackChoice AppUpdates::askAboutPacks(const QString& text, bool retry) {
    QMessageBox box(QMessageBox::Question, tr("Update usdcc"), text, QMessageBox::NoButton, m_window);
    box.setObjectName(QStringLiteral("updatePacksQuestion"));
    const QPushButton* update = box.addButton(tr("Update Anyway"), QMessageBox::AcceptRole);
    const QPushButton* again = retry ? box.addButton(tr("Retry"), QMessageBox::ActionRole) : nullptr;
    box.setDefaultButton(box.addButton(tr("Later"), QMessageBox::RejectRole));
    box.exec();
    if (box.clickedButton() == update) return PackChoice::UpdateAnyway;
    return again && box.clickedButton() == again ? PackChoice::Retry : PackChoice::Later;
}

void AppUpdates::fetchPacksThenDownload() {
#if defined(USDCC_HAS_VELOPACK) && USDCC_FEATURE_PACKS
    const QString version = QString::fromStdString(m_state->update->TargetFullRelease.Version);
    m_window->statusBar()->showMessage(tr("Looking for the packs of usdcc %1...").arg(version));
    m_packs->checkFor(version, [this, version](const AppPacks::ForVersion& packs) {
        if (!m_state->downloading) return;
        if (!packs.missing.isEmpty()) {
            const QString names = packs.missing.join(QStringLiteral(", "));
            USDCC_LOG_WARNING(kCategory, "usdcc {} has no {} pack{}", version.toStdString(), names.toStdString(),
                              packs.error.isEmpty() ? std::string() : " (" + packs.error.toStdString() + ")");
            QString text = packs.error.isEmpty()
                               ? (packs.missing.size() == 1 ? tr("%1 isn't available for usdcc %2 yet.")
                                                            : tr("%1 aren't available for usdcc %2 yet."))
                                     .arg(names, version)
                               : tr("The packs of usdcc %1 couldn't be looked up: %2").arg(version, packs.error);
            text += QStringLiteral("\n\n") +
                    tr("Updating now leaves usdcc without %1 until it is there; usdcc then fetches it on its own.")
                        .arg(packs.missing.size() == 1 ? names : tr("them"));
            if (askAboutPacks(text, false) != PackChoice::UpdateAnyway) {
                stopDownloading();
                return;
            }
        }
        m_packsFetched = packs.fetch.join(QStringLiteral(", "));
        fetchPacks(version);
    });
#endif
}

void AppUpdates::fetchPacks(const QString& version) {
#if defined(USDCC_HAS_VELOPACK) && USDCC_FEATURE_PACKS
    m_window->statusBar()->showMessage(tr("Fetching your packs for usdcc %1...").arg(version));
    m_packs->fetchFor(version, [this, version](const QStringList& failed, const QString& reason) {
        if (!m_state->downloading) return;
        if (!failed.isEmpty()) {
            const QString names = failed.join(QStringLiteral(", "));
            const QString text = tr("%1 couldn't be fetched for usdcc %2: %3").arg(names, version, reason) + QStringLiteral("\n\n") +
                                 tr("Updating now leaves usdcc without %1 until usdcc can fetch it.")
                                     .arg(failed.size() == 1 ? names : tr("them"));
            switch (askAboutPacks(text, true)) {
                case PackChoice::Retry:
                    fetchPacks(version);
                    return;
                case PackChoice::Later:
                    stopDownloading();
                    return;
                case PackChoice::UpdateAnyway:
                    m_packsFetched.clear();  // not all of them: the notification names none
                    break;
            }
        }
        downloadPackage();
    });
#else
    (void)version;
#endif
}

// The app's own package, once what goes with it is there. download() has set the state.
void AppUpdates::downloadPackage() {
#ifdef USDCC_HAS_VELOPACK
    if (m_state->ready || !m_state->manager || !m_state->update) {
        stopDownloading();
        return;
    }
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
            state->downloading = false;
            state->ready = error.empty();
            self.updateMenuAction();
            self.m_window->statusBar()->clearMessage();
            if (!error.empty()) {
                USDCC_LOG_ERROR(kCategory, "Couldn't download usdcc {}: {}", version.toStdString(), error);
                self.m_window->toasts()->show({.kind = ToastKind::Error,
                                               .title = tr("Couldn't download usdcc %1").arg(version).toStdString(),
                                               .body = error});
                return;
            }
            USDCC_LOG_INFO(kCategory, "Downloaded usdcc {}", version.toStdString());
            self.showReady();
        });
    }).detach();
#endif
}

void AppUpdates::showReady() {
#ifdef USDCC_HAS_VELOPACK
    if (!m_state->update) return;
    const QString version = QString::fromStdString(m_state->update->TargetFullRelease.Version);
    QString body = tr("Restart usdcc to finish updating. Otherwise it updates the next time usdcc starts.");
    if (!m_packsFetched.isEmpty()) body += QLatin1Char(' ') + tr("Your packs are there for it: %1.").arg(m_packsFetched);
    m_window->toasts()->show({.kind = ToastKind::Success,
                              .title = tr("usdcc %1 is ready").arg(version).toStdString(),
                              .body = body.toStdString(),
                              .durationMs = ToastRequest::kSticky},
                             {{tr("Restart Now"), [this]() { restartNow(); }}});
#endif
}

// Installing stops what runs from the install, or takes its files away (docs/UPDATES.md §9).
// Never another usdcc, which may have unsaved work; a render node when the user says so.
bool AppUpdates::mayRestartForUpdate() {
    const auto folder = ownFolder();
    if (!runningPrograms(folder, "usdcc").empty()) {
        QMessageBox::information(m_window, tr("Update usdcc"),
                                 tr("usdcc is open more than once. Close the other windows first: "
                                    "the update would stop them."));
        return false;
    }
    if (runningPrograms(folder, "usdcc_node").empty()) return true;
    QMessageBox box(QMessageBox::Question, tr("Update usdcc"), tr("A render node is running; updating will stop it."),
                    QMessageBox::NoButton, m_window);
    const QPushButton* update = box.addButton(tr("Update Anyway"), QMessageBox::AcceptRole);
    box.setDefaultButton(box.addButton(tr("Later"), QMessageBox::RejectRole));
    box.exec();
    return box.clickedButton() == update;
}

void AppUpdates::restartNow() {
    if (!mayRestartForUpdate()) return;
    m_state->restartRequested = true;
    // Asks about unsaved changes as closing does; cancelled, nothing happens.
    if (!m_closeWindow()) m_state->restartRequested = false;
}

void AppUpdates::applyIfRequested() {
#ifdef USDCC_HAS_VELOPACK
    if (!m_state->restartRequested || !m_state->ready || !m_state->manager || !m_state->update) return;
    USDCC_LOG_INFO(kCategory, "Restarting to install usdcc {}", m_state->update->TargetFullRelease.Version);
    try {
        // macOS: Velopack's updater doesn't stop them there.
        stopNodes(runningPrograms(ownFolder(), "usdcc_node"));
        // Velopack's updater waits for this process to end (up to a minute),
        // installs the update with a progress window, and starts usdcc again.
        m_state->manager->WaitExitThenApplyUpdates(*m_state->update, false, true);
    } catch (const std::exception& error) {
        USDCC_LOG_ERROR(kCategory, "Couldn't start the updater: {}", error.what());
    }
#endif
}

}  // namespace usdcc::app
