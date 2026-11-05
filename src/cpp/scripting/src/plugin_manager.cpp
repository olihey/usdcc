// pybind11 (and with it Python.h) first: Python.h must precede the standard
// headers.
#if USDCC_HAS_PYTHON
#include <pybind11/embed.h>
#include <pybind11/stl.h>
#endif

#include "usdcc/scripting/plugin_manager.h"

#include "usdcc/core/log.h"
#include "usdcc/plugin/plugin.h"
#include "usdcc/core/settings.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/python_settings.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/settings.h"

#include <pxr/base/plug/registry.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QLibrary>
#include <QStringList>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace usdcc::scripting {

namespace {
constexpr const char* kManifestName = "usdcc-plugin.json";
constexpr auto kDisabledKey = "Plugins/disabled";
constexpr auto kCrashedKey = "Plugins/crashed";

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path toPath(const QString& text) { return std::filesystem::path(text.toStdU16String()); }

QString toQString(const std::filesystem::path& path) { return QString::fromStdU16String(path.u16string()); }

QStringList settingList(const char* key) { return usdcc::ui::appSettings().value(key).toStringList(); }

void setSettingList(const char* key, const QStringList& list) {
    QSettings settings = usdcc::ui::appSettings();
    if (list.isEmpty()) {
        settings.remove(key);
    } else {
        settings.setValue(key, list);
    }
}

std::vector<std::filesystem::path> environmentFolders(const char* name) {
    std::vector<std::filesystem::path> folders;
    const QString value = qEnvironmentVariable(name);
    for (const QString& folder : value.split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
        folders.push_back(toPath(folder));
    }
    return folders;
}

std::vector<std::string> stringList(const QJsonValue& value) {
    std::vector<std::string> list;
    for (const QJsonValue& item : value.toArray()) list.push_back(item.toString().toStdString());
    return list;
}

// The reason it can't be used, or empty.
std::string parseManifest(const std::filesystem::path& file, PluginManifest& manifest) {
    QFile input(toQString(file));
    if (!input.open(QIODevice::ReadOnly)) return "Its manifest can't be read.";
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &error);
    if (!document.isObject()) return "Its manifest isn't valid JSON: " + error.errorString().toStdString() + ".";
    const QJsonObject json = document.object();

    const QString id = json.value("id").toString();
    if (id.isEmpty()) return "Its manifest has no \"id\".";
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9._-]+$")).match(id).hasMatch()) {
        return "Its id \"" + id.toStdString() + "\" has characters other than letters, digits, '.', '_' and '-'.";
    }
    manifest.id = id.toStdString();
    manifest.name = json.value("name").toString().toStdString();
    manifest.version = json.value("version").toString().toStdString();
    manifest.description = json.value("description").toString().toStdString();
    manifest.api = json.value("api").toInt();
    const QJsonObject python = json.value("python").toObject();
    manifest.pythonModule = python.value("module").toString().toStdString();
    manifest.pythonRequires = stringList(python.value("requires"));
    manifest.usdFolders = stringList(json.value("usd"));
    manifest.dependsOn = stringList(json.value("requires"));
    const QJsonObject native = json.value("native").toObject();
    manifest.nativeLibrary = native.value("library").toString().toStdString();
    const QJsonObject builtFor = native.value("built_for").toObject();
    for (auto it = builtFor.begin(); it != builtFor.end(); ++it) {
        manifest.builtFor[it.key().toStdString()] = it.value().toString().toStdString();
    }

    if (manifest.name.empty()) return "Its manifest has no \"name\".";
    if (manifest.version.empty()) return "Its manifest has no \"version\".";
    if (manifest.api != kPluginApiVersion) {
        return "It was made for plugin API " + std::to_string(manifest.api) + ", and this app has API " +
               std::to_string(kPluginApiVersion) + ".";
    }
    if (python.contains("requires") && manifest.pythonModule.empty()) {
        return "Its manifest has \"python\" \"requires\" with no \"module\".";
    }
    if (json.contains("native")) {
        if (manifest.nativeLibrary.empty()) return "Its manifest's \"native\" names no \"library\".";
        const auto app = pluginBuildInfo();
        if (manifest.builtFor.empty()) return "Its manifest doesn't say what its C++ part was built for.";
        // First the platform, which says the most.
        for (const char* key : {"platform", "compiler", "qt", "usd", "usdcc"}) {
            const auto built = manifest.builtFor.find(key);
            const std::string plugin = built == manifest.builtFor.end() ? "an unknown one" : built->second;
            const std::string ours = app.at(key);
            if (plugin != ours) {
                return std::string("Its C++ part was built for ") + key + " " + plugin + ", and this app has " + ours + ".";
            }
        }
    }
    return {};
}

std::filesystem::path nativeLibraryPath(const PluginManifest& manifest) {
#if defined(_WIN32)
    const char* extension = ".dll";
#elif defined(__APPLE__)
    const char* extension = ".so";
#else
    const char* extension = ".so";
#endif
    return manifest.folder / "bin" / USDCC_BUILT_FOR_PLATFORM / std::filesystem::u8path(manifest.nativeLibrary + extension);
}

// A library and what it exports. Its own DLLs are found next to it. Never unloaded.
struct LoadedLibrary {
    std::function<void*(const char*)> symbol;
    std::string error;
};

LoadedLibrary loadLibrary(const std::filesystem::path& given) {
    LoadedLibrary loaded;
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs a full path: a relative one (from
    // `--plugin-path build/plugins`) fails with "The parameter is incorrect".
    std::error_code absoluteError;
    const std::filesystem::path absolute = std::filesystem::absolute(given, absoluteError);
    const std::filesystem::path& path = absoluteError ? given : absolute;
#if defined(_WIN32)
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        const DWORD code = GetLastError();
        wchar_t* text = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                       reinterpret_cast<wchar_t*>(&text), 0, nullptr);
        loaded.error = text ? QString::fromWCharArray(text).trimmed().toStdString() : "error " + std::to_string(code);
        if (text) LocalFree(text);
        // Windows doesn't say which: it's the library or something it links.
        if (code == ERROR_MOD_NOT_FOUND) {
            loaded.error += " A DLL it links is missing, perhaps built against other libraries than this app's (a debug Qt, say).";
        }
        return loaded;
    }
    loaded.symbol = [module](const char* name) { return reinterpret_cast<void*>(GetProcAddress(module, name)); };
#else
    auto library = std::make_shared<QLibrary>(toQString(path));
    library->setLoadHints(QLibrary::PreventUnloadHint);
    if (!library->load()) {
        loaded.error = library->errorString().toStdString();
        return loaded;
    }
    loaded.symbol = [library](const char* name) { return reinterpret_cast<void*>(library->resolve(name)); };
#endif
    return loaded;
}
}  // namespace

std::vector<std::string> pluginTypes(const PluginManifest& manifest) {
    std::vector<std::string> types;
    if (!manifest.nativeLibrary.empty()) types.emplace_back("C++");
    if (!manifest.pythonModule.empty()) types.emplace_back("Python");
    if (!manifest.usdFolders.empty()) types.emplace_back("USD");
    return types;
}

std::map<std::string, std::string> pluginBuildInfo() {
    return {{"platform", USDCC_BUILT_FOR_PLATFORM},
            {"compiler", USDCC_BUILT_FOR_COMPILER},
            {"qt", USDCC_BUILT_FOR_QT},
            {"usd", USDCC_BUILT_FOR_USD},
            {"usdcc", USDCC_BUILT_FOR_USDCC}};
}

struct PluginManager::NativePlugin {
    usdcc::plugin::Host host;
    usdcc::plugin::DeactivateFunction deactivate = nullptr;
};

std::filesystem::path PluginManager::userFolder() {
    return toPath(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/plugins");
}

PluginManager::Options PluginManager::defaultOptions(const std::vector<std::filesystem::path>& extraFolders,
                                                     bool enabled) {
    Options options;
    options.enabled = enabled;
    options.folders = extraFolders;
    for (const auto& folder : environmentFolders("USDCC_PLUGIN_PATH")) options.folders.push_back(folder);
    options.folders.push_back(userFolder());
    // A deployed usdcc has bin/usdcc.exe next to plugins/ (tools/deploy.ps1).
    options.folders.push_back(toPath(QCoreApplication::applicationDirPath()).parent_path() / "plugins");

    options.pythonPackageFolders = environmentFolders("USDCC_PYTHON_PATH");
    for (const auto& folder : extraPackageFoldersSetting()) options.pythonPackageFolders.push_back(folder);
    if (usdcc::core::Settings::instance().contains(kPackagesFolderKey)) options.packagesRoot = packagesFolderSetting();
    const QString local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    options.crashMarker = toPath(local + "/plugin-activating.txt");
    return options;
}

PluginManager::PluginManager(Options options) : m_options(std::move(options)) {}

PluginManager::~PluginManager() = default;

std::string PluginManager::activateNative(PluginInfo& plugin) {
    const std::filesystem::path path = nativeLibraryPath(plugin.manifest);
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return "Its C++ library " + toUtf8(path) + " doesn't exist.";
    const LoadedLibrary library = loadLibrary(path);
    if (!library.symbol) return "Its C++ library can't be loaded: " + library.error;
    const auto activateFunction = reinterpret_cast<usdcc::plugin::ActivateFunction>(library.symbol("usdcc_plugin_activate"));
    if (!activateFunction) return "Its C++ library has no usdcc_plugin_activate().";

    // It lives as long as this manager, as the plugin's callbacks may hold it.
    auto native = std::make_unique<NativePlugin>();
    usdcc::plugin::Host& host = native->host;
    host.id = plugin.manifest.id;
    host.name = plugin.manifest.name;
    host.version = plugin.manifest.version;
    host.folder = plugin.manifest.folder;
    if (const AppContext* context = currentAppContext()) {
        host.stageManager = context->stageManager;
        host.undoManager = context->undoManager;
        host.toolSet = context->toolSet;
        host.mainWindow = context->mainWindow;
        host.viewPanels = context->viewPanels;
    }
    host.note = [this, id = plugin.manifest.id](const std::string& kind, const std::string& name) {
        addContribution(id, kind, name);
    };
    native->deactivate = reinterpret_cast<usdcc::plugin::DeactivateFunction>(library.symbol("usdcc_plugin_deactivate"));
    usdcc::plugin::Host* hostPointer = &host;
    m_native[plugin.manifest.id] = std::move(native);
    try {
        activateFunction(*hostPointer);
    } catch (const std::exception& failure) {
        return std::string("usdcc_plugin_activate() threw: ") + failure.what();
    } catch (...) {
        return "usdcc_plugin_activate() threw.";
    }
    return {};
}

PluginInfo* PluginManager::findMutable(const std::string& id) {
    const auto it = std::find_if(m_plugins.begin(), m_plugins.end(),
                                 [&id](const PluginInfo& plugin) { return plugin.manifest.id == id; });
    return it == m_plugins.end() ? nullptr : &*it;
}

const PluginInfo* PluginManager::find(const std::string& id) const {
    return const_cast<PluginManager*>(this)->findMutable(id);
}

void PluginManager::readManifests() {
    std::error_code error;
    for (const std::filesystem::path& root : m_options.folders) {
        std::vector<std::filesystem::path> folders;
        for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
            if (entry.is_regular_file(error) || !std::filesystem::exists(entry.path() / kManifestName, error)) continue;
            folders.push_back(entry.path());
        }
        std::sort(folders.begin(), folders.end());
        for (const std::filesystem::path& folder : folders) {
            PluginInfo plugin;
            plugin.manifest.folder = folder;
            const std::string problem = parseManifest(folder / kManifestName, plugin.manifest);
            if (!problem.empty()) {
                plugin.state = PluginState::Refused;
                plugin.reason = problem;
                if (plugin.manifest.id.empty()) plugin.manifest.id = toUtf8(folder.filename());
                if (plugin.manifest.name.empty()) plugin.manifest.name = plugin.manifest.id;
                m_unusable.insert(plugin.manifest.id);
                USDCC_LOG_WARNING("usdcc.plugins", "{} ({}): {}", plugin.manifest.name, toUtf8(folder), problem);
            }
            if (const PluginInfo* first = find(plugin.manifest.id)) {
                USDCC_LOG_WARNING("usdcc.plugins", "Ignoring plugin {} in {}: {} has the same id", plugin.manifest.id,
                                  toUtf8(folder), toUtf8(first->manifest.folder));
                continue;
            }
            m_plugins.push_back(std::move(plugin));
        }
    }
}

void PluginManager::applySettings() {
    // A plugin that was being activated when the last start ended.
    std::error_code error;
    if (!m_options.crashMarker.empty() && std::filesystem::exists(m_options.crashMarker, error)) {
        std::ifstream input(m_options.crashMarker, std::ios::binary);
        std::string id;
        std::getline(input, id);
        input.close();
        removeCrashMarker();
        if (!id.empty()) {
            m_crashedLastStart = id;
            QStringList disabled = settingList(kDisabledKey);
            QStringList crashed = settingList(kCrashedKey);
            const QString qid = QString::fromStdString(id);
            if (!disabled.contains(qid)) disabled << qid;
            if (!crashed.contains(qid)) crashed << qid;
            setSettingList(kDisabledKey, disabled);
            setSettingList(kCrashedKey, crashed);
            USDCC_LOG_ERROR("usdcc.plugins", "The last start ended while activating plugin {}: it's now disabled", id);
        }
    }

    const QStringList disabled = settingList(kDisabledKey);
    const QStringList crashed = settingList(kCrashedKey);
    for (PluginInfo& plugin : m_plugins) {
        const QString id = QString::fromStdString(plugin.manifest.id);
        if (plugin.state == PluginState::Ready && disabled.contains(id)) {
            plugin.state = PluginState::Disabled;
            plugin.reason = crashed.contains(id) ? "It crashed the app while starting, so it was turned off." : "";
        }
    }
}

void PluginManager::orderAndCheckDependencies() {
    // Dependencies first, otherwise by id, so the order doesn't depend on the
    // folder a plugin is in. A cycle's members are refused.
    std::sort(m_plugins.begin(), m_plugins.end(),
              [](const PluginInfo& a, const PluginInfo& b) { return a.manifest.id < b.manifest.id; });
    std::vector<PluginInfo> ordered;
    std::set<std::string> visited;
    std::vector<std::string> stack;
    std::set<std::string> inCycle;
    std::function<void(PluginInfo&)> visit = [&](PluginInfo& plugin) {
        const std::string& id = plugin.manifest.id;
        const auto onStack = std::find(stack.begin(), stack.end(), id);
        if (onStack != stack.end()) {
            inCycle.insert(onStack, stack.end());
            return;
        }
        if (!visited.insert(id).second) return;
        stack.push_back(id);
        for (const std::string& dependency : plugin.manifest.dependsOn) {
            if (PluginInfo* other = findMutable(dependency)) visit(*other);
        }
        stack.pop_back();
        ordered.push_back(plugin);
    };
    for (PluginInfo& plugin : m_plugins) visit(plugin);
    m_plugins = std::move(ordered);

    for (PluginInfo& plugin : m_plugins) {
        if (plugin.state == PluginState::Ready && inCycle.count(plugin.manifest.id)) {
            plugin.state = PluginState::Refused;
            plugin.reason = "Its \"requires\" go round in a circle.";
        }
    }
    refuseDependents();
}

void PluginManager::refuseDependents() {
    // In activation order, so that a refusal carries on down a chain.
    for (PluginInfo& plugin : m_plugins) {
        if (plugin.state != PluginState::Ready && plugin.state != PluginState::Active) continue;
        for (const std::string& dependency : plugin.manifest.dependsOn) {
            const PluginInfo* other = find(dependency);
            std::string why;
            if (!other) {
                why = "It requires " + dependency + ", which isn't installed.";
            } else if (other->state == PluginState::Disabled) {
                why = "It requires " + other->manifest.name + ", which is turned off.";
            } else if (other->state == PluginState::Refused || other->state == PluginState::NeedsPackages ||
                       other->state == PluginState::Failed) {
                why = "It requires " + other->manifest.name + ", which didn't load.";
            }
            if (!why.empty() && plugin.state == PluginState::Ready) {
                plugin.state = PluginState::Refused;
                plugin.reason = why;
                USDCC_LOG_WARNING("usdcc.plugins", "{}: {}", plugin.manifest.name, why);
                break;
            }
        }
    }
}

void PluginManager::registerUsdFolders() {
    std::vector<std::string> folders;
    for (const PluginInfo& plugin : m_plugins) {
        if (plugin.state != PluginState::Ready) continue;
        for (const std::string& folder : plugin.manifest.usdFolders) {
            const std::filesystem::path path = plugin.manifest.folder / std::filesystem::u8path(folder);
            std::error_code error;
            if (std::filesystem::exists(path, error)) {
                folders.push_back(toUtf8(path));
            } else {
                USDCC_LOG_WARNING("usdcc.plugins", "{}: its USD folder {} doesn't exist", plugin.manifest.name, toUtf8(path));
            }
        }
    }
    if (folders.empty()) return;
    const PXR_NS::PlugPluginPtrVector registered = PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(folders);
    USDCC_LOG_INFO("usdcc.plugins", "Registered {} USD plugins from {} folders", registered.size(), folders.size());
}

void PluginManager::scan() {
    if (!m_options.enabled) {
        USDCC_LOG_INFO("usdcc.plugins", "Plugins skipped (--no-plugins)");
        return;
    }
    readManifests();
    applySettings();
    orderAndCheckDependencies();
    registerUsdFolders();
    USDCC_LOG_INFO("usdcc.plugins", "Found {} plugins", m_plugins.size());
}

void PluginManager::writeCrashMarker(const std::string& id) const {
    if (m_options.crashMarker.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(m_options.crashMarker.parent_path(), error);
    std::ofstream(m_options.crashMarker, std::ios::binary) << id;
}

void PluginManager::removeCrashMarker() const {
    std::error_code error;
    if (!m_options.crashMarker.empty()) std::filesystem::remove(m_options.crashMarker, error);
}

void PluginManager::activate(PythonRuntime& python) {
    const auto needsPython = [](const PluginInfo& plugin) {
        return plugin.state == PluginState::Ready && !plugin.manifest.pythonModule.empty();
    };
    const bool anyPython = std::any_of(m_plugins.begin(), m_plugins.end(), needsPython);

    if (anyPython && !python.isAvailable()) {
        for (PluginInfo& plugin : m_plugins) {
            if (!needsPython(plugin)) continue;
            plugin.state = PluginState::Refused;
            plugin.reason = "It needs Python, which isn't available: " + python.unavailableReason();
        }
        refuseDependents();
    }

#if USDCC_HAS_PYTHON
    // Whether or not a plugin needs them: scripts use these folders too.
    if (python.isAvailable()) {
        std::error_code error;
        m_packageFolders.clear();
        for (const auto& folder : m_options.pythonPackageFolders) {
            if (std::filesystem::is_directory(folder, error)) {
                python.addToPath(folder);
                m_packageFolders.push_back(folder);
            }
        }
        if (!m_options.packagesRoot.empty()) {
            m_environment = std::make_unique<PackageEnvironment>(m_options.packagesRoot, PackageEnvironment::pythonTag(python.version()));
            if (const auto active = m_environment->activeFolder()) {
                python.addToPath(*active);
                m_packageFolders.push_back(*active);
                m_environment->markInUse();
                m_sharedFolder = toUtf8(*active);
            }
            m_environment->removeUnused();
        }
    }
    if (anyPython && python.isAvailable()) {
        std::vector<PluginInfo*> ready;
        for (PluginInfo& plugin : m_plugins) {
            if (needsPython(plugin)) ready.push_back(&plugin);
        }
        checkPythonModules(ready, python);
    }
    refuseDependents();
#endif

    for (PluginInfo& plugin : m_plugins) {
        if (plugin.state != PluginState::Ready) continue;
        refuseDependents();  // one activated just before this may have failed
        if (plugin.state != PluginState::Ready) continue;
        activateOne(plugin);
    }
}

void PluginManager::checkPythonModules(const std::vector<PluginInfo*>& plugins, PythonRuntime& python) {
#if USDCC_HAS_PYTHON
    namespace py = pybind11;
    const EnvironmentRecord record = m_environment ? m_environment->record() : EnvironmentRecord();
    py::gil_scoped_acquire gil;
    try {
        py::list specs;
        const auto addSpec = [&specs](const PluginInfo& plugin, bool loaded) {
            py::dict spec;
            spec["id"] = plugin.manifest.id;
            spec["name"] = plugin.manifest.name;
            spec["module"] = plugin.manifest.pythonModule;
            spec["folder"] = toUtf8(plugin.manifest.folder);
            spec["requires"] = plugin.manifest.pythonRequires;
            spec["loaded"] = loaded;
            specs.append(spec);
        };
        // Those running stay in the picture, for the packages they bring.
        for (const PluginInfo& plugin : m_plugins) {
            if (plugin.state == PluginState::Active && !plugin.manifest.pythonModule.empty()) addSpec(plugin, true);
        }
        for (const PluginInfo* plugin : plugins) addSpec(*plugin, false);
        const auto refused = py::module_::import("usdcc._plugins")
                                 .attr("check_modules")(specs, m_sharedFolder)
                                 .cast<std::map<std::string, std::pair<std::string, bool>>>();
        for (const auto& [id, verdict] : refused) {
            PluginInfo* plugin = findMutable(id);
            if (!plugin || plugin->state != PluginState::Ready) continue;
            plugin->state = verdict.second ? PluginState::NeedsPackages : PluginState::Refused;
            plugin->reason = verdict.first;
            // An install tried this already, and it can't be done. Don't keep
            // offering it, unless the plugin now asks for something else.
            for (const LeftOutPackages& left : record.leftOut) {
                if (verdict.second && left.id == id && left.declared == plugin->manifest.pythonRequires) {
                    plugin->state = PluginState::Refused;
                    plugin->reason = "Its packages couldn't be installed. " + left.why;
                }
            }
            USDCC_LOG_WARNING("usdcc.plugins", "{}: {}", plugin->manifest.name, plugin->reason);
        }
    } catch (py::error_already_set& failure) {
        // The app's own Python is at fault, not any one plugin's.
        USDCC_LOG_ERROR("usdcc.plugins", "Checking the plugins' Python modules failed: {}", failure.what());
        for (PluginInfo* plugin : plugins) {
            plugin->state = PluginState::Refused;
            plugin->reason = "Its Python modules couldn't be checked; the Log panel has the details.";
        }
    }
#else
    (void)plugins;
    (void)python;
#endif
}

void PluginManager::activateOne(PluginInfo& plugin) {
    if (plugin.manifest.pythonModule.empty() && plugin.manifest.nativeLibrary.empty()) {
        plugin.state = PluginState::Active;
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    writeCrashMarker(plugin.manifest.id);
    std::string failure;
    if (!plugin.manifest.nativeLibrary.empty()) failure = activateNative(plugin);
#if USDCC_HAS_PYTHON
    if (failure.empty() && !plugin.manifest.pythonModule.empty()) {
        namespace py = pybind11;
        py::gil_scoped_acquire gil;
        try {
            failure = py::module_::import("usdcc._plugins")
                          .attr("activate")(plugin.manifest.id, plugin.manifest.name, plugin.manifest.version,
                                            plugin.manifest.pythonModule, toUtf8(plugin.manifest.folder))
                          .cast<std::string>();
        } catch (py::error_already_set& error) {
            failure = error.what();
        }
    }
#endif
    removeCrashMarker();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (failure.empty()) {
        plugin.state = PluginState::Active;
        USDCC_LOG_INFO("usdcc.plugins", "Activated {} {} in {} ms", plugin.manifest.name, plugin.manifest.version, ms);
    } else {
        plugin.state = PluginState::Failed;
        plugin.reason = failure;
        USDCC_LOG_ERROR("usdcc.plugins", "{} failed to activate: {}", plugin.manifest.name, failure);
    }
}

std::string PluginManager::whyNotReloadable(const std::string& id) const {
    const PluginInfo* plugin = find(id);
    if (!plugin) return "There is no such plugin.";
    if (plugin->state == PluginState::Disabled) return "It's turned off.";
    if (!plugin->manifest.nativeLibrary.empty()) return "It has a C++ part, whose library can't be unloaded. It needs a restart.";
    // A manifest that was broken may be fixed by now, which only reading it will tell.
    if (plugin->manifest.pythonModule.empty() && !m_unusable.count(id)) return "It has no Python to reload.";
    const PythonRuntime* python = PythonRuntime::instance();
    if (!python || !python->isAvailable()) return "Python isn't available.";
    return {};
}

PluginManager::ReloadResult PluginManager::reload(const std::string& id, PythonRuntime& python) {
    ReloadResult result;
#if USDCC_HAS_PYTHON
    namespace py = pybind11;
    PluginInfo* target = findMutable(id);
    if (!target) return result;
    if (const std::string why = whyNotReloadable(id); !why.empty()) {
        result.notes.push_back(why);
        return result;
    }
    if (m_reloading) {
        result.notes.push_back("A reload is running.");
        return result;
    }
    m_reloading = true;

    // It and what requires it, however far, in activation order: those may hold its classes.
    std::vector<PluginInfo*> chain{target};
    for (PluginInfo& plugin : m_plugins) {
        if (&plugin == target || plugin.state == PluginState::Disabled) continue;
        for (const std::string& dependency : plugin.manifest.dependsOn) {
            const bool inChain = std::any_of(chain.begin(), chain.end(), [&](const PluginInfo* each) { return each->manifest.id == dependency; });
            if (inChain) {
                if (plugin.manifest.nativeLibrary.empty()) {
                    chain.push_back(&plugin);
                } else {
                    result.notes.push_back(plugin.manifest.name + " requires it, but has a C++ part, so it goes on using what it had until a restart.");
                }
                break;
            }
        }
    }
    std::stable_sort(chain.begin(), chain.end());  // m_plugins is in activation order

    usdcc::tools::ToolSet* tools = currentAppContext() ? currentAppContext()->toolSet : nullptr;
    const std::string activeTool = tools ? tools->activeId() : std::string();

    std::vector<std::string> panelTypes;
    {
        py::gil_scoped_acquire gil;
        py::module_ module = py::module_::import("usdcc._plugins");
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            PluginInfo& plugin = **it;
            if (plugin.manifest.pythonModule.empty()) continue;
            try {
                module.attr("unload")(plugin.manifest.id, plugin.manifest.pythonModule, toUtf8(plugin.manifest.folder));
            } catch (py::error_already_set& error) {
                USDCC_LOG_ERROR("usdcc.plugins", "Unloading {} failed: {}", plugin.manifest.name, error.what());
            }
        }
    }

    for (PluginInfo* plugin : chain) {
        result.names.push_back(plugin->manifest.name);
        // What it says now. USD files and C++ libraries are only taken in at a start.
        PluginManifest fresh;
        fresh.folder = plugin->manifest.folder;
        const std::string problem = parseManifest(fresh.folder / kManifestName, fresh);
        plugin->contributions.clear();
        if (!problem.empty()) {
            plugin->state = PluginState::Refused;
            plugin->reason = problem;
            m_unusable.insert(plugin->manifest.id);
            USDCC_LOG_WARNING("usdcc.plugins", "{}: {}", plugin->manifest.name, problem);
            continue;
        }
        if (fresh.id != plugin->manifest.id) {
            plugin->state = PluginState::Refused;
            plugin->reason = "Its manifest's id changed, from " + plugin->manifest.id + " to " + fresh.id + ". That takes effect at the next start.";
            continue;
        }
        m_unusable.erase(plugin->manifest.id);
        if (fresh.usdFolders != plugin->manifest.usdFolders || fresh.nativeLibrary != plugin->manifest.nativeLibrary) {
            result.notes.push_back(fresh.name + ": a change to its USD or C++ parts takes effect at the next start.");
        }
        plugin->manifest = std::move(fresh);
        plugin->state = PluginState::Ready;
        plugin->reason.clear();
    }

    std::vector<PluginInfo*> ready;
    for (PluginInfo* plugin : chain) {
        if (plugin->state == PluginState::Ready && !plugin->manifest.pythonModule.empty()) ready.push_back(plugin);
    }
    checkPythonModules(ready, python);
    refuseDependents();

    for (PluginInfo* plugin : chain) {
        refuseDependents();  // one just before this may have failed
        if (plugin->state == PluginState::Ready) activateOne(*plugin);
    }

    {
        py::gil_scoped_acquire gil;
        py::module_ module = py::module_::import("usdcc._plugins");
        for (PluginInfo* plugin : chain) {
            for (const std::string& type : module.attr("panel_types")(plugin->manifest.id).cast<std::vector<std::string>>()) {
                panelTypes.push_back(type);
            }
            if (plugin->state == PluginState::Active && !plugin->manifest.pythonModule.empty() &&
                !module.attr("has_deactivate")(plugin->manifest.id).cast<bool>()) {
                result.notes.push_back(plugin->manifest.name +
                                       " has no deactivate(): anything it set up directly, such as a QTimer, is still running the old code.");
            }
        }
    }
    rebuildScriptedPanels(panelTypes);
    if (tools && !activeTool.empty() && tools->tool(activeTool)) tools->setActive(activeTool);

    result.ok = target->state == PluginState::Active;
    if (!result.ok) result.failure = target->reason;
    for (const PluginInfo* plugin : chain) {
        if (plugin != target && plugin->state != PluginState::Active) result.notes.push_back(plugin->manifest.name + " isn't running: " + plugin->reason);
    }
    m_reloading = false;
#else
    (void)id;
    (void)python;
#endif
    return result;
}

std::string PluginManager::createPlugin(const std::string& name, const std::filesystem::path& parent, PythonRuntime& python, std::string* error) {
    const QString shown = QString::fromStdString(name).trimmed();
    // What's safe in code and a menu path, as the name goes in both.
    QString title = shown;
    title.remove(QRegularExpression(QStringLiteral("[\"\\\\\\r\\n]"))).replace(QLatin1Char('/'), QLatin1Char(' '));
    title = title.trimmed();
    QString slug = title.toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    slug.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
    if (slug.isEmpty()) {
        if (error) *error = "A plugin's name needs some letters or digits.";
        return {};
    }

    std::error_code fileError;
    QString folderName = slug;
    QString id = QStringLiteral("user.%1").arg(slug);
    for (int n = 2; std::filesystem::exists(parent / folderName.toStdU16String(), fileError) || find(id.toStdString()); ++n) {
        folderName = QStringLiteral("%1_%2").arg(slug).arg(n);
        id = QStringLiteral("user.%1").arg(folderName);
    }
    // "_plugin" keeps it clear of what else Python can import, and a digit from the front.
    const QString module = QStringLiteral("%1_plugin").arg(folderName.at(0).isDigit() ? QStringLiteral("p") + folderName : folderName);
    const std::filesystem::path folder = parent / folderName.toStdU16String();
    const std::filesystem::path package = folder / "python" / module.toStdU16String();
    std::filesystem::create_directories(package, fileError);
    if (fileError) {
        if (error) *error = "Can't create " + toUtf8(package) + ": " + fileError.message();
        return {};
    }

    const QJsonObject manifest{{"id", id},
                               {"name", shown},
                               {"version", "0.1.0"},
                               {"description", ""},
                               {"api", kPluginApiVersion},
                               {"python", QJsonObject{{"module", module}, {"requires", QJsonArray()}}}};
    QFile manifestFile(toQString(folder / kManifestName));
    QFile codeFile(toQString(package / "__init__.py"));
    const QString code = QStringLiteral(
                             "\"\"\"%1: a plugin for usdcc (docs/PLUGINS.md).\"\"\"\n\n"
                             "import usdcc.app\n\n\n"
                             "def say_hello():\n"
                             "    print(\"Hello from %1\")\n\n\n"
                             "def activate(plugin):\n"
                             "    \"\"\"Runs at startup and at each reload. What you register with usdcc is taken\n"
                             "    back by itself at a reload: menu actions, tools, panels, events.\"\"\"\n"
                             "    usdcc.app.add_menu_action(\"Tools/%1/Say Hello\", say_hello)\n\n\n"
                             "def deactivate(plugin):\n"
                             "    \"\"\"Runs at quit and before each reload. Stop here what you started yourself:\n"
                             "    a QTimer, a thread, a Qt signal you connected.\"\"\"\n")
                             .arg(title);
    if (!manifestFile.open(QIODevice::WriteOnly) || manifestFile.write(QJsonDocument(manifest).toJson()) < 0 ||
        !codeFile.open(QIODevice::WriteOnly) || codeFile.write(code.toUtf8()) < 0) {
        if (error) *error = "Can't write the plugin's files in " + toUtf8(folder) + ".";
        return {};
    }
    manifestFile.close();
    codeFile.close();

    PluginInfo plugin;
    plugin.manifest.folder = folder;
    if (const std::string problem = parseManifest(folder / kManifestName, plugin.manifest); !problem.empty()) {
        if (error) *error = problem;
        return {};
    }
    const std::string newId = plugin.manifest.id;
    m_plugins.push_back(std::move(plugin));
    reload(newId, python);
    return newId;
}

void PluginManager::deactivate(PythonRuntime& python) {
    for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it) {
        if (it->state != PluginState::Active) continue;
#if USDCC_HAS_PYTHON
        if (!it->manifest.pythonModule.empty() && python.isAvailable()) {
            namespace py = pybind11;
            py::gil_scoped_acquire gil;
            try {
                py::module_::import("usdcc._plugins").attr("deactivate")(it->manifest.id);
            } catch (py::error_already_set& error) {
                USDCC_LOG_ERROR("usdcc.plugins", "{} failed to deactivate: {}", it->manifest.name, error.what());
            }
        }
#else
        (void)python;
#endif
        const auto native = m_native.find(it->manifest.id);
        if (native != m_native.end() && native->second->deactivate) {
            try {
                native->second->deactivate(native->second->host);
            } catch (const std::exception& error) {
                USDCC_LOG_ERROR("usdcc.plugins", "{} failed to deactivate: {}", it->manifest.name, error.what());
            } catch (...) {
                USDCC_LOG_ERROR("usdcc.plugins", "{} failed to deactivate", it->manifest.name);
            }
        }
    }
}

bool PluginManager::isEnabled(const std::string& id) const {
    return !settingList(kDisabledKey).contains(QString::fromStdString(id));
}

void PluginManager::setEnabled(const std::string& id, bool enabled) {
    const QString qid = QString::fromStdString(id);
    QStringList disabled = settingList(kDisabledKey);
    disabled.removeAll(qid);
    if (!enabled) disabled << qid;
    setSettingList(kDisabledKey, disabled);
    if (enabled) {
        QStringList crashed = settingList(kCrashedKey);
        crashed.removeAll(qid);
        setSettingList(kCrashedKey, crashed);
    }
}

bool PluginManager::hasPendingChanges() const {
    return m_packagesInstalled || std::any_of(m_plugins.begin(), m_plugins.end(), [this](const PluginInfo& plugin) {
        return isEnabled(plugin.manifest.id) == (plugin.state == PluginState::Disabled);
    });
}

void PluginManager::addContribution(const std::string& id, const std::string& kind, const std::string& name) {
    if (PluginInfo* plugin = findMutable(id)) plugin->contributions.push_back({kind, name});
}

std::vector<PackageRequest> PluginManager::packageRequests(PythonRuntime& python, std::vector<std::string>* blocked) const {
    std::vector<PackageRequest> requests;
#if USDCC_HAS_PYTHON
    namespace py = pybind11;
    if (!python.isAvailable()) return requests;
    py::gil_scoped_acquire gil;
    try {
        py::list specs;
        for (const PluginInfo& plugin : m_plugins) {
            // By the setting, not the state: one turned on just now counts.
            if (plugin.manifest.pythonRequires.empty() || m_unusable.count(plugin.manifest.id) || !isEnabled(plugin.manifest.id)) continue;
            py::dict spec;
            spec["id"] = plugin.manifest.id;
            spec["name"] = plugin.manifest.name;
            spec["folder"] = toUtf8(plugin.manifest.folder);
            spec["requires"] = plugin.manifest.pythonRequires;
            specs.append(spec);
        }
        std::vector<std::string> extraFolders;
        for (const auto& folder : m_options.pythonPackageFolders) extraFolders.push_back(toUtf8(folder));
        const py::tuple result = py::module_::import("usdcc._plugins")
                                     .attr("package_requests")(specs, usdcc::core::Settings::instance().get<std::string>(kUserPackagesKey), extraFolders);
        for (const py::handle item : result[0]) {
            const py::dict entry = py::reinterpret_borrow<py::dict>(item);
            PackageRequest request;
            request.id = entry["id"].cast<std::string>();
            request.name = entry["name"].cast<std::string>();
            request.requirements = entry["requirements"].cast<std::vector<std::string>>();
            request.problem = entry["problem"].cast<std::string>();
            if (const PluginInfo* plugin = find(request.id)) request.declared = plugin->manifest.pythonRequires;
            requests.push_back(std::move(request));
        }
        if (blocked) *blocked = result[1].cast<std::vector<std::string>>();
    } catch (py::error_already_set& failure) {
        USDCC_LOG_ERROR("usdcc.plugins", "Working out which Python packages are needed failed: {}", failure.what());
    }
#else
    (void)python;
    (void)blocked;
#endif
    return requests;
}

PackageInstaller::Options PluginManager::installerOptions(const PythonRuntime& python, bool upgrade) const {
    const usdcc::core::Settings& settings = usdcc::core::Settings::instance();
    PackageInstaller::Options options;
    options.root = m_options.packagesRoot;
    options.pythonTag = PackageEnvironment::pythonTag(python.version());
    options.python = python.settings().home / "python.exe";
    options.uv = PackageInstaller::findUv();
    options.sources.index = settings.get<std::string>(kPackageIndexKey);
    options.sources.wheels = toPath(QString::fromStdString(settings.get<std::string>(kWheelsFolderKey)));
    options.sources.offline = settings.get<bool>(kPackagesOfflineKey);
    options.userPackages = settings.get<std::string>(kUserPackagesKey);
    options.upgrade = upgrade;
    return options;
}

bool PluginManager::packagesPending() const {
    if (std::any_of(m_plugins.begin(), m_plugins.end(), [](const PluginInfo& plugin) { return plugin.state == PluginState::NeedsPackages; })) {
        return true;
    }
    if (m_options.packagesRoot.empty()) return false;
    const QString wanted = QString::fromStdString(usdcc::core::Settings::instance().get<std::string>(kUserPackagesKey)).trimmed();
    if (wanted.isEmpty()) return false;
    return !m_environment || wanted != QString::fromStdString(m_environment->record().userPackages).trimmed();
}

int PluginManager::installPackages(PythonRuntime& python, const std::function<void(const std::string&)>& report) {
    if (!python.isAvailable()) {
        report("Python isn't available: " + python.unavailableReason());
        return 1;
    }
    if (m_options.packagesRoot.empty()) {
        report("There is no shared packages folder.");
        return 1;
    }
    std::vector<std::string> blocked;
    std::vector<PackageRequest> requests = packageRequests(python, &blocked);
    PackageInstaller::Options options = installerOptions(python, false);
    options.blocked = std::move(blocked);
    PackageInstaller installer(std::move(options));

    QEventLoop loop;
    int result = 0;
    QObject::connect(&installer, &PackageInstaller::output, &loop, [&report](const QString& line) { report(line.toStdString()); });
    QObject::connect(&installer, &PackageInstaller::failed, &loop, [&](const QString& reason) {
        report("Failed: " + reason.toStdString());
        result = 1;
        loop.quit();
    });
    QObject::connect(&installer, &PackageInstaller::resolved, &loop, [&](const PackagePlan& plan) {
        for (const LeftOutPackages& left : plan.leftOut) {
            report("Left out " + left.name + ": " + left.why);
            result = 2;
        }
        if (plan.unchanged) {
            report("Everything is up to date.");
            loop.quit();
            return;
        }
        installer.install();
    });
    QObject::connect(&installer, &PackageInstaller::installed, &loop, [&](const QString& folder) {
        report("Installed in " + folder.toStdString());
        loop.quit();
    });
    // resolved() can come before the loop runs only when it fails at once, and then
    // nothing is left to wait for.
    installer.resolve(std::move(requests));
    if (installer.isBusy()) loop.exec();
    return result;
}

}  // namespace usdcc::scripting
