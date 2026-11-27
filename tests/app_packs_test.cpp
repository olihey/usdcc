// The optional packs in the app (src/cpp/app/app_packs.h, docs/USD_PLUGINS.md §10): what
// View > Plugins > Packs lists and does, what a new version of usdcc means for the chosen
// packs, and the unattended install. Against a folder of indexes and archives the tests
// make (USDCC_PACKS_URL), so no network and no release.
#include "python_test_app.h"
#include "test_harness.h"

#include "app_packs.h"

#include "usdcc/scripting/pack_store.h"
#include "usdcc/scripting/python_runtime.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using usdcc::app::AppPacks;
using usdcc::scripting::PackListing;
using usdcc::scripting::PackStore;
using usdcc::test::pythonTestApp;

namespace {
constexpr const char* kPlatform = "test-x64";

std::string pythonPath(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// A folder as a release's downloads: an index per version, and the packs' archives.
struct Releases {
    std::filesystem::path top;
    explicit Releases(const std::string& name)
        : top(std::filesystem::temp_directory_path() /
              ("usdcc_app_packs_" + name + "_" + std::to_string(QCoreApplication::applicationPid()))) {
        std::error_code error;
        std::filesystem::remove_all(top, error);
        std::filesystem::create_directories(top / "feed");
        QStandardPaths::setTestModeEnabled(true);
        qputenv("USDCC_PACKS_URL", QString::fromStdU16String((top / "feed").u16string()).toUtf8());
    }
    ~Releases() {
        qunsetenv("USDCC_PACKS_URL");
        QStandardPaths::setTestModeEnabled(false);
        std::error_code error;
        std::filesystem::remove_all(top, error);
    }
    PackStore::Options store(const std::string& version) const {
        PackStore::Options options;
        options.root = top / "packs";
        options.version = version;
        options.platform = kPlatform;
        options.libraries = top;
        return options;
    }
    std::filesystem::path archive(const std::string& version, const std::string& pack) const {
        return top / "feed" / ("usdcc-" + version + "-" + kPlatform + "-pack-" + pack + ".zip");
    }
    // `version` offers these packs, as name and content; an archive each.
    void release(const std::string& version, const std::vector<std::pair<std::string, std::string>>& packs) const {
        QJsonArray entries;
        for (const auto& [name, content] : packs) {
            const std::filesystem::path file = archive(version, name);
            pythonTestApp().python.runString("import zipfile\nwith zipfile.ZipFile(r'" + pythonPath(file) + "', 'w') as z:\n"
                                             "    z.writestr('" + name + "/usdcc-plugin.json', '{}')\n"
                                             "    z.writestr('" + name + "/data.txt', '" + content + "')\n");
            QFile packed(QString::fromStdU16String(file.u16string()));
            packed.open(QIODevice::ReadOnly);
            const QByteArray bytes = packed.readAll();
            entries.append(QJsonObject{{"name", QString::fromStdString(name)},
                                       {"title", QString::fromStdString("The " + name)},
                                       {"description", "For the tests"},
                                       {"version", "1.0"},
                                       {"file", QString::fromStdU16String(file.filename().u16string())},
                                       {"size", static_cast<double>(bytes.size())},
                                       {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
                                       {"installed_size", 50},
                                       {"content", QString::fromStdString(content)}});
        }
        const QJsonObject index{{"schema", 1}, {"usdcc", QString::fromStdString(version)}, {"platform", kPlatform}, {"packs", entries}};
        QFile file(QString::fromStdU16String((top / "feed" / ("usdcc-" + version + "-" + kPlatform + "-packs.json")).u16string()));
        file.open(QIODevice::WriteOnly);
        file.write(QJsonDocument(index).toJson());
    }
};

AppPacks::Options options(PackStore& store) {
    AppPacks::Options options;
    options.store = &store;
    options.python = pythonTestApp().python.executable();
    return options;
}

// Runs the event loop until `done` says so; false when it took too long.
bool waitFor(const std::function<bool()>& done, int milliseconds = 60000) {
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (done()) loop.quit();
    });
    poll.start(10);
    bool late = false;
    QTimer::singleShot(milliseconds, &loop, [&]() {
        late = true;
        loop.quit();
    });
    if (!done()) loop.exec();
    return !late;
}

PackListing::State stateOf(const AppPacks& packs, const char* name) {
    for (const PackListing& pack : packs.packs()) {
        if (pack.name == QLatin1String(name)) return pack.state;
    }
    return PackListing::State::NotAvailable;
}

bool listed(const AppPacks& packs, const char* name) {
    for (const PackListing& pack : packs.packs()) {
        if (pack.name == QLatin1String(name)) return true;
    }
    return false;
}
}  // namespace

USDCC_TEST(app_packs_list_install_and_remove) {
    Releases releases("list");
    releases.release("26.12.1", {{"alpha", "a1"}, {"beta", "b1"}});
    PackStore store(releases.store("26.12.1"));
    {
        AppPacks packs(options(store));
        CHECK(packs.packs().empty(), "nothing is listed, or asked for, before the list is read");
        int changes = 0;
        QObject::connect(&packs, &AppPacks::changed, &packs, [&changes]() { ++changes; });
        packs.refresh();
        CHECK(waitFor([&]() { return packs.packs().size() == 2; }), "then what the release offers");
        const std::vector<PackListing> list = packs.packs();
        CHECK(list.size() == 2 && list[0].title == QLatin1String("The alpha") && list[0].description == QLatin1String("For the tests") &&
                  list[0].state == PackListing::State::NotInstalled && list[0].downloadSize > 0 && list[0].installedSize == 50,
              "with what the panel shows of each");
        CHECK(packs.note().isEmpty() && changes > 0, "and no note when there is nothing to say");

        packs.install(QStringLiteral("alpha"));
        CHECK(stateOf(packs, "alpha") == PackListing::State::Downloading, "an install shows at once");
        CHECK(waitFor([&]() { return stateOf(packs, "alpha") == PackListing::State::RestartToUse; }), "and ends waiting for a restart");
        CHECK(store.wanted() == std::vector<std::string>{"alpha"}, "the pack is one the user chose from then on");
        CHECK(store.installed().size() == 1 && stateOf(packs, "beta") == PackListing::State::NotInstalled, "the other isn't touched");

        // Not loaded by this usdcc yet, so it can go at once.
        packs.remove(QStringLiteral("alpha"));
        CHECK(stateOf(packs, "alpha") == PackListing::State::NotInstalled && store.installed().empty() && store.wanted().empty(),
              "removed before any restart, it is gone and no longer chosen");
        packs.install(QStringLiteral("alpha"));
        CHECK(waitFor([&]() { return stateOf(packs, "alpha") == PackListing::State::RestartToUse; }), "and installs again");
    }

    // The next start.
    store.prepare();
    AppPacks packs(options(store));
    CHECK(stateOf(packs, "alpha") == PackListing::State::Installed, "after a restart it is installed, listed even before the list is read");
    packs.remove(QStringLiteral("alpha"));
    CHECK(stateOf(packs, "alpha") == PackListing::State::RemovedAtRestart && store.wanted().empty() && store.installed().size() == 1,
          "a loaded pack is only marked: it goes at the next start, and isn't chosen any more");
    packs.keep(QStringLiteral("alpha"));
    CHECK(stateOf(packs, "alpha") == PackListing::State::Installed && store.wanted() == std::vector<std::string>{"alpha"},
          "Keep takes that back");
    packs.remove(QStringLiteral("alpha"));
    packs.install(QStringLiteral("alpha"));
    CHECK(stateOf(packs, "alpha") == PackListing::State::Installed, "as does Install on a pack that is only marked");
}

USDCC_TEST(app_packs_say_why_an_install_failed) {
    Releases releases("fail");
    releases.release("26.12.1", {{"alpha", "a1"}});
    PackStore store(releases.store("26.12.1"));
    AppPacks packs(options(store));
    packs.refresh();
    CHECK(waitFor([&]() { return packs.packs().size() == 1; }), "the list");
    // The archive changes after the index was read.
    {
        QFile file(QString::fromStdU16String(releases.archive("26.12.1", "alpha").u16string()));
        file.open(QIODevice::Append);
        file.write("x");
    }
    packs.install(QStringLiteral("alpha"));
    CHECK(waitFor([&]() { return !packs.packs()[0].problem.isEmpty(); }), "a failed install leaves its reason with the pack");
    CHECK(stateOf(packs, "alpha") == PackListing::State::NotInstalled && store.installed().empty() && store.wanted().empty(),
          "which is still not installed, and not chosen: %s", qPrintable(packs.packs()[0].problem));
}

USDCC_TEST(app_packs_for_an_update_of_usdcc) {
    Releases releases("update");
    releases.release("26.12.1", {{"same", "s1"}, {"changed", "c1"}, {"dropped", "d1"}});
    releases.release("26.12.2", {{"same", "s1"}, {"changed", "c2"}});
    PackStore store(releases.store("26.12.1"));
    AppPacks packs(options(store));
    CHECK(!packs.keepsPacks(), "with no pack chosen, an update has nothing to take along");
    packs.refresh();
    waitFor([&]() { return packs.packs().size() == 3; });
    for (const char* name : {"same", "changed", "dropped"}) packs.install(QString::fromLatin1(name));
    CHECK(waitFor([&]() { return store.installed().size() == 3; }), "three packs for this version");
    CHECK(packs.keepsPacks(), "now it has");

    // What 26.12.2 means for them, before anything is downloaded.
    std::optional<AppPacks::ForVersion> next;
    packs.checkFor(QStringLiteral("26.12.2"), [&next](const AppPacks::ForVersion& result) { next = result; });
    CHECK(waitFor([&]() { return next.has_value(); }), "the new version's list is read");
    QFile changedArchive(QString::fromStdU16String(releases.archive("26.12.2", "changed").u16string()));
    CHECK(next && next->error.isEmpty() && next->missing == QStringList{QStringLiteral("The dropped")}, "the pack it doesn't offer is named");
    CHECK(next && next->fetch.size() == 2 && next->downloadBytes == changedArchive.size(),
          "of the two it offers, only the one whose content differs counts as a download: %lld bytes",
          static_cast<long long>(next ? next->downloadBytes : -1));

    // A version with no list at all: every chosen pack is missing, and that isn't an error.
    std::optional<AppPacks::ForVersion> none;
    packs.checkFor(QStringLiteral("26.12.9"), [&none](const AppPacks::ForVersion& result) { none = result; });
    CHECK(waitFor([&]() { return none.has_value(); }) && none->missing.size() == 3 && none->fetch.isEmpty() && none->error.isEmpty(),
          "a release without a list of packs offers none");

    // Fetched beside this version's. The archive of the unchanged pack is gone: it must be copied.
    std::filesystem::remove(releases.archive("26.12.2", "same"));
    bool done = false;
    QStringList failed;
    packs.fetchFor(QStringLiteral("26.12.2"), [&](const QStringList& names, const QString&) {
        failed = names;
        done = true;
    });
    CHECK(waitFor([&]() { return done; }) && failed.isEmpty(), "the packs for the new version are fetched: %s", qPrintable(failed.join(',')));
    const PackStore after(releases.store("26.12.2"));
    const auto installed = after.installed();
    CHECK(installed.size() == 2 && installed[0].record.name == "changed" && installed[0].record.content == "c2" &&
              installed[1].record.name == "same" && installed[1].record.content == "s1",
          "into its own folder: one downloaded, one copied");
    CHECK(store.installed().size() == 3 && stateOf(packs, "same") == PackListing::State::RestartToUse,
          "this version's packs stay as they were, in use until the restart");
    CHECK(store.wanted().size() == 3, "and the chosen packs stay chosen, the missing one too: it is fetched once it is offered");

    // An update whose pack can't be fetched says which.
    releases.release("26.12.3", {{"changed", "c3"}});
    std::filesystem::remove(releases.archive("26.12.3", "changed"));
    done = false;
    QString reason;
    packs.fetchFor(QStringLiteral("26.12.3"), [&](const QStringList& names, const QString& why) {
        failed = names;
        reason = why;
        done = true;
    });
    CHECK(waitFor([&]() { return done; }) && failed == QStringList{QStringLiteral("The changed")} && !reason.isEmpty(),
          "a pack that couldn't be fetched is named, with why: %s", qPrintable(reason));
}

USDCC_TEST(app_packs_fetch_what_a_new_version_lacks) {
    Releases releases("lacks");
    releases.release("26.12.2", {{"alpha", "a2"}});
    // The user chose two packs with an earlier version; this one is a new copy.
    PackStore store(releases.store("26.12.2"));
    store.setWanted("alpha", true);
    store.setWanted("gone", true);
    AppPacks packs(options(store));
    packs.fetchChosen();
    CHECK(waitFor([&]() { return store.installed().size() == 1; }), "the chosen pack this version's release offers is fetched");
    CHECK(waitFor([&]() { return stateOf(packs, "alpha") == PackListing::State::RestartToUse; }), "and waits for a restart");
    CHECK(listed(packs, "gone") && stateOf(packs, "gone") == PackListing::State::NotAvailable,
          "the one it doesn't offer is listed as not for this version");
    packs.remove(QStringLiteral("gone"));
    CHECK(!listed(packs, "gone") && store.wanted() == std::vector<std::string>{"alpha"}, "until the user lets go of it");
}

USDCC_TEST(app_packs_unattended) {
    Releases releases("cli");
    releases.release("26.12.1", {{"alpha", "a1"}, {"beta", "b1"}});
    PackStore store(releases.store("26.12.1"));
    QStringList lines;
    const auto report = [&lines](const QString& line) { lines << line; };
    {
        AppPacks packs(options(store));
        CHECK(packs.installNow({QStringLiteral("alpha")}, report) == 0 && store.installed().size() == 1, "--install-packs alpha: %s",
              qPrintable(lines.join(" | ")));
        CHECK(packs.installNow({QStringLiteral("alpha"), QStringLiteral("nothing")}, report) == 2, "one that isn't offered: 2");
        store.setWanted("beta", true);
        CHECK(packs.installNow({}, report) == 0 && store.installed().size() == 2, "no names: the chosen ones");
        CHECK(packs.removeNow({QStringLiteral("alpha")}, report) == 0 && store.installed().size() == 1 &&
                  store.wanted() == std::vector<std::string>{"beta"},
              "--remove-packs alpha");
        CHECK(packs.removeNow({QStringLiteral("alpha")}, report) == 2, "one that isn't installed: 2");
    }
    std::filesystem::remove(releases.archive("26.12.1", "alpha"));
    AppPacks packs(options(store));
    lines.clear();
    CHECK(packs.installNow({QStringLiteral("alpha")}, report) == 1 && lines.join(' ').contains(QLatin1String("wasn't installed")),
          "a failed install: 1, saying why: %s", qPrintable(lines.join(" | ")));
}
