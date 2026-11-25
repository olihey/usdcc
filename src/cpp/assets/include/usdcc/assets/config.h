#pragma once

// The configuration (docs/REMOTE_ASSETS.md §4): endpoints, and for each environment
// the stack of endpoints behind each name.
//
//     {
//       "environment": "prod",
//       "endpoints": {
//         "prod": { "scheme": "webdav", "url": "https://dav.studio.local/assets", "readOnly": true },
//         "test": { "scheme": "webdav", "url": "https://dav-test.studio.local/assets" }
//       },
//       "environments": {
//         "prod": { "webdav": { "studio": ["prod"] } },
//         "test": { "webdav": { "studio": ["test", "prod"] } }
//       }
//     }

#include "usdcc/assets/result.h"
#include "usdcc/assets/transport.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::assets {

// A host of the plain HTTP protocol (docs/HTTP_RESOLVER.md §4).
struct HttpHost {
    std::string credential;  // a credential id; empty for anonymous
    bool insecure = false;   // its credential may be sent over plain http://
};

// Another configuration this one uses (docs/ASSET_SOURCES.md §3): read-only, everything it
// defines named with its alias in front ("studio-a/prod").
struct SourceEntry {
    std::string alias;  // a name (isValidName())
    std::string path;   // its file; relative to the file that lists it
    std::string url;    // or where it's served, https:// (docs/ASSET_SOURCES.md §7)
};

struct Config {
    // The environment used when nothing else names one. One of another source or a mixed one
    // is written as "defaultEnvironment", which an older usdcc doesn't read.
    std::string environment;
    // The other configurations, in order (only a personal file's are read).
    std::vector<SourceEntry> sources;
    // The alias a source suggests for itself ("name").
    std::string name;
    // A mixed environment's (docs/ASSET_SOURCES.md §4): the environments of sources whose names it
    // has too. An environment with these, or with another source's servers in a stack, is written
    // under "mixed", where an older usdcc doesn't look.
    std::map<std::string, std::vector<std::string>> extends;
    // Of a merged configuration (mergeSources()): each source's default environment, qualified.
    std::vector<std::string> sourceDefaults;
    // Of a merged configuration: the credential ids sources from a URL are fetched with, and
    // which source.
    std::map<std::string, std::string> sourceLogins;
    // "http": { "hosts": { "<host>": { "credential": "...", "insecure": false } }, "allow": ["*.studio.local"] }
    std::map<std::string, HttpHost> httpHosts;
    std::vector<std::string> httpAllow;
    // "s3": { "allow": ["studio-*"] }: the buckets a stage may name by themselves
    // (docs/S3_RESOLVER.md §3.2), with the patterns hosts have.
    std::vector<std::string> s3Allow;
    // Resolve and open from the disk cache only (§7).
    bool offline = false;
    // The disk cache's size limit; "cacheLimitGB" in the file. Unset leaves it as it is.
    std::optional<double> cacheLimitGB;
    std::map<std::string, Endpoint> endpoints;
    // environment -> scheme -> name -> endpoint ids, top of the stack first.
    std::map<std::string, std::map<std::string, std::map<std::string, std::vector<std::string>>>> environments;

    // The text it was parsed from, so toJson() keeps the keys this version doesn't know.
    std::string source;

    // Everything wrong with the text is in the failure's message, one problem per line.
    static Result<Config> parse(const std::string& json);
    static Result<Config> load(const std::filesystem::path& file);

    // As the file has it (docs/REMOTE_ASSETS.md §10.1): indented JSON, keys in a fixed
    // (sorted) order, and every key of `source` this version doesn't know kept as it was,
    // so a newer usdcc's settings survive an older one's save.
    std::string toJson() const;
    // toJson() written to `file`, through a temporary file beside it. Folders are made.
    Result<bool> save(const std::filesystem::path& file) const;

    // USDCC_ASSETS_CONFIG, or else assets.json in userDataFolder().
    static std::filesystem::path defaultPath();
    // The file at defaultPath(). Empty when there is none; a broken one warns and is empty too.
    static Config loadDefault();

    // USDCC_ASSET_ENV, or else `environment`.
    std::string chosenEnvironment() const;

    // Whether `environment` is written under "mixed".
    bool isMixed(const std::string& environment) const;
};

// %LOCALAPPDATA%/usdcc_config on Windows (as ui::userDataFolder()), ~/.local/share/usdcc (or XDG_DATA_HOME) on Linux,
// ~/Library/Application Support/usdcc on macOS: where usdcc keeps its own files.
std::filesystem::path userDataFolder();

}  // namespace usdcc::assets
