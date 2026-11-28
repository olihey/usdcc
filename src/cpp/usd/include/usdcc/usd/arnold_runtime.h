#pragma once

// Arnold in usdcc (docs/USD_PLUGINS.md §3): off unless Settings > Renderers >
// Arnold names an Arnold folder. The Arnold pack is in plugin/arnold, beside OpenUSD's
// plugin/usd but not on its plugin path, as its shader registry plugin starts
// the Arnold core when OpenUSD's shader registry is first used. A pack usdcc
// downloaded (View > Plugins > Packs, §10) has the same plugin/arnold in its own
// folder, and is used when there is none beside OpenUSD's.
//
// With a folder set, enableArnold() checks that its Arnold core is one the pack
// can run on, loads it by its full path (so neither its folder, which has Qt
// DLLs of its own, nor anything else goes on PATH) and registers the pack's
// plugins with OpenUSD. Not through PXR_PLUGINPATH_NAME, which OpenUSD reads
// when its libraries load, before main(). applyUsdEnvironmentSettings() calls
// it at start-up, before anything makes a stage or asks for a renderer, and
// enableArnoldPack() (usd_environment.h) with a downloaded pack after it.
// Qt-free.

#include <string>
#include <string_view>

namespace usdcc::usd {

// What enableArnold() found and did.
struct ArnoldStatus {
    bool enabled = false;
    std::string arnoldVersion;  // the Arnold core's, as "7.5.4.0"; "" if it wasn't loaded
    std::string packVersion;    // the Arnold the pack was built against
    std::string pluginFolder;   // the pack's, whose plugins it registers
    bool noPlugins = false;     // there is no pack under the root that was looked in
    std::string message;        // what was done, or why not: a line for the log
};

// Whether plugins built against Arnold `builtWith` run on the Arnold core
// `runtime` (both as "7.5.4.0"): the same 7.x, and not older. Plugins built
// against 7.5.4 don't load with 7.5.2's core ("The specified procedure could
// not be found").
bool arnoldVersionFits(std::string_view runtime, std::string_view builtWith);

// The folder OpenUSD's plugin/ and usd-plugins/ are in for this process: the
// parent of the folder its libraries were loaded from (the OpenUSD install's
// lib/, a deploy's bin/).
std::string usdRootFolder();

// Turns Arnold on from `arnoldFolder`, an Arnold SDK or install (the folder
// with bin/ai.dll, bin/libai.dylib on macOS, bin/libai.so on Linux), with the pack under `usdRoot` (default: usdRootFolder()).
// An empty folder does nothing. Logs nothing: the caller logs `message`.
ArnoldStatus enableArnold(const std::string& arnoldFolder, const std::string& usdRoot = {});

}  // namespace usdcc::usd
