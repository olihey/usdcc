#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace usdcc::scripting {

class PythonRuntime;

// What a plugin's manifest has to name in "api" to load. Raised only when
// something breaks (docs/PLUGINS.md §5).
inline constexpr int kPluginApiVersion = 1;

// usdcc-plugin.json, in the plugin's folder.
struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    int api = 0;
    std::string pythonModule;
    std::vector<std::string> pythonRequires;  // third-party modules
    std::vector<std::string> usdFolders;      // holding plugInfo.json, relative to `folder`
    std::vector<std::string> dependsOn;       // the ids in "requires"
    bool hasNative = false;
    std::filesystem::path folder;
};

enum class PluginState {
    Ready,     // scanned; activate() hasn't got to it
    Active,    // running
    Disabled,  // the user turned it off, or it crashed the last start
    Refused,   // won't be loaded, for `reason`; none of its code ran
    Failed,    // its code ran and raised, for `reason`
};

struct PluginContribution {
    std::string kind;  // "View panel", "Tool", ...
    std::string name;
};

struct PluginInfo {
    PluginManifest manifest;
    PluginState state = PluginState::Ready;
    std::string reason;
    std::vector<PluginContribution> contributions;
};

// Finds and runs the plugins (docs/PLUGINS.md). Startup has two steps: scan()
// reads the manifests and registers USD plugin folders, running no plugin code,
// and has to come before the first stage (§3.1). activate() runs the plugins'
// code, once the window and registries exist and before the saved layout is
// restored, so their panels come back (§3.3). What a plugin registered can't be
// taken back, so turning one on or off takes effect at the next start (§3.4).
class PluginManager {
public:
    struct Options {
        // Folders of plugin folders, the first holding an id winning.
        std::vector<std::filesystem::path> folders;
        // Folders of shared Python packages, for sys.path (§3.10).
        std::vector<std::filesystem::path> pythonPackageFolders;
        // Names the plugin being activated, so that finding it at the next
        // start means that plugin crashed (§3.6).
        std::filesystem::path crashMarker;
        bool enabled = true;  // false for --no-plugins: nothing is scanned
    };

    // `extraFolders` (--plugin-path), USDCC_PLUGIN_PATH, the user's folder,
    // then the one shipped with the app. Python packages from USDCC_PYTHON_PATH
    // and the user's python-packages. Needs the Qt application object.
    static Options defaultOptions(const std::vector<std::filesystem::path>& extraFolders, bool enabled);
    // <AppLocalData>/plugins.
    static std::filesystem::path userFolder();

    explicit PluginManager(Options options);

    void scan();
    void activate(PythonRuntime& python);
    // deactivate(plugin) of each active plugin, last activated first.
    void deactivate(PythonRuntime& python);

    // In activation order.
    const std::vector<PluginInfo>& plugins() const { return m_plugins; }
    const PluginInfo* find(const std::string& id) const;
    // Set when scan() found that a plugin crashed the last start; else empty.
    const std::string& crashedLastStart() const { return m_crashedLastStart; }

    // Whether it should load at the next start (kept in usdcc.ini).
    bool isEnabled(const std::string& id) const;
    void setEnabled(const std::string& id, bool enabled);
    // Whether anything differs from what this session is running.
    bool hasPendingChanges() const;

    void addContribution(const std::string& id, const std::string& kind, const std::string& name);

private:
    PluginInfo* findMutable(const std::string& id);
    void readManifests();
    void applySettings();
    void orderAndCheckDependencies();
    void refuseDependents();
    void registerUsdFolders();
    void writeCrashMarker(const std::string& id) const;
    void removeCrashMarker() const;

    Options m_options;
    std::vector<PluginInfo> m_plugins;
    std::string m_crashedLastStart;
};

}  // namespace usdcc::scripting
