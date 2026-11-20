#pragma once

#include <string>
#include <vector>

namespace usdcc::usd {

// What USD knows about its schemas, for the Schema Explorer (docs/SCHEMAS.md
// §4): every registered schema, what it is, where it comes from and what it
// defines. Read from the schema registry and the plugins' metadata, so it
// loads no plugin library. Qt-free.

// One property a schema defines.
struct SchemaProperty {
    std::string name;  // a multiple-apply schema's as a template: "collection:__INSTANCE_NAME__:includes"
    bool relationship = false;
    std::string typeName;  // "float3", "token"; empty for a relationship
    bool uniform = false;
    bool hasFallback = false;
    std::string fallback;  // as USDA writes it: 1001, "wip", (0, 0, 0)
    std::vector<std::string> allowedTokens;
    std::string documentation;
    std::string displayName;
    std::string displayGroup;
    // The schema that declares it: the most basic typed schema that has it, or
    // the built-in API schema it comes from; the schema itself when it's its own.
    std::string declaredBy;
};

struct SchemaInfo {
    enum class Kind {
        ConcreteTyped,     // a prim type: Mesh
        AbstractTyped,     // a base for prim types: Gprim
        SingleApplyAPI,    // PhysicsRigidBodyAPI
        MultipleApplyAPI,  // CollectionAPI, applied as CollectionAPI:name
        NonAppliedAPI,     // C++ helpers only: ModelAPI
        AbstractBase,      // the roots: Typed, APISchemaBase
    };
    std::string name;      // the schema's identifier: "Mesh", "Capsule_1"
    std::string typeName;  // its TfType: "UsdGeomMesh"
    Kind kind = Kind::ConcreteTyped;
    std::string family;    // "Capsule" for Capsule and Capsule_1
    unsigned version = 0;
    std::string plugin;    // "usdGeom"
    std::string category;  // the plugin's friendly name: "Geometry"; "Core" for the usd plugin's
    bool codeless = false;  // its plugin has no library: generatedSchema.usda only
    std::string base;       // the schema it inherits; empty for a root
    std::string documentation;
    std::vector<std::string> builtInAPIs;     // applied with it, always, as its schema.usda says
    std::vector<std::string> autoAppliedAPIs;  // a type: the APIs that apply themselves to it
    std::vector<std::string> autoApplyTo;     // an API: the types it applies itself to
    std::vector<std::string> canOnlyApplyTo;  // an API: the only types it can be applied to
    std::string propertyNamespacePrefix;      // a multiple-apply API: "collection"
    std::vector<SchemaProperty> properties;   // by name
    std::string sourcePath;     // the plugin's schema.usda, or empty if it ships none
    std::string generatedPath;  // its generatedSchema.usda, or empty
};

// Every registered schema, by name.
std::vector<SchemaInfo> allSchemas();
// One schema by name ("Mesh", or "CollectionAPI:ferns" for CollectionAPI); a
// SchemaInfo with an empty name if USD doesn't know it.
SchemaInfo schemaInfo(const std::string& name);

// "Prim type", "Abstract type", "API", "Multiple-apply API", ...
std::string schemaKindName(SchemaInfo::Kind kind);
// True for the kinds a prim can be made as or have applied.
bool isTyped(SchemaInfo::Kind kind);
bool isAPI(SchemaInfo::Kind kind);

// For a plugin's schema.usda (<resources>/<plugin>/schema.usda): the folders
// its sublayers (@usd/schema.usda@, @usdGeom/schema.usda@) are found in, as
// usdGenSchema finds them: its own folder, then every plugin's resources. Empty
// for any other file.
std::vector<std::string> schemaSourceSearchPaths(const std::string& path);

// A plugin's friendly name ("usdLux" is "Lighting"); one this doesn't know
// keeps its own. The categories of the Applied Schemas menu too.
std::string schemaCategoryName(const std::string& plugin);

}  // namespace usdcc::usd
