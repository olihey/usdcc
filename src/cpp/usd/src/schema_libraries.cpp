#include "usdcc/usd/schema_libraries.h"

#include "usdcc/assets/paths.h"
#include "usdcc/usd/schema_info.h"

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/primDefinition.h>
#include <pxr/usd/usd/property.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {

std::filesystem::path pathOf(const std::string& text) { return usdcc::assets::fromUtf8(text); }
std::string textOf(const std::filesystem::path& path) { return usdcc::assets::toUtf8(path); }

long long modified(const std::filesystem::path& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<long long>(time.time_since_epoch().count());
}

bool samePath(const std::string& a, const std::string& b) {
    std::error_code error;
    return std::filesystem::equivalent(pathOf(a), pathOf(b), error);
}

// usd/<name>/schema.usda in each folder, by folder.
std::vector<std::pair<std::string, std::string>> librariesIn(const std::vector<std::string>& usdFolders) {
    std::vector<std::pair<std::string, std::string>> found;  // (usd folder, name)
    std::error_code error;
    for (const std::string& folder : usdFolders) {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(pathOf(folder), error)) {
            if (entry.is_directory(error) && std::filesystem::is_regular_file(entry.path() / "schema.usda", error)) {
                found.emplace_back(folder, textOf(entry.path().filename()));
            }
        }
    }
    return found;
}

// Codeless plugins USD registered from elsewhere (PXR_PLUGINPATH_NAME) that ship their source.
std::vector<std::string> registeredCodelessFolders() {
    std::vector<std::string> folders;
    std::error_code error;
    for (const PlugPluginPtr& plugin : PlugRegistry::GetInstance().GetAllPlugins()) {
        if (!plugin->IsResource() || plugin->GetResourcePath().empty()) continue;
        if (std::filesystem::is_regular_file(pathOf(plugin->GetResourcePath()) / pathOf(plugin->GetName()) / "schema.usda", error) &&
            std::filesystem::is_regular_file(pathOf(plugin->GetResourcePath()) / "generatedSchema.usda", error)) {
            folders.push_back(plugin->GetResourcePath());
        }
    }
    return folders;
}

}  // namespace

SchemaLibraries& SchemaLibraries::instance() {
    static SchemaLibraries libraries;
    return libraries;
}

void SchemaLibraries::configure(std::vector<std::string> usdFolders, std::string userPluginsFolder, int pluginApi) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_usdFolders = std::move(usdFolders);
    m_userPluginsFolder = std::move(userPluginsFolder);
    m_pluginApi = pluginApi;
    m_generatedAtStart.clear();
    std::vector<std::string> all = m_usdFolders;
    for (const std::string& folder : registeredCodelessFolders()) all.push_back(folder);
    for (const auto& [folder, name] : librariesIn(all)) {
        m_generatedAtStart[folder] = modified(pathOf(generatedSchemaPath(folder)));
    }
}

std::string SchemaLibraries::userPluginsFolder() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_userPluginsFolder;
}

SchemaLibraryLocation SchemaLibraries::locate(const std::string& usdFolder, const std::string& name) const {
    SchemaLibraryLocation location;
    location.name = name;
    location.usdFolder = usdFolder;
    const std::filesystem::path folder = pathOf(usdFolder);
    std::error_code error;
    if (std::filesystem::is_regular_file(folder.parent_path() / "usdcc-plugin.json", error)) {
        location.pluginFolder = textOf(folder.parent_path());
    }
    const std::filesystem::path source = pathOf(location.source());
    const std::filesystem::path generated = pathOf(generatedSchemaPath(usdFolder));
    location.generated = std::filesystem::is_regular_file(generated, error) &&
                         std::filesystem::is_regular_file(pathOf(plugInfoPath(usdFolder)), error);
    {
        // Opening to append changes nothing, and says whether it could be written.
        std::ofstream probe(source, std::ios::app);
        location.writable = probe.is_open();
    }
    const PlugPluginPtr plugin = PlugRegistry::GetInstance().GetPluginWithName(name);
    location.loaded = plugin && samePath(plugin->GetResourcePath(), usdFolder);
    const long long generatedTime = location.generated ? modified(generated) : 0;
    location.changed = !location.generated || modified(source) > generatedTime;
    const auto atStart = m_generatedAtStart.find(usdFolder);
    location.waitingForRestart =
        location.generated && (!location.loaded || atStart == m_generatedAtStart.end() || atStart->second != generatedTime);
    return location;
}

std::vector<SchemaLibraryLocation> SchemaLibraries::libraries() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> folders = m_usdFolders;
    for (const std::string& folder : registeredCodelessFolders()) {
        if (std::none_of(folders.begin(), folders.end(), [&](const std::string& f) { return samePath(f, folder); })) {
            folders.push_back(folder);
        }
    }
    std::vector<SchemaLibraryLocation> result;
    std::set<std::string> names;
    for (const auto& [folder, name] : librariesIn(folders)) {
        if (names.insert(name).second) result.push_back(locate(folder, name));  // the first of a name wins, as for plugins
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}

std::optional<SchemaLibraryLocation> SchemaLibraries::find(const std::string& name) const {
    for (const SchemaLibraryLocation& location : libraries()) {
        if (location.name == name) return location;
    }
    return std::nullopt;
}

bool SchemaLibraries::create(const std::string& name, SchemaLibraryLocation* location, std::string* error) {
    auto fail = [error](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!TfIsValidIdentifier(name)) return fail("'" + name + "' isn't a valid name: letters, digits and _, not starting with a digit");
    if (find(name)) return fail("There's a library called " + name + " already");
    if (PlugRegistry::GetInstance().GetPluginWithName(name)) return fail("USD has a plugin called " + name + " already");
    const std::string user = userPluginsFolder();
    if (user.empty()) return fail("No plugins folder to put it in");
    const std::filesystem::path plugin = pathOf(user) / pathOf(name);
    std::error_code ec;
    if (std::filesystem::exists(plugin, ec)) return fail("There's a folder " + textOf(plugin) + " already");
    std::filesystem::create_directories(plugin / "usd", ec);
    if (ec) return fail("Can't make " + textOf(plugin) + ": " + ec.message());
    {
        std::ofstream manifest(plugin / "usdcc-plugin.json", std::ios::binary);
        manifest << "{\n"
                 << "    \"id\": \"user.schemas." << name << "\",\n"
                 << "    \"name\": \"" << name << " schemas\",\n"
                 << "    \"version\": \"0.1.0\",\n"
                 << "    \"description\": \"Codeless USD schemas, made in usdcc's Schemas panel\",\n"
                 << "    \"api\": " << m_pluginApi << ",\n"
                 << "    \"usd\": [\"usd\"]\n"
                 << "}\n";
        if (!manifest) return fail("Can't write its manifest in " + textOf(plugin));
    }
    SchemaLibraryDef library;
    library.name = name;
    library.sublayers = {"usd/schema.usda", "usdGeom/schema.usda"};
    const std::string usdFolder = textOf(plugin / "usd");
    std::string why;
    if (!writeSchemaLibrary(library, schemaSourcePath(usdFolder, name), &why)) return fail(why);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_usdFolders.push_back(usdFolder);
    }
    if (location) *location = *find(name);
    return true;
}

std::vector<std::string> SchemaLibraries::baseFolders(const SchemaLibraryDef& library) const {
    std::vector<std::string> folders;
    for (const std::string& sublayer : library.sublayers) {
        const std::string first = sublayer.substr(0, sublayer.find('/'));
        if (first == library.name) continue;
        if (const std::optional<SchemaLibraryLocation> base = find(first)) folders.push_back(base->usdFolder);
    }
    return folders;
}

std::vector<std::string> schemaLibraryChanges(const SchemaLibraryDef& library) {
    std::vector<std::string> changes;
    std::map<std::string, const SchemaClassDef*> classes;
    for (const SchemaClassDef& c : library.classes) classes[c.name] = &c;
    for (const SchemaInfo& loaded : allSchemas()) {
        if (loaded.plugin != library.name) continue;
        const auto next = classes.find(loaded.name);
        if (next == classes.end()) {
            changes.push_back(isTyped(loaded.kind)
                                  ? loaded.name + " is removed: its prims keep its name as their type, with no schema"
                                  : loaded.name + " is removed: where it's applied it stays, not registered");
            continue;
        }
        const SchemaClassDef& c = *next->second;
        // The new class's properties by the name USD gives them.
        std::map<std::string, const SchemaPropertyDef*> properties;
        for (const SchemaPropertyDef& p : c.properties) {
            const std::string name = c.kind == SchemaInfo::Kind::MultipleApplyAPI && !c.propertyNamespacePrefix.empty()
                                         ? c.propertyNamespacePrefix + ":__INSTANCE_NAME__:" + p.name
                                         : p.name;
            properties[name] = &p;
        }
        for (const SchemaProperty& old : loaded.properties) {
            if (old.declaredBy != loaded.name) continue;  // another schema's
            const auto now = properties.find(old.name);
            const std::string where = loaded.name + "." + old.name;
            if (now == properties.end()) {
                changes.push_back(where + " is removed: values authored for it stay, as attributes no schema defines");
                continue;
            }
            const std::string oldType = old.relationship ? std::string("relationship") : old.typeName;
            const std::string newType = now->second->relationship ? std::string("relationship") : now->second->typeName;
            if (oldType != newType) {
                changes.push_back(where + " changes from " + oldType + " to " + newType +
                                  ": values authored as " + oldType + " no longer match");
            } else if (!old.relationship && old.fallback != now->second->fallback) {
                const std::string from = old.fallback.empty() ? std::string("none") : old.fallback;
                const std::string to = now->second->fallback.empty() ? std::string("none") : now->second->fallback;
                changes.push_back(where + "'s fallback changes from " + from + " to " + to +
                                  ": prims that don't author it read the new one");
            }
        }
    }
    return changes;
}

}  // namespace usdcc::usd

namespace usdcc::usd {

std::vector<SchemaPropertyDef> promotableProperties(const UsdPrim& prim) {
    std::vector<SchemaPropertyDef> properties;
    if (!prim) return properties;
    const UsdPrimDefinition& definition = prim.GetPrimDefinition();
    for (const UsdProperty& property : prim.GetAuthoredProperties()) {
        const TfToken name = property.GetName();
        if (definition.GetPropertyDefinition(name)) continue;  // a schema's already
        if (TfStringStartsWith(name.GetString(), "xformOp:")) continue;  // the transform stack's
        SchemaPropertyDef p;
        p.name = name.GetString();
        p.documentation = property.GetDocumentation();
        p.displayName = property.GetDisplayName();
        p.displayGroup = property.GetDisplayGroup();
        if (const UsdAttribute attribute = property.As<UsdAttribute>()) {
            p.typeName = attribute.GetTypeName().GetAsToken().GetString();
            p.uniform = attribute.GetVariability() == SdfVariabilityUniform;
            VtValue value;
            if (attribute.Get(&value, UsdTimeCode::Default()) && !value.IsEmpty()) p.fallback = usdaValueText(value);
            VtTokenArray allowed;
            if (attribute.GetMetadata(SdfFieldKeys->AllowedTokens, &allowed)) {
                for (const TfToken& token : allowed) p.allowedTokens.push_back(token.GetString());
            }
        } else {
            p.relationship = true;
        }
        properties.push_back(std::move(p));
    }
    std::sort(properties.begin(), properties.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return properties;
}

std::string suggestedApiName(const SchemaLibraryDef& library, const std::vector<SchemaPropertyDef>& properties) {
    // The namespace they all share, each part capitalized.
    std::vector<std::string> shared;
    for (std::size_t i = 0; i < properties.size(); ++i) {
        std::vector<std::string> parts = TfStringSplit(properties[i].name, ":");
        parts.pop_back();
        if (i == 0) {
            shared = parts;
        } else {
            std::size_t n = 0;
            while (n < shared.size() && n < parts.size() && shared[n] == parts[n]) ++n;
            shared.resize(n);
        }
    }
    std::string stem;
    for (std::string part : shared) {
        if (!part.empty()) part[0] = char(std::toupper(static_cast<unsigned char>(part[0])));
        stem += part;
    }
    if (stem.empty() || !TfIsValidIdentifier(stem)) stem = libraryTypePrefix(library) + "Properties";
    auto taken = [&library](const std::string& name) {
        return std::any_of(library.classes.begin(), library.classes.end(), [&](const SchemaClassDef& c) { return c.name == name; }) ||
               !schemaInfo(name).name.empty();
    };
    std::string name = stem + "API";
    for (int n = 2; taken(name); ++n) name = stem + std::to_string(n) + "API";
    return name;
}

bool addApiToLibrary(const std::string& library, const std::string& name, const std::vector<SchemaPropertyDef>& properties,
                     std::string* error) {
    auto fail = [error](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::optional<SchemaLibraryLocation> location = SchemaLibraries::instance().find(library);
    if (!location) return fail("No schema library " + library);
    SchemaLibraryDef definition;
    std::string why;
    if (!readSchemaLibrary(location->source(), &definition, &why)) return fail(why);
    SchemaClassDef api;
    api.name = name;
    api.kind = SchemaInfo::Kind::SingleApplyAPI;
    api.base = "APISchemaBase";
    api.properties = properties;
    std::sort(api.properties.begin(), api.properties.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    definition.classes.push_back(api);
    std::vector<SchemaLibraryDef> bases;
    for (const std::string& folder : SchemaLibraries::instance().baseFolders(definition)) {
        SchemaLibraryDef base;
        if (findSchemaLibrary(folder, &base, nullptr)) bases.push_back(std::move(base));
    }
    for (const SchemaIssue& issue : checkSchemaLibrary(definition, bases)) {
        if (issue.severity == SchemaIssue::Severity::Error && (issue.schema == name || issue.schema.empty())) {
            return fail(issueText(issue));
        }
    }
    return writeSchemaLibrary(definition, location->source(), error);
}

}  // namespace usdcc::usd
