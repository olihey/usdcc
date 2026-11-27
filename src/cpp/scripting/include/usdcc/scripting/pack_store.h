#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace usdcc::scripting {

class InUseMarker;

// The optional packs usdcc downloads (docs/USD_PLUGINS.md §10): plugins too large or
// too special to ship, MoonRay and Cycles first. They live in the user's data folder,
// never in the app's own, one folder per usdcc version, since a pack is built for
// exactly one:
//
//   <root>/wanted.json                 the packs the user installed, and those they
//                                      removed; kept over updates
//   <root>/26.12.1-linux-x64/moonray/  a plugin folder (usdcc-plugin.json), laid out
//                                      as an install: plugin/usd/..., bin/ on Windows,
//                                      and pack.json, .in-use
//   <root>/26.12.2-linux-x64/moonray/  fetched before the update to 26.12.2
//   <root>/.downloads/                 archives, and packs being unpacked
//
// This version's folder is one of the plugin manager's (PluginManager::Options), so a
// pack is found as any plugin is and its USD folders are registered before the first
// stage. A running app must never see a pack change: installing renames a finished
// folder into place, removing waits for a start at which no usdcc holds its .in-use
// (in_use_marker.h), and both take effect at the next start.

// pack.json, in a pack's folder: what was installed.
struct PackRecord {
    std::string name;      // "moonray": the folder's name, and what wanted.json lists
    std::string title;     // "MoonRay"
    std::string version;   // the pack's own, as "2026.29.1"
    std::string usdcc;     // the usdcc version it was packed for
    std::string platform;  // as PackStore::platformName()
    // A hash of its files, from the release's index: two releases' packs with one
    // `content` are the same files, so an update copies instead of downloading.
    std::string content;
    std::int64_t installedSize = 0;  // bytes, from the index
};

struct InstalledPack {
    PackRecord record;
    std::filesystem::path folder;
    bool removed = false;  // marked by markRemoved(): goes at a later start
    std::string problem;   // why this app can't load it (prepare()); empty when it can
};

// Orders two versions as Semantic Versioning does, as the app's updates do: -1, 0 or
// 1. "26.12.1-beta.1" comes before "26.12.1". Nothing when one isn't MAJOR.MINOR.PATCH.
std::optional<int> comparePackVersions(const std::string& a, const std::string& b);

class PackStore {
public:
    struct Options {
        std::filesystem::path root;       // <user data>/packs
        std::string version;              // this usdcc's
        std::string platform;             // as platformName()
        // The folder the app's own libraries are in, which a pack's lib is linked to
        // on Linux and macOS: its files' RUNPATHs lead there, as in an install.
        std::filesystem::path libraries;
    };

    // The user's data folder, core::version() (for a development build, the release it
    // follows), this platform and the folder OpenUSD's libraries were loaded from.
    // Needs the Qt application object.
    static Options defaultOptions();
    // What a release's files call this platform: "windows-x64", "linux-arm64",
    // "macos-arm64". Empty where no release is made.
    static std::string platformName();

    explicit PackStore(Options options);
    ~PackStore();

    const Options& options() const { return m_options; }
    // This version's folder of packs, and another version's.
    std::filesystem::path folder() const { return folder(m_options.version); }
    std::filesystem::path folder(const std::string& version) const;
    std::filesystem::path downloadsFolder() const;

    // The complete packs (those with a pack.json) in this version's folder, or in
    // another's, by name.
    std::vector<InstalledPack> installed() const { return installed(m_options.version); }
    std::vector<InstalledPack> installed(const std::string& version) const;
    static std::optional<PackRecord> readRecord(const std::filesystem::path& packFolder);
    static bool writeRecord(const std::filesystem::path& packFolder, const PackRecord& record);

    // wanted.json: the packs the user chose, whatever version has them.
    std::vector<std::string> wanted() const;
    // Not wanted puts the pack among the removed ones, wanted takes it out of them.
    bool setWanted(const std::string& name, bool wanted);
    // The packs the user removed and hasn't installed again: a session saved with one
    // loads without it, and says nothing (docs/SESSIONS.md §4.3).
    std::vector<std::string> removed() const;

    // At the start, before the plugins are scanned. Deletes the packs marked removed
    // and the folders of versions lower than this one, where no usdcc uses them
    // (`keepVersions` stay: a downloaded update's). Then makes each of this version's
    // packs loadable and holds its .in-use. Returns them; one with a `problem`, or
    // still marked removed, must not be loaded.
    std::vector<InstalledPack> prepare(const std::set<std::string>& keepVersions = {});
    // What prepare() returned.
    const std::vector<InstalledPack>& prepared() const { return m_prepared; }
    // Whether this process may have loaded the pack: prepare() offered it.
    bool isInUseHere(const std::string& name) const;

    // Marks a pack of this version for removal at a later start, or takes the mark
    // back. False when there is no such pack or the mark can't be written.
    bool markRemoved(const std::string& name, bool removed);
    // Deletes a pack of this version at once. Only for one this process hasn't
    // loaded and no other usdcc holds; false with `error` otherwise.
    bool removeNow(const std::string& name, std::string* error);

private:
    Options m_options;
    std::vector<InstalledPack> m_prepared;
    std::vector<std::unique_ptr<InUseMarker>> m_inUse;
};

}  // namespace usdcc::scripting
