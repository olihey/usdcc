#pragma once

// What the Asset Servers editor (docs/REMOTE_ASSETS.md §10.1) does to a configuration,
// without Qt: so the dialog in usdcc_usd stays thin and this is tested on its own. It edits
// a Config in memory, checks it the way Config::parse() would read it back, and saves it
// as the file has it (Config::toJson()), noticing a file changed on disk meanwhile.

#include "usdcc/assets/config.h"
#include "usdcc/assets/sources.h"
#include "usdcc/assets/result.h"
#include "usdcc/assets/transport.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace usdcc::assets {

class ConfigEditor {
public:
    ConfigEditor() = default;
    // The file at `file`; one that doesn't exist yet is an empty configuration. A broken
    // one is read as far as it goes, its problems in loadProblem().
    explicit ConfigEditor(std::filesystem::path file);
    // A configuration with no file behind it (tests).
    explicit ConfigEditor(Config config);

    const std::filesystem::path& file() const { return m_file; }
    const Config& config() const { return m_config; }
    Config& config() { return m_config; }
    // Why the file couldn't be read as it was; empty when it could.
    const std::string& loadProblem() const { return m_loadProblem; }

    // Whether this user can write the file (or make it, in a folder they can write).
    bool writable() const;
    // Whether the file changed on disk since it was read (or saved).
    bool changedOnDisk() const;
    // Changed in memory since it was read (or saved).
    bool modified() const;

    // Every problem with what would be written, one per entry, as Config::parse() names
    // them. Empty when it can be saved.
    std::vector<std::string> problems() const;

    // Its sources as read (docs/ASSET_SOURCES.md §3), and what they and it make together.
    const std::vector<LoadedSource>& sources() const { return m_sources; }
    void setSources(std::vector<LoadedSource> sources) { m_sources = std::move(sources); }
    // Reads its sources' files again.
    void reloadSources();
    Config merged(MergeProblems* problems = nullptr) const;
    // Adds a source at the end (not read until reloadSources()), or removes one.
    std::string addSource(const SourceEntry& entry);
    std::string removeSource(const std::string& alias);
    // What a mixed environment extends: sources' environments ("studio-a/prod").
    std::string setExtends(const std::string& environment, const std::vector<std::string>& extended);

    // Servers (the configuration's "endpoints"). Each returns why not, or empty when done.
    std::string addServer(const Endpoint& endpoint);
    // Renaming one renames it in every stack.
    std::string renameServer(const std::string& id, const std::string& newId);
    // Refused while a stack uses it, naming where.
    std::string removeServer(const std::string& id);
    // "environment 'test', webdav://studio" for each stack holding `id`.
    std::vector<std::string> usesOf(const std::string& id) const;

    // Environments and their names.
    std::string addEnvironment(const std::string& environment);
    // A copy of `from`'s names and stacks: how a test environment usually starts.
    std::string duplicateEnvironment(const std::string& from, const std::string& environment);
    std::string renameEnvironment(const std::string& environment, const std::string& newName);
    std::string removeEnvironment(const std::string& environment);
    // A name with its first server, whose protocol the name is then for.
    std::string addName(const std::string& environment, const std::string& name, const std::string& server);
    std::string removeName(const std::string& environment, const std::string& scheme, const std::string& name);
    // The stack in this order, top first: what dragging its servers gives. Only servers of
    // the name's protocol.
    std::string setStack(const std::string& environment, const std::string& scheme, const std::string& name,
                         const std::vector<std::string>& servers);

    // Writes the file (Config::toJson()); the configuration then counts as read from it.
    // Refused when it has problems; a file changed on disk meanwhile is only overwritten
    // with `overwriteChanged`.
    Result<bool> save(bool overwriteChanged = false);
    // The same configuration into `file`, which is used from then on: "Save a copy as mine".
    Result<bool> saveAs(const std::filesystem::path& file);
    // Reads the file again, dropping what was changed in memory.
    void reload();

private:
    struct Stamp {
        bool exists = false;
        uint64_t size = 0;
        int64_t modified = 0;
        bool operator==(const Stamp& other) const = default;
    };
    static Stamp stampOf(const std::filesystem::path& file);

    std::filesystem::path m_file;
    Config m_config;
    std::vector<LoadedSource> m_sources;
    std::string m_saved;  // toJson() as last read or saved: what modified() compares with
    std::string m_loadProblem;
    Stamp m_stamp;
};

// What a server said when asked for its root folder, for the editor's Test button.
struct EndpointTest {
    Status status = Status::Error;
    double milliseconds = 0;
    size_t entries = 0;      // in the root's first page
    std::string url;         // which of its URLs answered, or the last one tried
    std::string message;     // the server's or the transport's own words
    std::string summary;     // the line the editor shows
};

// Asks `endpoint` for its root through the same transport and credential a stage would
// use (its mirrors too, as resolving does), as it is: nothing has to be saved first. A
// protocol that can't list is asked for the root's stat instead. Any thread.
EndpointTest testEndpoint(const Endpoint& endpoint, Cancel& cancel);

}  // namespace usdcc::assets
