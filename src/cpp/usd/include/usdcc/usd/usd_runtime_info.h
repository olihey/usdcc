#pragma once

// What Help > USD Runtime shows (docs/USD_RUNTIME.md §2): the OpenUSD the app
// runs with, its plugins, the file formats and render delegates they add, the
// USD environment variables, and TF_DEBUG's flags. Qt-free.
//
// Collecting loads nothing: plugins, file formats and render delegates are
// read from the plugins' plugInfo.json, so opening the dialog doesn't load a
// render delegate or a file format's library.

#include <string>
#include <string_view>
#include <vector>

namespace usdcc::usd {

struct UsdRuntimeInfo {
    struct Row {
        std::string name;
        std::string value;
    };
    struct Section {
        std::string title;
        std::vector<Row> rows;
    };
    struct Plugin {
        std::string name;
        std::string type;  // "library", "python" or "resource"
        bool loaded = false;
        std::string path;  // its library, Python package or resource folder
    };
    struct FileFormat {
        std::string formatId;
        std::vector<std::string> extensions;
        std::string target;  // "usd" for most; another one for a format only some clients use
        bool reading = true;
        bool writing = true;
        std::string plugin;
    };
    struct Renderer {
        std::string id;
        std::string displayName;
        int priority = 0;
        std::string plugin;
    };
    struct EnvironmentVariable {
        std::string name;
        std::string value;
        std::string source;  // usdEnvironmentSource(): "" when it came from outside the app
    };

    std::vector<Section> overview;  // version, libraries, threads, asset resolution, shading
    std::vector<Plugin> plugins;    // by name
    std::vector<FileFormat> fileFormats;
    std::vector<Renderer> renderers;
    std::vector<EnvironmentVariable> environment;  // isUsdEnvironmentVariable() ones, by name
};

// Everything above, now.
UsdRuntimeInfo collectUsdRuntimeInfo();

// All of it as plain text, a heading per section and a line per row: what the
// dialog's Copy puts on the clipboard, for a bug report.
std::string usdRuntimeInfoText(const UsdRuntimeInfo& info);

// Whether `name` is a variable OpenUSD, Hydra, MaterialX or usdcc reads: one
// starting with PXR_, TF_, HD, HGI, USD, SDF_, ... (the list is in the .cpp).
bool isUsdEnvironmentVariable(std::string_view name);

// TF_DEBUG's flags (TfDebug), which turn on extra output from parts of OpenUSD.
// It goes to the log (docs/LOGGING.md §6). Unlike the TF_DEBUG environment
// variable, which is read once when the libraries load, these change at once.
struct UsdDebugFlag {
    std::string name;
    std::string description;
    bool enabled = false;
};
// By name.
std::vector<UsdDebugFlag> usdDebugFlags();
// False for an unknown flag.
bool setUsdDebugFlag(const std::string& name, bool enabled);

}  // namespace usdcc::usd
