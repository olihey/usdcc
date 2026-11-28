#pragma once

// What usdcc.exe and usdcc_node.exe both do to start (docs/RENDER_NODE.md §3.1), before the
// app makes its window: logging, the profile, settings, USD's environment, color
// management, the plugins' manifests, the renderers and the peers' options. One function a
// step, called in this order, so the two programs can't drift apart on any of them.

#include "usdcc/core/log.h"
#include "usdcc/peers/peers.h"
#include "usdcc/scripting/plugin_manager.h"

#include <QString>
#include <QStringList>

#include <filesystem>
#include <memory>
#include <vector>

class QCoreApplication;

namespace usdcc::scripting {
class PackStore;
}

namespace usdcc::app {

// First thing: the main thread's name, and the levels USDCC_LOG asks for
// ("debug", "info,usdcc.viewport=debug,usd=warning"; docs/LOGGING.md §3.1).
void configureLogLevels(const char* source);

// Before the application object: the OpenGL context Storm and the viewports want, a
// 4.5 compatibility profile (USDCC_GL_DEBUG=1: a debug context).
void setDefaultSurfaceFormat();

// After the application object: a test profile (USDCC_TEST_PROFILE) isolates the user's
// folders and settings, under the name "<applicationName>-test-<profile>". False with none.
bool applyTestProfile(QCoreApplication& app, const QString& applicationName);

// A log sink attached to the hub while this exists. Declared right after the application
// object, so it outlives everything that logs while being destroyed.
class ScopedLogSink {
public:
    ScopedLogSink() = default;
    explicit ScopedLogSink(std::shared_ptr<usdcc::core::LogSink> sink);
    ~ScopedLogSink();
    ScopedLogSink(const ScopedLogSink&) = delete;
    ScopedLogSink& operator=(const ScopedLogSink&) = delete;

    usdcc::core::LogSink* get() const { return m_sink.get(); }
    explicit operator bool() const { return m_sink != nullptr; }

private:
    std::shared_ptr<usdcc::core::LogSink> m_sink;
};

// The log file in the profile's logs folder (needs the application object), started with
// the program and build that write it. `path`: the file, empty when it couldn't be made.
std::unique_ptr<ScopedLogSink> openLogFile(const char* source, const char* program, QString* path);

// usdcc.ini (docs/SETTINGS.md), then what must be set before anything uses USD's plugin
// registry, resolvers or Hydra: Settings > USD's environment, the OCIO config. Then the
// Python and plugins settings the plugin manager reads.
void installSettings();

// The plugin folders given with --plugin-path, made absolute.
std::vector<std::filesystem::path> pluginFolders(const QStringList& values);

// What the plugin manager is made with: PluginManager::defaultOptions() with the
// --plugin-path folders first, and the downloaded packs of this version
// (docs/USD_PLUGINS.md §10) made loadable by `packs` before anything is scanned. `packs`
// holds them in use from here on, so it is declared before the plugin manager. Not
// `enabled` (--no-plugins): no pack is touched or loaded either. Also turns Arnold on
// from its pack, where Settings names an Arnold folder (usd::enableArnoldPack()).
usdcc::scripting::PluginManager::Options pluginOptions(const QStringList& pluginPaths, bool enabled,
                                                       usdcc::scripting::PackStore& packs);

// After PluginManager::scan(): the codeless schema libraries in the plugins' USD folders
// (docs/SCHEMAS.md §5.1), and Settings > Assets' servers (docs/REMOTE_ASSETS.md §10).
void configurePluginUsd(const usdcc::scripting::PluginManager& plugins);

// Final renders (docs/FINAL_RENDER.md): Settings > Final Render, and every Hydra render
// delegate as a renderer. Plugins add theirs when they're activated.
void registerRenderers();

// The Peers object's options for this profile (docs/PEERS.md): its name and key, this
// machine's rooms, and this network's when `network`.
usdcc::peers::Peers::Options peersOptions(bool network = true);

}  // namespace usdcc::app
