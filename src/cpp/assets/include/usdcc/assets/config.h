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
#include <string>
#include <vector>

namespace usdcc::assets {

struct Config {
    // The environment used when nothing else names one.
    std::string environment;
    std::map<std::string, Endpoint> endpoints;
    // environment -> scheme -> name -> endpoint ids, top of the stack first.
    std::map<std::string, std::map<std::string, std::map<std::string, std::vector<std::string>>>> environments;

    // Everything wrong with the text is in the failure's message, one problem per line.
    static Result<Config> parse(const std::string& json);
    static Result<Config> load(const std::filesystem::path& file);

    // USDCC_ASSETS_CONFIG, or else assets.json in userDataFolder().
    static std::filesystem::path defaultPath();
    // The file at defaultPath(). Empty when there is none; a broken one warns and is empty too.
    static Config loadDefault();

    // USDCC_ASSET_ENV, or else `environment`.
    std::string chosenEnvironment() const;
};

// %LOCALAPPDATA%/usdcc on Windows, ~/.local/share/usdcc (or XDG_DATA_HOME) on Linux,
// ~/Library/Application Support/usdcc on macOS: where usdcc keeps its own files.
std::filesystem::path userDataFolder();

}  // namespace usdcc::assets
