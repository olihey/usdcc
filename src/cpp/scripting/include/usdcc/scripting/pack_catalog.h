#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace usdcc::scripting {

// A pack as View > Plugins > Packs lists it (docs/USD_PLUGINS.md §10): one the release
// of this usdcc offers, or one that is installed.
struct PackListing {
    enum class State {
        NotInstalled,    // offered, not here
        Downloading,     // `progress` percent
        Installing,      // downloaded and checked; being unpacked, or copied
        RestartToUse,    // installed since the start
        Installed,       // loaded at this start
        RemovedAtRestart,  // loaded, and marked to go
        NotLoaded,       // installed, but this usdcc couldn't load it: `problem`
        NotAvailable,    // installed, and this version's release doesn't offer it
    };
    QString name;  // "moonray"
    QString title;
    QString description;
    QString version;
    QString license;
    QString homepage;
    QStringList renderers;
    qint64 downloadSize = 0;   // bytes; 0 when not known
    qint64 installedSize = 0;
    State state = State::NotInstalled;
    int progress = 0;
    QString problem;  // why it isn't loaded, or why the last install failed
};

// What the Plugins panel shows and asks for; the app's AppPacks is the one that does it.
// Nothing here uses the network until refresh() or install().
class PackCatalog : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual std::vector<PackListing> packs() const = 0;
    // A line under the list: why it is empty, how old it is, or nothing.
    virtual QString note() const = 0;
    // Reads this version's list of packs again.
    virtual void refresh() = 0;
    virtual void install(const QString& name) = 0;
    virtual void cancel(const QString& name) = 0;
    // Takes it out: at once if this usdcc hasn't loaded it, else at the next start.
    virtual void remove(const QString& name) = 0;
    // Takes back a remove() that waits for the next start.
    virtual void keep(const QString& name) = 0;
    // Closes the app, asking what closing asks, and starts it again.
    virtual void restart() = 0;

Q_SIGNALS:
    void changed();
};

}  // namespace usdcc::scripting
