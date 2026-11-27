// Installing an optional pack (src/cpp/app/pack_installer.h, docs/USD_PLUGINS.md §10): from
// archives the tests make in a folder, read as file: URLs, so no network. The embedded
// Python makes the archives, and usdcc's own interpreter unpacks them, as in the app.
#include "python_test_app.h"
#include "test_harness.h"

#include "pack_installer.h"

#include "usdcc/scripting/pack_store.h"
#include "usdcc/scripting/python_runtime.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>

#include <filesystem>
#include <fstream>
#include <string>

using usdcc::app::PackIndexEntry;
using usdcc::app::PackInstaller;
using usdcc::scripting::PackRecord;
using usdcc::scripting::PackStore;
using usdcc::test::pythonTestApp;

namespace {
constexpr const char* kVersion = "26.12.2";
constexpr const char* kPlatform = "test-x64";

std::string pythonPath(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

struct Feed {
    std::filesystem::path top;
    explicit Feed(const std::string& name)
        : top(std::filesystem::temp_directory_path() /
              ("usdcc_pack_install_" + name + "_" + std::to_string(QCoreApplication::applicationPid()))) {
        std::error_code error;
        std::filesystem::remove_all(top, error);
        std::filesystem::create_directories(top / "feed");
    }
    ~Feed() {
        std::error_code error;
        std::filesystem::remove_all(top, error);
    }
    PackStore::Options store() const {
        PackStore::Options options;
        options.root = top / "packs";
        options.version = kVersion;
        options.platform = kPlatform;
        options.libraries = top;
        return options;
    }
    // A zip in the feed holding `files` (name: text), and its index entry.
    PackIndexEntry archive(const std::string& pack, const std::string& files) const {
        const std::filesystem::path file = top / "feed" / (pack + ".zip");
        pythonTestApp().python.runString("import zipfile\nwith zipfile.ZipFile(r'" + pythonPath(file) + "', 'w') as z:\n"
                                         "    for name, text in " + files + ".items():\n        z.writestr(name, text)\n");
        PackIndexEntry entry;
        entry.name = QString::fromStdString(pack);
        entry.title = QString::fromStdString(pack);
        entry.version = QStringLiteral("1.0");
        entry.file = QString::fromStdString(pack + ".zip");
        entry.content = QStringLiteral("content-of-") + entry.name;
        QFile packed(QString::fromStdU16String(file.u16string()));
        if (packed.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = packed.readAll();
            entry.size = bytes.size();
            entry.sha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        }
        entry.installedSize = 100;
        return entry;
    }
    // A pack as its archive should be: one folder, with a manifest.
    PackIndexEntry good(const std::string& pack) const {
        return archive(pack, "{'" + pack + "/usdcc-plugin.json': '{}', '" + pack + "/plugin/usd/plugInfo.json': '{}', '" + pack +
                                 "/bin/data.txt': 'data'}");
    }
    QUrl url() const { return QUrl::fromLocalFile(QString::fromStdU16String((top / "feed").u16string()) + QLatin1Char('/')); }
    std::filesystem::path installed(const std::string& pack, const std::string& version = kVersion) const {
        return PackStore(store()).folder(version) / pack;
    }
    bool nothingLeft() const {
        std::error_code error;
        const std::filesystem::path downloads = PackStore(store()).downloadsFolder();
        return !std::filesystem::exists(downloads, error) || std::filesystem::is_empty(downloads, error);
    }
};

struct Outcome {
    bool finished = false;
    bool downloaded = false;
    bool cancelled = false;
    QString reason;
    bool timedOut = false;
};

// Runs one install to its end. `cancelAtOnce` cancels it right after it started.
Outcome install(const Feed& feed, const PackInstaller::Request& request, bool cancelAtOnce = false) {
    PackInstaller installer(feed.store(), pythonTestApp().python.executable());
    Outcome outcome;
    QEventLoop loop;
    QObject::connect(&installer, &PackInstaller::finished, &loop, [&](const QString&, bool downloaded) {
        outcome.finished = true;
        outcome.downloaded = downloaded;
        loop.quit();
    });
    QObject::connect(&installer, &PackInstaller::failed, &loop, [&](const QString&, const QString& reason, bool cancelled) {
        outcome.reason = reason;
        outcome.cancelled = cancelled;
        loop.quit();
    });
    QTimer::singleShot(60000, &loop, [&]() {
        outcome.timedOut = true;
        loop.quit();
    });
    installer.start(request);
    if (cancelAtOnce) installer.cancel();
    loop.exec();
    return outcome;
}
}  // namespace

USDCC_TEST(pack_install_from_an_archive) {
    Feed feed("good");
    const PackIndexEntry entry = feed.good("alpha");
    const Outcome outcome = install(feed, {entry, feed.url(), QString::fromLatin1(kVersion), {}});
    CHECK(outcome.finished && outcome.downloaded && !outcome.timedOut, "it installs: %s", qPrintable(outcome.reason));
    const std::filesystem::path pack = feed.installed("alpha");
    CHECK(std::filesystem::exists(pack / "usdcc-plugin.json") && std::filesystem::exists(pack / "bin/data.txt"), "its files are in place");
    const auto record = PackStore::readRecord(pack);
    CHECK(record && record->name == "alpha" && record->usdcc == kVersion && record->platform == kPlatform &&
              record->content == "content-of-alpha" && record->installedSize == 100,
          "with the record of what it is and what it was installed for");
    const std::vector<usdcc::scripting::InstalledPack> installed = PackStore(feed.store()).installed();
    CHECK(installed.size() == 1 && installed[0].record.name == "alpha", "so the store lists it");
    CHECK(feed.nothingLeft(), "and nothing is left of the download");

    // Again: the same files are there, so nothing is fetched (the archive is gone).
    std::filesystem::remove(feed.top / "feed" / "alpha.zip");
    const Outcome again = install(feed, {entry, feed.url(), QString::fromLatin1(kVersion), {}});
    CHECK(again.finished && !again.downloaded, "a pack that is there with the same content is done at once: %s", qPrintable(again.reason));
}

USDCC_TEST(pack_install_checks_the_download) {
    Feed feed("checks");
    const QString version = QString::fromLatin1(kVersion);

    PackIndexEntry wrongHash = feed.good("hash");
    wrongHash.sha256 = QByteArray(64, 'a');
    Outcome outcome = install(feed, {wrongHash, feed.url(), version, {}});
    CHECK(!outcome.finished && outcome.reason.contains(QLatin1String("SHA-256")), "another file than the index names is refused: %s",
          qPrintable(outcome.reason));
    CHECK(!std::filesystem::exists(feed.installed("hash")) && feed.nothingLeft(), "and nothing of it is kept");

    PackIndexEntry larger = feed.good("larger");
    larger.size -= 10;
    outcome = install(feed, {larger, feed.url(), version, {}});
    CHECK(!outcome.finished && outcome.reason.contains(QLatin1String("larger")), "a download larger than the index says: %s",
          qPrintable(outcome.reason));

    PackIndexEntry shorter = feed.good("shorter");
    shorter.size += 10;
    outcome = install(feed, {shorter, feed.url(), version, {}});
    CHECK(!outcome.finished && outcome.reason.contains(QLatin1String("ended early")), "one that ends early: %s", qPrintable(outcome.reason));

    PackIndexEntry missing = feed.good("missing");
    std::filesystem::remove(feed.top / "feed" / "missing.zip");
    outcome = install(feed, {missing, feed.url(), version, {}});
    CHECK(!outcome.finished && !outcome.cancelled && !outcome.reason.isEmpty(), "an archive that isn't there: %s", qPrintable(outcome.reason));
    CHECK(PackStore(feed.store()).installed().empty() && feed.nothingLeft(), "none of them left anything in the store");
}

USDCC_TEST(pack_install_refuses_a_bad_archive) {
    Feed feed("bad");
    const QString version = QString::fromLatin1(kVersion);

    // A file that would land beside the pack's folder, in the store or above it.
    const PackIndexEntry escaping =
        feed.archive("escape", "{'escape/usdcc-plugin.json': '{}', '../../escaped.txt': 'out', 'escape/../../../escaped2.txt': 'out'}");
    Outcome outcome = install(feed, {escaping, feed.url(), version, {}});
    CHECK(!outcome.finished, "an archive with a path out of its folder isn't installed: %s", qPrintable(outcome.reason));
    bool escaped = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(feed.top)) {
        escaped = escaped || entry.path().filename().string().rfind("escaped", 0) == 0;
    }
    CHECK(!escaped && !std::filesystem::exists(feed.top.parent_path() / "escaped.txt"), "and nothing was written outside");

    const PackIndexEntry noFolder = feed.archive("flat", "{'usdcc-plugin.json': '{}', 'plugin/usd/plugInfo.json': '{}'}");
    outcome = install(feed, {noFolder, feed.url(), version, {}});
    CHECK(!outcome.finished && outcome.reason.contains(QLatin1String("isn't a pack")), "one that isn't one folder of the pack's name: %s",
          qPrintable(outcome.reason));

    const PackIndexEntry noManifest = feed.archive("bare", "{'bare/readme.txt': 'hello'}");
    outcome = install(feed, {noManifest, feed.url(), version, {}});
    CHECK(!outcome.finished && outcome.reason.contains(QLatin1String("isn't a pack")), "one without a manifest: %s", qPrintable(outcome.reason));

    PackIndexEntry notAnArchive = feed.good("garbage");
    {
        std::ofstream file(feed.top / "feed" / "garbage.zip", std::ios::binary | std::ios::trunc);
        file << "this is no archive";
    }
    QFile written(QString::fromStdU16String((feed.top / "feed" / "garbage.zip").u16string()));
    written.open(QIODevice::ReadOnly);
    const QByteArray bytes = written.readAll();
    notAnArchive.size = bytes.size();
    notAnArchive.sha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    outcome = install(feed, {notAnArchive, feed.url(), version, {}});
    CHECK(!outcome.finished && outcome.reason.contains(QLatin1String("unpacked")), "a file that is no archive: %s", qPrintable(outcome.reason));
    CHECK(PackStore(feed.store()).installed().empty() && feed.nothingLeft(), "none of them left anything in the store");
}

USDCC_TEST(pack_install_cancelled_changes_nothing) {
    Feed feed("cancel");
    const PackIndexEntry entry = feed.good("stopped");
    const Outcome outcome = install(feed, {entry, feed.url(), QString::fromLatin1(kVersion), {}}, true);
    CHECK(!outcome.finished && outcome.cancelled && !outcome.timedOut, "a cancelled install says so");
    CHECK(!std::filesystem::exists(feed.installed("stopped")) && feed.nothingLeft(), "and leaves the store as it was");
    const Outcome after = install(feed, {entry, feed.url(), QString::fromLatin1(kVersion), {}});
    CHECK(after.finished, "after which it installs as if nothing had been: %s", qPrintable(after.reason));
}

USDCC_TEST(pack_install_copies_the_same_content) {
    Feed feed("copy");
    const PackIndexEntry entry = feed.good("same");
    const QString version = QString::fromLatin1(kVersion);
    CHECK(install(feed, {entry, feed.url(), version, {}}).finished, "the pack for this version");
    // What usdcc itself puts into an installed pack.
    { std::ofstream(feed.installed("same") / ".in-use") << ""; }

    // The next version's index names the same content: no archive is asked for.
    std::filesystem::remove(feed.top / "feed" / "same.zip");
    Outcome outcome = install(feed, {entry, feed.url(), QStringLiteral("26.12.3"), feed.installed("same")});
    CHECK(outcome.finished && !outcome.downloaded, "it is copied, not downloaded: %s", qPrintable(outcome.reason));
    const std::filesystem::path copy = feed.installed("same", "26.12.3");
    CHECK(std::filesystem::exists(copy / "usdcc-plugin.json") && std::filesystem::exists(copy / "bin/data.txt"), "with its files");
    CHECK(!std::filesystem::exists(copy / ".in-use"), "without what belongs to the running usdcc");
    const auto record = PackStore::readRecord(copy);
    CHECK(record && record->usdcc == "26.12.3" && record->content == "content-of-same", "and a record for the new version");
    CHECK(std::filesystem::exists(feed.installed("same") / "usdcc-plugin.json"), "this version's is untouched");

    // Another content: the copy isn't taken, and the archive is needed.
    PackIndexEntry changed = entry;
    changed.content = QStringLiteral("something-else");
    outcome = install(feed, {changed, feed.url(), QStringLiteral("26.12.4"), feed.installed("same")});
    CHECK(!outcome.finished && !std::filesystem::exists(feed.installed("same", "26.12.4")),
          "a pack whose content differs is downloaded, here from nowhere: %s", qPrintable(outcome.reason));
}
