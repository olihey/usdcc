// The packs usdcc downloads (docs/USD_PLUGINS.md §10): the store in the user's data folder,
// what a start does to it, and a pack found as a plugin. No network and no archive here:
// each test lays its packs out by hand.
#include "test_harness.h"

#include "usdcc/scripting/in_use_marker.h"
#include "usdcc/scripting/pack_store.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/ui/settings.h"

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <QString>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using usdcc::scripting::comparePackVersions;
using usdcc::scripting::InstalledPack;
using usdcc::scripting::InUseMarker;
using usdcc::scripting::PackRecord;
using usdcc::scripting::PackStore;
using usdcc::scripting::PluginInfo;
using usdcc::scripting::PluginManager;
using usdcc::scripting::PluginState;

namespace {
constexpr const char* kVersion = "26.12.2";
constexpr const char* kPlatform = "test-x64";

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A store in a folder of its own, and usdcc.ini out of the user's way.
struct Store {
    std::filesystem::path top;
    explicit Store(const std::string& name)
        : top(std::filesystem::temp_directory_path() /
              ("usdcc_packs_" + name + "_" + std::to_string(QCoreApplication::applicationPid()))) {
        std::error_code error;
        std::filesystem::remove_all(top, error);
        std::filesystem::create_directories(top / "app-lib");
        QStandardPaths::setTestModeEnabled(true);
        QFile::remove(usdcc::ui::userConfigFolder() + "/usdcc.ini");
    }
    ~Store() {
        QFile::remove(usdcc::ui::userConfigFolder() + "/usdcc.ini");
        QStandardPaths::setTestModeEnabled(false);
        std::error_code error;
        std::filesystem::remove_all(top, error);
    }
    PackStore::Options options(const std::string& version = kVersion) const {
        PackStore::Options options;
        options.root = top / "packs";
        options.version = version;
        options.platform = kPlatform;
        options.libraries = top / "app-lib";
        return options;
    }
    void write(const std::filesystem::path& file, const std::string& text) const {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
    // A pack as an install leaves it; `finished` false stops before the record.
    std::filesystem::path pack(const std::string& name, const std::string& version = kVersion, bool finished = true,
                               const std::string& usd = {}) const {
        const std::filesystem::path folder = PackStore(options(version)).folder() / name;
        write(folder / "usdcc-plugin.json", "{\"id\": \"org.usdcc.pack." + name + "\", \"name\": \"" + name +
                                                "\", \"version\": \"1.0\", \"api\": 1, \"usd\": [\"plugin/usd\"]}");
        write(folder / "plugin/usd/plugInfo.json", usd.empty() ? "{\"Plugins\": []}" : usd);
        if (finished) {
            PackRecord record;
            record.name = name;
            record.title = name;
            record.version = "1.0";
            record.usdcc = version;
            record.platform = kPlatform;
            record.content = "abc";
            PackStore::writeRecord(folder, record);
        }
        return folder;
    }
};

std::vector<std::string> names(const std::vector<InstalledPack>& packs) {
    std::vector<std::string> list;
    for (const InstalledPack& pack : packs) list.push_back(pack.record.name);
    return list;
}

const InstalledPack* find(const std::vector<InstalledPack>& packs, const std::string& name) {
    const auto found = std::find_if(packs.begin(), packs.end(), [&name](const InstalledPack& pack) { return pack.record.name == name; });
    return found == packs.end() ? nullptr : &*found;
}
}  // namespace

USDCC_TEST(pack_versions_in_order) {
    CHECK(comparePackVersions("26.12.1", "26.12.2") == -1, "the counter");
    CHECK(comparePackVersions("26.12.10", "26.12.9") == 1, "as numbers, not text");
    CHECK(comparePackVersions("27.1.1", "26.12.9") == 1, "the year first");
    CHECK(comparePackVersions("26.12.1", "v26.12.1+build") == 0, "a leading v and a +suffix don't count");
    CHECK(comparePackVersions("26.12.1-beta.1", "26.12.1") == -1, "a beta comes before its release");
    CHECK(comparePackVersions("26.12.1-beta.2", "26.12.1-beta.10") == -1, "betas by number");
    CHECK(!comparePackVersions("26.12", "26.12.1"), "two numbers aren't a version");
    CHECK(!comparePackVersions("0.0.0-dev", "nonsense"), "nor is text");
}

USDCC_TEST(pack_store_lists_finished_packs) {
    Store sandbox("list");
    PackStore store(sandbox.options());
    CHECK(store.installed().empty(), "nothing in a store that isn't there yet");
    CHECK(store.folder().filename() == std::filesystem::path(std::string(kVersion) + "-" + kPlatform), "a folder per version and platform");

    sandbox.pack("beta");
    sandbox.pack("alpha");
    sandbox.pack("half", kVersion, false);
    sandbox.pack("other", "26.12.3");
    // A record that names another pack: a folder someone copied and renamed.
    const std::filesystem::path renamed = sandbox.pack("renamed");
    PackRecord wrong;
    wrong.name = "something-else";
    PackStore::writeRecord(renamed, wrong);

    CHECK(names(store.installed()) == (std::vector<std::string>{"alpha", "beta"}),
          "the finished packs of this version, by name; not one without its record, nor a renamed folder");
    CHECK(names(store.installed("26.12.3")) == std::vector<std::string>{"other"}, "another version's are its own");
    const std::optional<PackRecord> record = PackStore::readRecord(store.folder() / "alpha");
    CHECK(record && record->usdcc == kVersion && record->platform == kPlatform && record->content == "abc",
          "the record says what it was packed for and what is in it");
}

USDCC_TEST(pack_store_remembers_what_is_wanted) {
    Store sandbox("wanted");
    PackStore store(sandbox.options());
    CHECK(store.wanted().empty(), "nothing wanted at first");
    CHECK(store.setWanted("moonray", true) && store.setWanted("cycles", true) && store.setWanted("moonray", true),
          "packs are added, once each");
    CHECK(store.wanted() == (std::vector<std::string>{"cycles", "moonray"}), "and listed in order");
    CHECK(PackStore(sandbox.options("27.1.1")).wanted() == store.wanted(), "for every version: an update keeps them");
    CHECK(store.setWanted("cycles", false) && store.wanted() == std::vector<std::string>{"moonray"}, "and taken out again");
    CHECK(!store.setWanted("../elsewhere", true) && !store.setWanted("", true) && !store.setWanted(".hidden", true),
          "a name is one folder's, nothing else");
}

USDCC_TEST(pack_store_prepares_packs) {
    Store sandbox("prepare");
    const std::filesystem::path good = sandbox.pack("good");
    const std::filesystem::path foreign = sandbox.pack("foreign");
    PackRecord other = *PackStore::readRecord(foreign);
    other.usdcc = "26.12.1";
    PackStore::writeRecord(foreign, other);
    sandbox.write(good / "bin/readme.txt", "");

    PackStore store(sandbox.options());
    const std::vector<InstalledPack> prepared = store.prepare();
    CHECK(names(prepared) == (std::vector<std::string>{"foreign", "good"}), "both are returned");
    CHECK(find(prepared, "good") && find(prepared, "good")->problem.empty(), "one made for this usdcc is loadable: %s",
          find(prepared, "good") ? find(prepared, "good")->problem.c_str() : "missing");
    CHECK(find(prepared, "foreign") && contains(find(prepared, "foreign")->problem, "26.12.1"),
          "one made for another usdcc isn't, and says which");
    CHECK(store.isInUseHere("good") && !store.isInUseHere("foreign"), "and only the first counts as loaded here");
    CHECK(std::filesystem::exists(good / ".in-use") && !std::filesystem::exists(foreign / ".in-use"), "which is marked in use");
#if defined(_WIN32)
    std::filesystem::path bin = good / "bin";
    bin.make_preferred();
    CHECK(qEnvironmentVariable("PATH").contains(QString::fromStdWString(bin.wstring()), Qt::CaseInsensitive),
          "Windows: its bin is on this process's PATH, where the libraries of its plugins find its DLLs");
    const QString before = qEnvironmentVariable("PATH");
    PackStore(sandbox.options()).prepare();
    CHECK(qEnvironmentVariable("PATH") == before, "once");
#else
    std::error_code error;
    CHECK(std::filesystem::is_symlink(good / "lib") && std::filesystem::read_symlink(good / "lib", error) == sandbox.top / "app-lib",
          "its lib is a link to the app's libraries, where its files' RUNPATHs lead");
    // The app somewhere else at the next start, as an AppImage is.
    std::filesystem::create_directories(sandbox.top / "moved-lib");
    PackStore::Options moved = sandbox.options();
    moved.libraries = sandbox.top / "moved-lib";
    PackStore second(moved);
    second.prepare();
    CHECK(std::filesystem::read_symlink(good / "lib", error) == sandbox.top / "app-lib",
          "while a usdcc uses the pack, the link stays where that one loads from");
#endif
}

#if !defined(_WIN32)
USDCC_TEST(pack_store_links_to_the_app_again) {
    Store sandbox("relink");
    const std::filesystem::path good = sandbox.pack("good");
    std::error_code error;
    { PackStore(sandbox.options()).prepare(); }
    std::filesystem::create_directories(sandbox.top / "moved-lib");
    PackStore::Options moved = sandbox.options();
    moved.libraries = sandbox.top / "moved-lib";
    PackStore store(moved);
    const std::vector<InstalledPack> prepared = store.prepare();
    CHECK(std::filesystem::read_symlink(good / "lib", error) == sandbox.top / "moved-lib",
          "with no usdcc using it, the link follows the app to where it is now");
    CHECK(find(prepared, "good") && find(prepared, "good")->problem.empty(), "and the pack is loadable");

    // Without the app's libraries there is nothing to link to, and the pack must not load.
    PackStore::Options none = sandbox.options();
    none.libraries = sandbox.top / "nowhere";
    const std::filesystem::path second = sandbox.pack("second");
    PackStore lost(none);
    const std::vector<InstalledPack> refused = lost.prepare();
    CHECK(find(refused, "second") && !find(refused, "second")->problem.empty(), "a pack whose link can't be made is refused");
    (void)second;
}
#endif

USDCC_TEST(pack_store_removes_packs_at_a_start) {
    Store sandbox("remove");
    const std::filesystem::path going = sandbox.pack("going");
    const std::filesystem::path held = sandbox.pack("held");
    const std::filesystem::path fresh = sandbox.pack("fresh");
    {
        PackStore store(sandbox.options());
        CHECK(store.markRemoved("going", true) && store.markRemoved("held", true), "packs are marked for removal");
        CHECK(!store.markRemoved("nothing", true), "not one that isn't there");
        CHECK(find(store.installed(), "going")->removed && !find(store.installed(), "fresh")->removed, "and listed as marked");
        CHECK(store.markRemoved("going", false) && !find(store.installed(), "going")->removed, "Keep takes the mark back");
        store.markRemoved("going", true);

        std::string error;
        CHECK(store.removeNow("fresh", &error) && !std::filesystem::exists(fresh), "one this usdcc hasn't loaded goes at once: %s",
              error.c_str());
    }
    {
        // Another usdcc still runs with `held`.
        const InUseMarker other(held);
        PackStore store(sandbox.options());
        const std::vector<InstalledPack> prepared = store.prepare();
        CHECK(!std::filesystem::exists(going), "a marked pack nothing uses is deleted at the start");
        CHECK(std::filesystem::exists(held) && find(prepared, "held") && find(prepared, "held")->removed,
              "one another usdcc uses stays, still marked");
        CHECK(!store.isInUseHere("held"), "and isn't loaded here");
        std::string error;
        CHECK(!store.removeNow("held", &error) && !error.empty(), "nor removed at once: %s", error.c_str());
    }
    {
        PackStore store(sandbox.options());
        store.prepare();
        CHECK(!std::filesystem::exists(held), "it goes at the start after that usdcc quit");
    }
    {
        const std::filesystem::path loaded = sandbox.pack("loaded");
        PackStore store(sandbox.options());
        store.prepare();
        std::string error;
        CHECK(!store.removeNow("loaded", &error) && std::filesystem::exists(loaded), "a pack this usdcc loaded can't go at once");
    }
}

USDCC_TEST(pack_store_removes_older_versions) {
    Store sandbox("versions");
    const std::filesystem::path older = sandbox.pack("a", "26.12.1");
    const std::filesystem::path inUse = sandbox.pack("b", "26.11.9");
    const std::filesystem::path newer = sandbox.pack("a", "26.12.3");
    const std::filesystem::path current = sandbox.pack("a");
    std::filesystem::create_directories(sandbox.top / "packs/.downloads");
    std::filesystem::create_directories(sandbox.top / "packs/26.12.1-other-platform/a");
    {
        const InUseMarker other(inUse);
        PackStore store(sandbox.options());
        store.prepare();
        CHECK(!std::filesystem::exists(older.parent_path()), "an older version's packs are deleted: the update left them behind");
        CHECK(std::filesystem::exists(inUse), "not while that version still runs");
        CHECK(std::filesystem::exists(newer), "a newer version's stay: fetched for an update not installed yet");
        CHECK(std::filesystem::exists(current), "and this version's");
        CHECK(std::filesystem::exists(sandbox.top / "packs/.downloads") &&
                  std::filesystem::exists(sandbox.top / "packs/26.12.1-other-platform/a"),
              "nothing else in the store is touched");
    }
    PackStore store(sandbox.options());
    store.prepare();
    CHECK(!std::filesystem::exists(inUse.parent_path()), "it goes once that version has quit");
}

USDCC_TEST(pack_is_found_as_a_plugin) {
    Store sandbox("plugin");
    const char* usd = R"({"Plugins": [{"Name": "%1", "Type": "resource", "Root": ".", "ResourcePath": ".", "Info": {}}]})";
    const auto plugInfo = [usd](const char* name) { return QString::fromLatin1(usd).arg(QLatin1String(name)).toStdString(); };
    sandbox.pack("ready", kVersion, true, plugInfo("usdccTestPackReady"));
    sandbox.pack("half", kVersion, false, plugInfo("usdccTestPackHalf"));
    sandbox.pack("gone", kVersion, true, plugInfo("usdccTestPackGone"));
    sandbox.pack("broken", kVersion, true, plugInfo("usdccTestPackBroken"));

    PackStore store(sandbox.options());
    PluginManager::Options options;
    options.packsFolder = store.folder();
    options.folders = {store.folder()};
    options.packsRemoved = {"gone"};
    options.packsRefused = {{"broken", "Its libraries can't be found."}};
    PluginManager manager(options);
    manager.scan();

    const PluginInfo* ready = manager.find("org.usdcc.pack.ready");
    CHECK(ready && ready->pack && ready->state == PluginState::Ready, "a finished pack is a plugin, known as a pack");
    CHECK(PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("usdccTestPackReady"), "whose USD folder is registered by scan()");
    CHECK(!manager.find("org.usdcc.pack.half") && !PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("usdccTestPackHalf"),
          "one whose install didn't finish isn't there at all");
    CHECK(!manager.find("org.usdcc.pack.gone") && !PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("usdccTestPackGone"),
          "nor one marked for removal");
    const PluginInfo* broken = manager.find("org.usdcc.pack.broken");
    CHECK(broken && broken->state == PluginState::Refused && contains(broken->reason, "libraries"),
          "one that couldn't be made loadable is listed, refused, with why");
    CHECK(!PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("usdccTestPackBroken"), "and none of it is registered");

    // The same folders as an ordinary plugin folder: no pack.json is asked for.
    PluginManager::Options plain;
    plain.folders = {store.folder()};
    PluginManager ordinary(plain);
    ordinary.scan();
    CHECK(ordinary.find("org.usdcc.pack.half") && !ordinary.find("org.usdcc.pack.half")->pack,
          "only the packs folder is held to it");
}

USDCC_TEST(pack_folder_is_in_the_default_search_order) {
    Store sandbox("order");
    const PluginManager::Options options = PluginManager::defaultOptions({}, true);
    const auto position = [&options](const std::filesystem::path& folder) {
        return int(std::find(options.folders.begin(), options.folders.end(), folder) - options.folders.begin());
    };
    CHECK(!options.packsFolder.empty(), "this platform has a folder of packs");
    CHECK(options.packsFolder == PackStore(PackStore::defaultOptions()).folder(), "the store's, for this version");
    CHECK(position(PluginManager::userFolder()) < position(options.packsFolder) &&
              position(options.packsFolder) < position(PluginManager::shippedFolder()),
          "after the user's own plugins, before the shipped ones");
}
