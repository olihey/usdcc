#include "startup.h"

#include "usdcc/core/file_log_sink.h"
#include "usdcc/core/settings.h"
#include "usdcc/core/version.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/peers_controller.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/asset_settings.h"
#include "usdcc/usd/color_management.h"
#include "usdcc/usd/render/hydra_final_renderer.h"
#include "usdcc/usd/render/render_spec.h"
#include "usdcc/usd/schema_libraries.h"
#include "usdcc/usd/usd_environment.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSurfaceFormat>

namespace usdcc::app {

void configureLogLevels(const char* source) {
    usdcc::core::setThreadName("main");
    if (const QString spec = qEnvironmentVariable("USDCC_LOG"); !spec.isEmpty()) {
        if (!usdcc::core::LogHub::instance().configureLevels(spec.toStdString())) {
            USDCC_LOG_WARNING(source, "Ignored parts of USDCC_LOG=\"{}\" that aren't valid", spec.toStdString());
        }
    }
}

void setDefaultSurfaceFormat() {
    // Must be set before the application object is made (QSurfaceFormat docs).
    // HgiGL_ScopedStateHolder (Hydra/Storm's save-and-restore-caller's-GL-state
    // wrapper, used so Storm can share a GL context with a foreign renderer like
    // Qt's) queries/pushes legacy state that doesn't exist in a Core profile
    // context: "GL error: invalid enum" from HgiGLPostPendingGLErrors when a Core
    // profile was asked for. A compatibility profile is what it expects.
    QSurfaceFormat format;
    format.setVersion(4, 5);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    // Without it Qt asks for a forward-compatible context (no deprecated functions),
    // which Mesa answers with a Core profile despite the profile above
    // (docs/LINUX_WSL.md). NVIDIA's Windows driver ignores the flag.
    format.setOption(QSurfaceFormat::DeprecatedFunctions);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    // USDCC_GL_DEBUG=1 asks for a debug context: the viewport then logs each GL
    // error the driver reports, at the call that caused it, with a stack.
    if (qEnvironmentVariableIntValue("USDCC_GL_DEBUG") != 0) {
        format.setOption(QSurfaceFormat::DebugContext);
    }
    QSurfaceFormat::setDefaultFormat(format);
}

bool applyTestProfile(QCoreApplication& app, const QString& applicationName) {
    // Integration tests need isolation before settings, plugins, Python and layouts
    // load. APPDATA alone does not redirect Qt's Windows known folders.
    const QString testProfile = qEnvironmentVariable("USDCC_TEST_PROFILE");
    if (testProfile.isEmpty() || testProfile.contains('/') || testProfile.contains('\\')) return false;
    QStandardPaths::setTestModeEnabled(true);
    app.setApplicationName(applicationName + "-test-" + testProfile);
    return true;
}

ScopedLogSink::ScopedLogSink(std::shared_ptr<usdcc::core::LogSink> sink) : m_sink(std::move(sink)) {
    if (m_sink) usdcc::core::LogHub::instance().addSink(m_sink);
}

ScopedLogSink::~ScopedLogSink() {
    if (m_sink) usdcc::core::LogHub::instance().removeSink(m_sink.get());
}

std::unique_ptr<ScopedLogSink> openLogFile(const char* source, const char* program, QString* path) {
    // Needs the application object for its folder (ui/settings.h).
    const QString logDir = usdcc::ui::userDataFolder() + "/logs";
    auto fileLog = std::make_unique<ScopedLogSink>(usdcc::core::FileLogSink::openInDirectory(logDir.toStdU16String()));
    if (path) path->clear();
    if (*fileLog) {
        // First, so every log file says which build wrote it.
        USDCC_LOG_INFO(source, "{} {}{} starting", program,
                       usdcc::core::version(), usdcc::core::isReleaseBuild() ? "" : " (development build)");
        const auto& file = static_cast<usdcc::core::FileLogSink&>(*fileLog->get()).path();
        const QString text = QString::fromStdU16String(file.generic_u16string());
        if (path) *path = text;
        USDCC_LOG_INFO(source, "Logging to {}", text.toStdString());
    } else {
        USDCC_LOG_WARNING(source, "Could not create a log file in {}", logDir.toStdString());
    }
    return fileLog;
}

void installSettings() {
    // Preferences (docs/SETTINGS.md) load from usdcc.ini from here on; every setting
    // defined later starts from its saved value.
    usdcc::ui::installAppSettings();
    // Settings > USD: environment variables OpenUSD reads once, so set before anything
    // uses its plugin registry or asset resolver (docs/USD_RUNTIME.md §3).
    usdcc::usd::defineUsdEnvironmentSettings();
    usdcc::usd::applyUsdEnvironmentSettings();
    // The OCIO config, and OCIO for Hydra's color correction, before anything renders
    // (docs/COLOR_MANAGEMENT.md).
    usdcc::usd::defineColorManagementSettings();
    usdcc::usd::applyColorManagementSettings();
    // Read by the plugin manager and Python.
    usdcc::scripting::definePythonSettings();
    usdcc::scripting::definePluginSettings();
}

std::vector<std::filesystem::path> pluginFolders(const QStringList& values) {
    std::vector<std::filesystem::path> folders;
    for (const QString& folder : values) {
        // Absolute, so the plugins' paths (their libraries, USD folders, Python modules)
        // don't depend on the working directory later on.
        folders.emplace_back(QFileInfo(folder).absoluteFilePath().toStdU16String());
    }
    return folders;
}

void configurePluginUsd(const usdcc::scripting::PluginManager& plugins) {
    // The codeless schema libraries the Schemas panel can edit (docs/SCHEMAS.md §5.1): in
    // every plugin's usd folders, enabled or not; a new one in the user's plugins folder.
    std::vector<std::string> usdFolders;
    for (const usdcc::scripting::PluginInfo& plugin : plugins.plugins()) {
        for (const std::string& folder : plugin.manifest.usdFolders) {
            const std::u8string text = (plugin.manifest.folder / std::filesystem::u8path(folder)).lexically_normal().u8string();
            usdFolders.emplace_back(text.begin(), text.end());
        }
    }
    const std::u8string user = usdcc::scripting::PluginManager::userFolder().u8string();
    usdcc::usd::SchemaLibraries::instance().configure(std::move(usdFolders), std::string(user.begin(), user.end()),
                                                      usdcc::scripting::kPluginApiVersion);
    // Settings > Assets (docs/REMOTE_ASSETS.md §10): which servers a webdav:// path means,
    // before a stage can open one. After the scan, which registers the protocols.
    usdcc::usd::defineAssetSettings();
}

void registerRenderers() {
    usdcc::render::defineFinalRenderSettings();
    usdcc::render::registerHydraFinalRenderers();
}

usdcc::peers::Peers::Options peersOptions(bool network) {
    usdcc::ui::PeersController::defineSettings();
    usdcc::peers::Peers::Options options;
    options.profile = usdcc::ui::peersProfile();
    options.identity = &usdcc::ui::peersIdentity;
    options.roomsFolder = usdcc::ui::peersRoomsFolder();
    if (network) options.network = usdcc::peers::createMdnsDiscovery();
    return options;
}

}  // namespace usdcc::app
