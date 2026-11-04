#pragma once

#include <pxr/base/tf/token.h>
#include <pxr/usd/usd/prim.h>

#include <string>
#include <vector>

namespace usdcc::usd {

// A prim's applied API schemas, what could be applied to it, and what removing
// one leaves behind (docs/APPLIED_SCHEMAS.md).
struct AppliedSchema {
    enum class State {
        Authored,      // on this prim: can be removed
        BuiltIn,       // from its type or another schema: removing it does nothing
        Unregistered,  // authored, but USD doesn't know it (a plugin not loaded, a typo)
    };
    std::string name;  // "PhysicsMassAPI", or "CollectionAPI:lightLink"
    State state = State::Authored;
    // For a built-in one: the type or schema it comes with; empty if USD
    // applies it automatically.
    std::string cameFrom;
};

// Every schema on the prim: UsdPrim::GetAppliedSchemas() in its order, with the
// unregistered ones it leaves out at the end.
std::vector<AppliedSchema> appliedSchemas(const PXR_NS::UsdPrim& prim);

// One applicable schema, as the + menu lists it. A multiple-apply one is always
// available: a prim can have many, and canApplyInstance() judges each name.
struct SchemaCatalogEntry {
    std::string name;
    bool multipleApply = false;
    bool available = true;
    std::string whyNot;  // already applied, or USD's reason it can't apply here
    std::string documentation;
};
struct SchemaCategory {
    std::string name;
    std::vector<SchemaCatalogEntry> schemas;  // by name
};
// Every registered applicable schema, by the plugin it comes from under a
// friendly name (an unknown plugin under its own name), categories by name.
std::vector<SchemaCategory> schemaCatalog(const PXR_NS::UsdPrim& prim);

// Whether `schema` ("CollectionAPI") can be applied as `instance` ("ferns"); if
// not, `why` says why.
bool canApplyInstance(const PXR_NS::UsdPrim& prim, const std::string& schema, const std::string& instance,
                      std::string* why);

// What removing `schema` (from appliedSchemas()) leaves authored on the prim:
// properties it defines that no other schema or the type still does, by whether
// the edit target can remove them. A property elsewhere too is in the first.
struct OrphanedProperties {
    std::vector<PXR_NS::TfToken> inEditTarget;
    std::vector<PXR_NS::TfToken> inOtherLayers;
    bool empty() const { return inEditTarget.empty() && inOtherLayers.empty(); }
};
OrphanedProperties orphanedProperties(const PXR_NS::UsdPrim& prim, const std::string& schema);

// The attributes applying `schema` could author on the prim: those it defines,
// its nested schemas' too, that the prim hasn't authored yet. No relationships:
// an empty one says nothing.
std::vector<PXR_NS::TfToken> schemaAttributesToAdd(const PXR_NS::UsdPrim& prim, const std::string& schema);

// For a tooltip: what USD says about a schema (a type name or "Type:instance"),
// and the properties it defines. Empty if it isn't registered.
std::string schemaDocumentation(const std::string& schema);
std::vector<std::string> schemaPropertyNames(const std::string& schema);

}  // namespace usdcc::usd
