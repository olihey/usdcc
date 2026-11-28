#pragma once

#include "usdcc/usd/schema_library.h"

#include <pxr/usd/usd/prim.h>

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {

// The codeless schema libraries usdcc can edit (docs/SCHEMAS.md §5.1): one per
// usd folder of a usdcc plugin, as usd/<name>/schema.usda. The app tells it
// where its plugins' usd folders are; a library made with New Schema Library
// goes in the user's plugins folder, as a plugin of its own. Qt-free.
struct SchemaLibraryLocation {
    std::string name;          // libraryName, and the folder its schema.usda is in
    std::string usdFolder;     // where plugInfo.json and generatedSchema.usda go
    std::string pluginFolder;  // the usdcc plugin's (usdFolder's parent), or empty
    bool writable = false;     // its schema.usda can be written
    bool generated = false;    // generatedSchema.usda and plugInfo.json are there
    bool loaded = false;       // USD registered it at this start
    bool changed = false;      // schema.usda is newer than what was generated from it
    bool waitingForRestart = false;  // generated since this start, or not loaded
    std::string source() const { return schemaSourcePath(usdFolder, name); }
};

// What deleting a library removes: files and folders, each whole.
struct SchemaLibraryDeletion {
    std::vector<std::string> paths;
    // `paths` is its plugin's folder: the plugin is nothing but the library, as
    // New Schema Library makes it. Otherwise they are the library's own files
    // (its source's folder, plugInfo.json, generatedSchema.usda), and the rest
    // of the plugin stays.
    bool wholePlugin = false;
};

class SchemaLibraries {
public:
    static SchemaLibraries& instance();

    // The usd folders of every plugin found (enabled or not), and where a new
    // library goes. Notes when each library was generated, so a Generate after
    // this start reads as waiting for a restart.
    void configure(std::vector<std::string> usdFolders, std::string userPluginsFolder, int pluginApi);
    std::string userPluginsFolder() const;

    // Every library in those folders, by name.
    std::vector<SchemaLibraryLocation> libraries() const;
    std::optional<SchemaLibraryLocation> find(const std::string& name) const;

    // Makes a library `name` (an identifier) as a plugin of its own in the user's
    // plugins folder: <name>/usdcc-plugin.json with "usd": ["usd"], and
    // usd/<name>/schema.usda building on OpenUSD's base and geometry schemas.
    // False, with `error`, if the name is taken or isn't one.
    bool create(const std::string& name, SchemaLibraryLocation* location, std::string* error);

    // The usd folders of the libraries `library` builds on, from its sublayers
    // ("studio/schema.usda" is studio's), for generateSchemaLibrary().
    std::vector<std::string> baseFolders(const SchemaLibraryDef& library) const;

    // What deleting library `name` takes away. Nothing, with `error`, if there's
    // no such library or it can't be written.
    std::optional<SchemaLibraryDeletion> deletionOf(const std::string& name, std::string* error = nullptr) const;
    // Deletes it: each of deletionOf()'s paths through `discard` (the app's moves
    // it to the trash), or for good without one. False, with `error`, at the
    // first that fails. USD keeps its schemas until the next start
    // (removedSchemas() says so); a library made anew may have its name then.
    bool remove(const std::string& name, std::string* error = nullptr,
                const std::function<bool(const std::string& path)>& discard = {});

private:
    SchemaLibraryLocation locate(const std::string& usdFolder, const std::string& name) const;

    mutable std::mutex m_mutex;
    std::vector<std::string> m_usdFolders;
    std::string m_userPluginsFolder;
    int m_pluginApi = 1;
    std::map<std::string, long long> m_generatedAtStart;  // usd folder to generatedSchema.usda's time
};

// What generating `library` would change for files that use its schemas now
// (docs/SCHEMAS.md §5.5), against what USD loaded at this start: schemas
// removed, properties removed or changing type, fallbacks changing. Empty if
// nothing that files would notice; a library not loaded yet changes nothing.
std::vector<std::string> schemaLibraryChanges(const SchemaLibraryDef& library);

// A codeless schema USD loaded at this start that its library's files no
// longer have. USD can't unload one (docs/SCHEMAS.md §2, trap 1), so it stays
// registered, as it was, until usdcc starts again.
enum class SchemaRemoval {
    FromSource,      // schema.usda no longer has it, the generated files still do: it stays until a Generate
    AtNextStart,     // the generated files no longer have it: gone at the next start
    LibraryDeleted,  // the generated files are gone themselves: gone at the next start
};
// Which of `schemas`, loaded from the codeless library `library` in
// `usdFolder`, the files there no longer have. Those they still have aren't in
// the result; a library that ships no schema.usda has none removed from it.
std::map<std::string, SchemaRemoval> removedSchemas(const std::string& usdFolder, const std::string& library,
                                                    const std::vector<std::string>& schemas);
// The same for every codeless schema among `loaded` (allSchemas()), each
// against its plugin's folder.
std::map<std::string, SchemaRemoval> removedSchemas(const std::vector<SchemaInfo>& loaded);

// Promote to Schema (docs/SCHEMAS.md §6): the prim's authored properties no
// schema defines, as an API schema's would be (type, uniform, and the value it
// has now as the fallback), by name.
std::vector<SchemaPropertyDef> promotableProperties(const PXR_NS::UsdPrim& prim);
// The name Promote suggests for an API of `properties` in `library`: their
// shared namespace as a name ("userProperties:..." is UserPropertiesAPI), else
// the library's prefix and PropertiesAPI; made unique.
std::string suggestedApiName(const SchemaLibraryDef& library, const std::vector<SchemaPropertyDef>& properties);
// Adds single-apply API `name` with `properties` to library `library`'s
// schema.usda. False, with `error`, if it can't be read or written, or the
// result has an error the checks find.
bool addApiToLibrary(const std::string& library, const std::string& name, const std::vector<SchemaPropertyDef>& properties,
                     std::string* error);

}  // namespace usdcc::usd
