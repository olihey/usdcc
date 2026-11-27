#include "app_packs.h"

#include "system_proxy.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/core/toast.h"
#include "usdcc/core/version.h"
#include "usdcc/ui/about_dialog.h"
#include "usdcc/ui/toast_manager.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

namespace usdcc::app {

namespace {

constexpr auto kCategory = "usdcc.packs";
constexpr auto kKeepKey = "Updates/keepPacks";
// After the update check's ten seconds, so the two don't start together.
constexpr int kStartupFetchDelayMs = 14000;
constexpr int kRequestTimeoutMs = 30000;

using core::ToastKind;
using core::ToastRequest;
using scripting::InstalledPack;
using scripting::PackListing;

// A folder or a URL of one to read packs from instead of the releases (docs/USD_PLUGINS.md §10).
QString customSource() { return qEnvironmentVariable("USDCC_PACKS_URL"); }

// "A", "A and B", "A, B and C".
QString listed(const QStringList& names) {
    if (names.size() < 2) return names.join(QString());
    return AppPacks::tr("%1 and %2").arg(names.mid(0, names.size() - 1).join(QStringLiteral(", ")), names.last());
}

const InstalledPack* find(const std::vector<InstalledPack>& packs, const QString& name) {
    const std::string wanted = name.toStdString();
    const auto found = std::find_if(packs.begin(), packs.end(), [&wanted](const InstalledPack& pack) { return pack.record.name == wanted; });
    return found == packs.end() ? nullptr : &*found;
}

const PackIndexEntry* find(const std::vector<PackIndexEntry>& index, const QString& name) {
    const auto found = std::find_if(index.begin(), index.end(), [&name](const PackIndexEntry& entry) { return entry.name == name; });
    return found == index.end() ? nullptr : &*found;
}

}  // namespace

AppPacks::AppPacks(Options options, QObject* parent) : PackCatalog(parent), m_options(std::move(options)) {}

AppPacks::~AppPacks() = default;

void AppPacks::defineSettings() {
    core::Settings::instance().define(
        {.key = kKeepKey,
         .category = "Updates",
         .group = "Packs",
         .label = "Keep installed packs up to date",
         .description = "A pack (View > Plugins > Packs) is made for one version of usdcc. With this on, usdcc "
                        "fetches the packs you installed again for a new version: before it restarts to update "
                        "itself, or a few seconds after the start of a copy you unpacked yourself.",
         .type = core::SettingType::Bool,
         .defaultValue = true});
}

QString AppPacks::thisVersion() const { return QString::fromStdString(m_options.store->options().version); }

bool AppPacks::mayAsk() const {
    if (m_options.store->options().platform.empty()) return false;
    if (!customSource().isEmpty()) return true;
    // A development build has no release, and a test doesn't ask GitHub.
    return core::isReleaseBuild() && !qEnvironmentVariableIsSet("USDCC_TEST_PROFILE");
}

QUrl AppPacks::baseUrl(const QString& version) const {
    return packsBaseUrl(version, QString::fromLatin1(ui::AboutDialog::kRepositoryUrl), customSource());
}

void AppPacks::readIndex(const QString& version, std::function<void(std::optional<Index>, const QString&)> done) {
    if (!mayAsk()) {
        // Later, as an answer from the network would be.
        QTimer::singleShot(0, this, [done = std::move(done)]() {
            done(std::nullopt, tr("This usdcc has no release to get packs from."));
        });
        return;
    }
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
        m_network->setProxyFactory(new SystemProxy);
    }
    const QString platform = QString::fromStdString(m_options.store->options().platform);
    const QUrl url = baseUrl(version).resolved(QUrl(packIndexFileName(version, platform)));
    USDCC_LOG_INFO(kCategory, "Reading the list of packs for usdcc {}: {}", version.toStdString(), url.toString().toStdString());
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("usdcc"));
    request.setTransferTimeout(kRequestTimeoutMs);
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, version, platform, done = std::move(done)]() {
        reply->deleteLater();
        // A release from before there were packs, or one whose job for this platform failed.
        if (reply->error() == QNetworkReply::ContentNotFoundError) {
            USDCC_LOG_INFO(kCategory, "usdcc {} has no list of packs for {}", version.toStdString(), platform.toStdString());
            done(Index{}, QString());
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            done(std::nullopt, reply->errorString());
            return;
        }
        QString error;
        std::optional<Index> index = parsePackIndex(reply->readAll(), version, platform, &error);
        done(std::move(index), error);
    });
}

void AppPacks::start() {
    QTimer::singleShot(kStartupFetchDelayMs, this, [this]() {
        if (core::Settings::instance().get<bool>(kKeepKey, true) && mayAsk()) fetchChosen();
    });
}

void AppPacks::fetchChosen() {
    const std::vector<InstalledPack> installed = m_options.store->installed();
    QStringList lacking;
    for (const std::string& name : m_options.store->wanted()) {
        if (!find(installed, QString::fromStdString(name))) lacking << QString::fromStdString(name);
    }
    if (lacking.isEmpty() || m_reading) return;
    m_reading = true;
    Q_EMIT changed();
    readIndex(thisVersion(), [this, lacking](std::optional<Index> index, const QString& error) {
        m_reading = false;
        m_indexAsked = true;
        if (!index) {
            m_indexError = error;
            USDCC_LOG_WARNING(kCategory, "Couldn't read the list of packs for usdcc {}: {}", thisVersion().toStdString(), error.toStdString());
            Q_EMIT changed();
            return;
        }
        m_index = std::move(index);
        m_indexError.clear();
        for (const QString& name : lacking) {
            const PackIndexEntry* entry = offered(name);
            if (!entry) {
                USDCC_LOG_INFO(kCategory, "The {} pack isn't offered for usdcc {} (yet)", name.toStdString(), thisVersion().toStdString());
                continue;
            }
            m_fetching = true;
            const QString title = entry->title;
            enqueue({.request = {*entry, baseUrl(thisVersion()), thisVersion(), {}},
                     .quiet = true,
                     .done = [this, title](bool ok, const QString&) {
                         if (ok) m_fetched << title;
                     }});
        }
        Q_EMIT changed();
    });
}

const PackIndexEntry* AppPacks::offered(const QString& name) const { return m_index ? find(*m_index, name) : nullptr; }

QString AppPacks::titleOf(const QString& name) const {
    if (const PackIndexEntry* entry = offered(name)) return entry->title;
    const std::vector<InstalledPack> installed = m_options.store->installed();
    if (const InstalledPack* pack = find(installed, name)) return QString::fromStdString(pack->record.title);
    return name;
}

std::vector<PackListing> AppPacks::packs() const {
    const QString version = thisVersion();
    const std::vector<InstalledPack> installed = m_options.store->installed();
    const std::vector<InstalledPack>& prepared = m_options.store->prepared();
    const auto fill = [&](PackListing& listing) {
        const QString& name = listing.name;
        const auto isJob = [&](const Job& job) { return job.request.entry.name == name && job.request.version == version; };
        listing.problem = m_problems.value(name);
        if (m_running && isJob(*m_running)) {
            listing.state = m_progress < 0 ? PackListing::State::Installing : PackListing::State::Downloading;
            listing.progress = std::max(0, m_progress);
        } else if (std::any_of(m_queue.begin(), m_queue.end(), isJob)) {
            listing.state = PackListing::State::Downloading;
        } else if (const InstalledPack* pack = find(installed, name)) {
            if (pack->removed) {
                listing.state = PackListing::State::RemovedAtRestart;
            } else if (m_options.store->isInUseHere(name.toStdString())) {
                listing.state = PackListing::State::Installed;
            } else if (const InstalledPack* tried = find(prepared, name); tried && !tried->problem.empty()) {
                listing.state = PackListing::State::NotLoaded;
                listing.problem = QString::fromStdString(tried->problem);
            } else {
                listing.state = PackListing::State::RestartToUse;
            }
        }
    };

    std::vector<PackListing> list;
    QSet<QString> seen;
    if (m_index) {
        for (const PackIndexEntry& entry : *m_index) {
            PackListing listing;
            listing.name = entry.name;
            listing.title = entry.title;
            listing.description = entry.description;
            listing.version = entry.version;
            listing.license = entry.license;
            listing.homepage = entry.homepage;
            listing.renderers = entry.renderers;
            listing.downloadSize = entry.size;
            listing.installedSize = entry.installedSize;
            fill(listing);
            seen.insert(entry.name);
            list.push_back(std::move(listing));
        }
    }
    // Installed, and not in the list: read before the list was, or this version's
    // release doesn't offer it any more.
    for (const InstalledPack& pack : installed) {
        const QString name = QString::fromStdString(pack.record.name);
        if (seen.contains(name)) continue;
        PackListing listing;
        listing.name = name;
        listing.title = QString::fromStdString(pack.record.title);
        listing.version = QString::fromStdString(pack.record.version);
        listing.installedSize = pack.record.installedSize;
        fill(listing);
        seen.insert(name);
        list.push_back(std::move(listing));
    }
    // Chosen, and this version's release has none: after an update that went ahead without it.
    if (m_index) {
        for (const std::string& wanted : m_options.store->wanted()) {
            const QString name = QString::fromStdString(wanted);
            if (seen.contains(name)) continue;
            PackListing listing;
            listing.name = name;
            listing.title = name;
            listing.state = PackListing::State::NotAvailable;
            list.push_back(std::move(listing));
        }
    }
    std::sort(list.begin(), list.end(), [](const PackListing& a, const PackListing& b) {
        return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
    });
    return list;
}

QString AppPacks::note() const {
    if (m_options.store->options().platform.empty()) return tr("There are no packs for this platform.");
    if (!mayAsk()) {
        return core::isReleaseBuild()
                   ? tr("A test profile asks no release for packs. USDCC_PACKS_URL names a folder of packs to read instead.")
                   : tr("This is a development build, with no release to get packs from. USDCC_PACKS_URL names a folder of "
                        "packs to read instead.");
    }
    if (m_reading) return tr("Reading the list of packs...");
    if (!m_indexError.isEmpty()) return tr("The list of packs couldn't be read: %1").arg(m_indexError);
    if (m_index && m_index->empty()) return tr("usdcc %1 has no optional packs for this platform.").arg(thisVersion());
    return {};
}

void AppPacks::refresh() {
    if (m_reading) return;
    m_indexAsked = true;
    if (!mayAsk()) {
        Q_EMIT changed();
        return;
    }
    m_reading = true;
    Q_EMIT changed();
    readIndex(thisVersion(), [this](std::optional<Index> index, const QString& error) {
        m_reading = false;
        if (index) {
            m_index = std::move(index);
            m_indexError.clear();
        } else {
            m_indexError = error;
            USDCC_LOG_WARNING(kCategory, "Couldn't read the list of packs for usdcc {}: {}", thisVersion().toStdString(), error.toStdString());
        }
        Q_EMIT changed();
    });
}

void AppPacks::install(const QString& name) {
    const std::vector<InstalledPack> installed = m_options.store->installed();
    if (const InstalledPack* pack = find(installed, name)) {
        // There, and only marked to go: taking the mark back is all it needs.
        if (pack->removed) keep(name);
        return;
    }
    const PackIndexEntry* entry = offered(name);
    if (!entry) return;
    const QString version = thisVersion();
    const auto same = [&](const Job& job) { return job.request.entry.name == name && job.request.version == version; };
    if ((m_running && same(*m_running)) || std::any_of(m_queue.begin(), m_queue.end(), same)) return;
    m_problems.remove(name);
    enqueue({.request = {*entry, baseUrl(version), version, {}}});
}

void AppPacks::cancel(const QString& name) {
    const QString version = thisVersion();
    const auto same = [&](const Job& job) { return job.request.entry.name == name && job.request.version == version; };
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        if (!same(*it)) {
            ++it;
            continue;
        }
        const Job job = *it;
        it = m_queue.erase(it);
        if (job.done) job.done(false, tr("Cancelled."));
    }
    if (m_running && same(*m_running) && m_installer) m_installer->cancel();
    Q_EMIT changed();
}

void AppPacks::remove(const QString& name) {
    const std::string pack = name.toStdString();
    m_options.store->setWanted(pack, false);
    m_problems.remove(name);
    // Its libraries are in this process, or another usdcc's: it goes at a later start.
    std::string error;
    if (m_options.store->isInUseHere(pack) || !m_options.store->removeNow(pack, &error)) {
        m_options.store->markRemoved(pack, true);
        USDCC_LOG_INFO(kCategory, "The {} pack is removed at the next start", pack);
    } else {
        USDCC_LOG_INFO(kCategory, "Removed the {} pack", pack);
    }
    Q_EMIT changed();
}

void AppPacks::keep(const QString& name) {
    const std::string pack = name.toStdString();
    if (m_options.store->markRemoved(pack, false)) m_options.store->setWanted(pack, true);
    Q_EMIT changed();
}

void AppPacks::restart() {
    if (m_options.restart) m_options.restart();
}

void AppPacks::enqueue(Job job) {
    m_queue.push_back(std::move(job));
    Q_EMIT changed();
    if (!m_running) startNext();
}

void AppPacks::startNext() {
    if (m_running) return;
    if (m_queue.empty()) {
        // What usdcc fetched on its own account, said once for all of it.
        if (m_fetching) {
            m_fetching = false;
            if (!m_fetched.isEmpty() && m_options.toasts) {
                const QStringList titles = std::exchange(m_fetched, {});
                m_options.toasts->show(
                    {.kind = ToastKind::Success,
                     .title = (titles.size() == 1 ? tr("%1 is ready") : tr("%1 are ready")).arg(listed(titles)).toStdString(),
                     .body = tr("Fetched for usdcc %1. Restart usdcc to use %2.")
                                 .arg(thisVersion(), titles.size() == 1 ? tr("it") : tr("them"))
                                 .toStdString(),
                     .durationMs = ToastRequest::kSticky},
                    {{tr("Restart Now"), [this]() { restart(); }}});
            }
            m_fetched.clear();
        }
        return;
    }
    m_running = std::move(m_queue.front());
    m_queue.pop_front();
    m_progress = 0;
    if (!m_installer) {
        m_installer = new PackInstaller(m_options.store->options(), m_options.python, this);
        connect(m_installer, &PackInstaller::progress, this, [this](int percent) {
            m_progress = percent;
            Q_EMIT changed();
        });
        connect(m_installer, &PackInstaller::finished, this, [this](const QString& name, bool) { jobEnded(name, true, {}, false); });
        connect(m_installer, &PackInstaller::failed, this,
                [this](const QString& name, const QString& reason, bool cancelled) { jobEnded(name, false, reason, cancelled); });
    }
    m_installer->start(m_running->request);
    Q_EMIT changed();
}

void AppPacks::jobEnded(const QString& name, bool ok, const QString& reason, bool cancelled) {
    if (!m_running) return;
    const Job job = std::move(*m_running);
    m_running.reset();
    const QString title = job.request.entry.title;
    const bool forThisVersion = job.request.version == thisVersion();
    if (ok) {
        m_problems.remove(name);
        if (forThisVersion) m_options.store->setWanted(name.toStdString(), true);
        if (!job.quiet && m_options.toasts) {
            m_options.toasts->show({.kind = ToastKind::Success,
                                    .title = tr("%1 is installed").arg(title).toStdString(),
                                    .body = tr("Restart usdcc to use it.").toStdString(),
                                    .durationMs = ToastRequest::kSticky},
                                   {{tr("Restart Now"), [this]() { restart(); }}});
        }
    } else if (cancelled) {
        m_problems.remove(name);
    } else {
        if (forThisVersion) m_problems[name] = reason;
        if (!job.quiet && m_options.toasts) {
            m_options.toasts->show({.kind = ToastKind::Error,
                                    .title = tr("%1 wasn't installed").arg(title).toStdString(),
                                    .body = reason.toStdString()});
        }
    }
    if (job.done) job.done(ok, reason);
    Q_EMIT changed();
    startNext();
}

bool AppPacks::keepsPacks() const {
    return core::Settings::instance().get<bool>(kKeepKey, true) && mayAsk() && !m_options.store->wanted().empty();
}

void AppPacks::checkFor(const QString& version, std::function<void(const ForVersion&)> done) {
    readIndex(version, [this, done = std::move(done)](std::optional<Index> index, const QString& error) {
        ForVersion result;
        const std::vector<InstalledPack> installed = m_options.store->installed();
        for (const std::string& wanted : m_options.store->wanted()) {
            const QString name = QString::fromStdString(wanted);
            const PackIndexEntry* entry = index ? find(*index, name) : nullptr;
            if (!entry) {
                result.missing << titleOf(name);
                continue;
            }
            result.fetch << entry->title;
            const InstalledPack* have = find(installed, name);
            const bool same = have && !have->record.content.empty() && have->record.content == entry->content.toStdString();
            if (!same) result.downloadBytes += entry->size;
        }
        if (!index) result.error = error;
        done(result);
    });
}

void AppPacks::fetchFor(const QString& version, std::function<void(const QStringList&, const QString&)> done) {
    readIndex(version, [this, version, done = std::move(done)](std::optional<Index> index, const QString&) {
        // What all the jobs share: how many are left, and what went wrong.
        struct Shared {
            int left = 0;
            QStringList failed;
            QString reason;
            std::function<void(const QStringList&, const QString&)> done;
        };
        auto shared = std::make_shared<Shared>();
        shared->done = done;
        const std::vector<InstalledPack> installed = m_options.store->installed();
        std::vector<Job> jobs;
        for (const std::string& wanted : m_options.store->wanted()) {
            const QString name = QString::fromStdString(wanted);
            const PackIndexEntry* entry = index ? find(*index, name) : nullptr;
            if (!entry) continue;  // checkFor() said so, and the user went ahead
            const InstalledPack* have = find(installed, name);
            const QString title = entry->title;
            jobs.push_back({.request = {*entry, baseUrl(version), version, have ? have->folder : std::filesystem::path()},
                            .quiet = true,
                            .done = [shared, title](bool ok, const QString& reason) {
                                if (!ok) {
                                    shared->failed << title;
                                    shared->reason = reason;
                                }
                                if (--shared->left == 0) shared->done(shared->failed, shared->reason);
                            }});
        }
        shared->left = static_cast<int>(jobs.size());
        if (jobs.empty()) {
            done({}, {});
            return;
        }
        for (Job& job : jobs) enqueue(std::move(job));
    });
}

int AppPacks::installNow(const QStringList& names, const std::function<void(const QString&)>& report) {
    if (m_options.store->options().platform.empty()) {
        report(tr("There are no packs for this platform."));
        return 1;
    }
    if (!mayAsk()) {
        report(note());
        return 1;
    }
    QEventLoop loop;
    int code = 0;
    const QString version = thisVersion();
    readIndex(version, [&](std::optional<Index> index, const QString& error) {
        if (!index) {
            report(tr("The list of packs for usdcc %1 couldn't be read: %2").arg(version, error));
            code = 1;
            loop.quit();
            return;
        }
        m_index = std::move(index);
        QStringList wanted = names;
        if (wanted.isEmpty()) {
            for (const std::string& name : m_options.store->wanted()) wanted << QString::fromStdString(name);
        }
        const std::vector<InstalledPack> installed = m_options.store->installed();
        auto left = std::make_shared<int>(0);
        for (const QString& name : wanted) {
            if (const InstalledPack* pack = find(installed, name); pack && !pack->removed) {
                report(tr("%1 is installed.").arg(QString::fromStdString(pack->record.title)));
                m_options.store->setWanted(name.toStdString(), true);
                continue;
            }
            const PackIndexEntry* entry = offered(name);
            if (!entry) {
                report(tr("%1 isn't offered for usdcc %2 on %3.").arg(name, version, QString::fromStdString(m_options.store->options().platform)));
                code = std::max(code, 2);
                continue;
            }
            if (find(installed, name)) m_options.store->markRemoved(name.toStdString(), false);
            ++*left;
            const QString title = entry->title;
            report(tr("Installing %1 (%2)...").arg(title, packSizeText(entry->size)));
            enqueue({.request = {*entry, baseUrl(version), version, {}},
                     .quiet = true,
                     .done = [&, left, title](bool ok, const QString& reason) {
                         if (ok) {
                             report(tr("%1 is installed.").arg(title));
                         } else {
                             report(tr("%1 wasn't installed: %2").arg(title, reason));
                             code = 1;
                         }
                         if (--*left == 0) loop.quit();
                     }});
        }
        if (*left == 0) loop.quit();
    });
    loop.exec();
    return code;
}

int AppPacks::removeNow(const QStringList& names, const std::function<void(const QString&)>& report) {
    int code = 0;
    const std::vector<InstalledPack> installed = m_options.store->installed();
    for (const QString& name : names) {
        const std::string pack = name.toStdString();
        m_options.store->setWanted(pack, false);
        if (!find(installed, name)) {
            report(tr("%1 isn't installed.").arg(name));
            code = std::max(code, 2);
            continue;
        }
        std::string error;
        if (m_options.store->removeNow(pack, &error)) {
            report(tr("%1 is removed.").arg(name));
        } else if (m_options.store->markRemoved(pack, true)) {
            report(tr("%1 is removed at the next start of usdcc: %2").arg(name, QString::fromStdString(error)));
        } else {
            report(tr("%1 couldn't be removed: %2").arg(name, QString::fromStdString(error)));
            code = 1;
        }
    }
    return code;
}

}  // namespace usdcc::app
