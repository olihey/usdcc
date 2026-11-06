#pragma once

#include "usdcc/scripting/package_environment.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
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
    // Package indexes besides the usual one, for pythonRequires ("python" "indexes"): https
    // URLs only, shown to the user before installing from them (docs/PYTHON_PACKAGES.md §4).
    std::vector<std::string> pythonIndexes;
    std::vector<std::string> usdFolders;      // holding plugInfo.json, relative to `folder`
    std::vector<std::string> dependsOn;       // the ids in "requires"
    // The C++ part: bin/<platform>/<library> in `folder`, and what it was built for.
    std::string nativeLibrary;
    std::map<std::string, std::string> builtFor;
    std::filesystem::path folder;
};

// What a plugin is made of, from its manifest: "C++", "Python" and "USD", in that order,
// those it has. Empty for a plugin with none of them.
std::vector<std::string> pluginTypes(const PluginManifest& manifest);

// What a C++ plugin has to have been built with, as usdcc_add_plugin() writes it
// in the manifest: platform, compiler, qt, usd and usdcc. std:: and Qt types cross
// the boundary, and USD's symbols carry its version (docs/PLUGINS.md §3.2).
std::map<std::string, std::string> pluginBuildInfo();

enum class PluginState {
    Ready,     // scanned; activate() hasn't got to it
    Active,    // running
    Disabled,  // the user turned it off, or it crashed the last start
    Refused,   // won't be loaded, for `reason`; none of its code ran
    NeedsPackages,  // as Refused, and installing Python packages could fix it
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
        // Ready-made folders of Python packages, for sys.path (§3.10).
        std::vector<std::filesystem::path> pythonPackageFolders;
        // Where the app installs Python packages (docs/PYTHON_PACKAGES.md); its
        // environment goes on sys.path after the folders above. Empty for none.
        std::filesystem::path packagesRoot;
        // Names the plugin being activated, so that finding it at the next
        // start means that plugin crashed (§3.6).
        std::filesystem::path crashMarker;
        bool enabled = true;  // false for --no-plugins: nothing is scanned
    };

    // `extraFolders` (--plugin-path), USDCC_PLUGIN_PATH, the user's folder,
    // then the one shipped with the app. Python packages from USDCC_PYTHON_PATH
    // and the Python settings (python_settings.h, defined by then, or they count
    // as empty). Needs the Qt application object.
    static Options defaultOptions(const std::vector<std::filesystem::path>& extraFolders, bool enabled);
    // <AppLocalData>/plugins.
    static std::filesystem::path userFolder();

    explicit PluginManager(Options options);
    ~PluginManager();

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

    // Why reload() wouldn't do anything for it; empty if it would.
    std::string whyNotReloadable(const std::string& id) const;
    struct ReloadResult {
        bool ok = false;                 // the plugin is running again
        std::vector<std::string> names;  // what was reloaded, it and those requiring it
        std::vector<std::string> notes;  // what the user should know, whichever way it went
        std::string failure;             // why it isn't running, when it isn't
    };
    // Puts a Python plugin's changes into effect with no restart (docs/PLUGIN_DEV.md):
    // its deactivate(), taking back what it registered, forgetting its modules, reading
    // its manifest and checking its packages again, and activating. Plugins requiring it
    // go with it. Open panels of its view panel types are rebuilt where they are. Why it
    // failed is in its PluginInfo's `reason`. Main thread; not from inside the plugin
    // (usdcc.app.reload_plugin() waits for it to finish).
    ReloadResult reload(const std::string& id, PythonRuntime& python);

    // Writes a new Python plugin called `name` into a folder of its own in `parent`, and
    // starts it with no restart. Its id, or empty with `error` saying why.
    std::string createPlugin(const std::string& name, const std::filesystem::path& parent, PythonRuntime& python, std::string* error);

    // What should be in the shared packages folder, in the order to resolve it,
    // for the enabled plugins and My packages; and what must never be installed.
    std::vector<PackageRequest> packageRequests(PythonRuntime& python, std::vector<std::string>* blocked) const;
    // How to run the installer, from the settings.
    PackageInstaller::Options installerOptions(const PythonRuntime& python, bool upgrade) const;
    // Whether an install would do something the user is waiting for: a plugin needs
    // packages, or My packages changed.
    bool packagesPending() const;
    // Says an install finished, which takes effect at the next start.
    void noteInstalled() { m_packagesInstalled = true; }
    // The folders of packages on sys.path, in order; set by activate().
    const std::vector<std::filesystem::path>& packageFoldersInUse() const { return m_packageFolders; }
    // Installs what packageRequests() asks for, for --install-plugin-packages.
    // Runs an event loop until it's done. 0 when everything went in, 2 when some
    // was left out, 1 when it failed. What happens goes to `report`.
    int installPackages(PythonRuntime& python, const std::function<void(const std::string&)>& report);

private:
    PluginInfo* findMutable(const std::string& id);
    void readManifests();
    void applySettings();
    void orderAndCheckDependencies();
    void refuseDependents();
    void registerUsdFolders();
    void writeCrashMarker(const std::string& id) const;
    void removeCrashMarker() const;
    // Loads and activates its library; returns why not, or nothing.
    std::string activateNative(PluginInfo& plugin);
    // Refuses those of `plugins` whose Python packages won't do.
    void checkPythonModules(const std::vector<PluginInfo*>& plugins, PythonRuntime& python);
    void activateOne(PluginInfo& plugin);

    struct NativePlugin;

    Options m_options;
    std::vector<PluginInfo> m_plugins;
    std::string m_crashedLastStart;
    // By id. Libraries are never unloaded (§3.4).
    std::map<std::string, std::unique_ptr<NativePlugin>> m_native;
    // Manifests that can't be used, whose packages shouldn't be installed.
    std::set<std::string> m_unusable;
    std::unique_ptr<PackageEnvironment> m_environment;
    std::vector<std::filesystem::path> m_packageFolders;
    std::string m_sharedFolder;
    bool m_packagesInstalled = false;
    bool m_reloading = false;
};

}  // namespace usdcc::scripting
