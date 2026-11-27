#pragma once

// The optional packs in the app (docs/USD_PLUGINS.md §10): View > Plugins > Packs lists
// what this version's release offers and what is installed, installs and removes them,
// and keeps the chosen ones over updates. A pack is fetched for exactly one usdcc
// version: at a start that lacks one the user chose (a new 7z, or an update that went
// ahead without it), and for an update of the app before its restart (AppUpdates).
//
// Nothing asks the network before the window is up, and nothing at all in a development
// build or under a test profile, unless USDCC_PACKS_URL names a folder or URL to read.

#include "pack_index.h"
#include "pack_installer.h"

#include "usdcc/scripting/pack_catalog.h"
#include "usdcc/scripting/pack_store.h"

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

class QNetworkAccessManager;

namespace usdcc::ui {
class ToastManager;
}

namespace usdcc::app {

class AppPacks : public scripting::PackCatalog {
public:
    struct Options {
        // The app's store, which made this start's packs loadable. Outlives this.
        scripting::PackStore* store = nullptr;
        // usdcc's own Python, which unpacks; empty when there is none.
        std::filesystem::path python;
        // Null: nothing is shown (--install-packs).
        ui::ToastManager* toasts = nullptr;
        // Closes the app, asking what closing asks, and starts it again. False when it stayed.
        std::function<bool()> restart;
    };

    explicit AppPacks(Options options, QObject* parent = nullptr);
    ~AppPacks() override;

    // Settings > Updates > Keep installed packs up to date. Before the Settings dialog opens.
    static void defineSettings();

    // Once the window is up: a few seconds later, the packs the user chose that this
    // version lacks are fetched, if the setting asks for it.
    void start();
    // What start() does when its time has come: fetches the chosen packs this version
    // lacks and its release offers, and says so once they are in.
    void fetchChosen();

    // PackCatalog.
    std::vector<scripting::PackListing> packs() const override;
    QString note() const override;
    void refresh() override;
    void install(const QString& name) override;
    void cancel(const QString& name) override;
    void remove(const QString& name) override;
    void keep(const QString& name) override;
    void restart() override;

    // Whether an update of usdcc has packs to take along: the setting is on, and the
    // user chose some.
    bool keepsPacks() const;

    // What the chosen packs need for usdcc `version`, before an update to it.
    struct ForVersion {
        QString error;         // why its list of packs couldn't be read; empty when it was
        QStringList missing;   // titles of chosen packs it doesn't offer
        QStringList fetch;     // titles of those it does
        qint64 downloadBytes = 0;  // of those, what has to be downloaded and not copied
    };
    void checkFor(const QString& version, std::function<void(const ForVersion&)> done);
    // Installs the chosen packs `version`'s release offers into that version's folder,
    // beside this one's. `done` with the titles of those that failed, and why (the last).
    void fetchFor(const QString& version, std::function<void(const QStringList& failed, const QString& reason)> done);

    // --install-packs [names] and --remove-packs names: no window, an event loop of its
    // own. No names: the chosen packs. 0 when everything went in, 2 when some isn't
    // offered for this version, 1 when something failed. What happens goes to `report`.
    int installNow(const QStringList& names, const std::function<void(const QString&)>& report);
    int removeNow(const QStringList& names, const std::function<void(const QString&)>& report);

private:
    struct Job {
        PackInstaller::Request request;
        bool quiet = false;  // fetched by usdcc itself: one notification for all, not one each
        std::function<void(bool ok, const QString& reason)> done;
    };
    using Index = std::vector<PackIndexEntry>;

    bool mayAsk() const;
    QUrl baseUrl(const QString& version) const;
    QString thisVersion() const;
    // Reads `version`'s list of packs; `done` with it, or nothing and why.
    void readIndex(const QString& version, std::function<void(std::optional<Index>, const QString& error)> done);
    void enqueue(Job job);
    void startNext();
    void jobEnded(const QString& name, bool ok, const QString& reason, bool cancelled);
    const PackIndexEntry* offered(const QString& name) const;
    QString titleOf(const QString& name) const;

    Options m_options;
    QNetworkAccessManager* m_network = nullptr;
    PackInstaller* m_installer = nullptr;
    std::deque<Job> m_queue;
    std::optional<Job> m_running;
    int m_progress = 0;
    std::optional<Index> m_index;  // this version's, once read
    QString m_indexError;
    bool m_indexAsked = false;
    bool m_reading = false;
    QMap<QString, QString> m_problems;  // by name: why the last install failed
    // Fetched on usdcc's own account since the start, to say so once.
    QStringList m_fetched;
    bool m_fetching = false;
};

}  // namespace usdcc::app
