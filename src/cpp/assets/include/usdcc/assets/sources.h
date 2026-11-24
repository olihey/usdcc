#pragma once

// Several configurations at once (docs/ASSET_SOURCES.md §3-§5): the personal file, and the
// sources it lists, read-only. What a source defines is named with its alias in front:
// its servers ("studio-a/nas"), environments ("studio-a/prod") and credential ids
// ("studio-a/dav"), so no source can name another's. Mixed environments of the personal
// file take names from sources' environments.

#include "usdcc/assets/config.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace usdcc::assets {

struct LoadedSource {
    SourceEntry entry;
    std::filesystem::path file;  // where it was read from
    Config config;               // as its file has it, unqualified
    std::string problem;         // why it isn't used; empty when it is
    // From a URL: what its copy on this machine is. Seconds since the epoch, 0 for never.
    double checked = 0;          // when the server last said what it has
    double changed = 0;          // when the copy was fetched
    std::string fetchProblem;    // why the last try failed; the copy is used meanwhile
    bool wantsLogin = false;     // it was refused: sourceCredentialId()'s login is missing or wrong
};

struct MergeProblems {
    // Of the personal file: its mixed environments, its default. They keep it from being saved.
    std::vector<std::string> personal;
    // Of the sources: one that can't be read, a web host two of them have. The rest still work.
    std::vector<std::string> sources;
    std::vector<std::string> all() const;
};

// "<alias>/<id>"; `id` alone for the personal file (an empty alias).
std::string qualifiedId(const std::string& alias, const std::string& id);
// A credential id as a source's servers use it: qualified, but for AWS's own and none.
std::string qualifiedCredential(const std::string& alias, const std::string& id);
// The alias of a qualified id; empty for a personal one.
std::string aliasOf(const std::string& id);

// Reads `personal`'s sources, a path relative to `personalFile`'s folder. Each whose file
// can't be read, or isn't a configuration, has its problem said and is left out.
std::vector<LoadedSource> loadSources(const Config& personal, const std::filesystem::path& personalFile);
LoadedSource loadSource(const SourceEntry& entry, const std::filesystem::path& personalFile);

// The credential id a source's URL is fetched with: "<alias>/@source".
std::string sourceCredentialId(const std::string& alias);
// Why `url` can't be a source's: not https://, no host, or a login in it. Empty when it can.
std::string badSourceUrl(const std::string& url);

enum class Refreshed { Unchanged, Changed, Failed };
struct SourceRefresh {
    Refreshed what = Refreshed::Failed;
    bool wantsLogin = false;
    std::string problem;
};
// Asks the server of a source's URL for its configuration, and keeps it when it's a valid one
// that changed. Blocks, for `timeout` at most: not on the UI's thread. Redirects are followed
// on the same host only. `anonymous` sends no login.
SourceRefresh refreshSource(const SourceEntry& entry, std::chrono::milliseconds timeout, bool anonymous = false);
// Removes its copy.
void forgetSourceCopy(const SourceEntry& entry);
// Whether loadSource() fetches a source that has no copy (once, with a short timeout). On by
// default, for tools; the app turns it off, and fetches in the background.
void setFetchSourcesAtLoad(bool fetch);
// Tests: http:// on 127.0.0.1 is let through.
void allowLoopbackSources(bool allow);

// One configuration of the personal file and its sources: what Context and the resolver use.
Config mergeSources(const Config& personal, const std::vector<LoadedSource>& sources, MergeProblems* problems = nullptr);

// The environments of `config` that have `scheme`://`name`, each source's default first.
std::vector<std::string> environmentsWithName(const Config& config, const std::string& scheme, const std::string& name);

// What to offer for a stage whose environment lacks `scheme`://`name` (docs/ASSET_SOURCES.md §4):
// of each source, its default when that has the name, else each of its environments that has;
// and the personal ones that have it.
std::vector<std::string> offeredEnvironments(const Config& config, const std::string& scheme, const std::string& name);

// An alias for a source of that file: the one it suggests, else made from its file name.
std::string suggestedAlias(const Config& source, const std::filesystem::path& file);

}  // namespace usdcc::assets
