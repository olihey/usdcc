#include "usdcc/usd/applied_schemas.h"

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/usd/apiSchemaBase.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/primDefinition.h>
#include <pxr/usd/usd/property.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <map>
#include <set>

namespace usdcc::usd {

namespace {

const PXR_NS::UsdPrimDefinition* definitionOf(const std::string& schema) {
    // For "CollectionAPI:ferns", the definition of "CollectionAPI".
    const auto [typeName, instance] = PXR_NS::UsdSchemaRegistry::GetTypeNameAndInstance(PXR_NS::TfToken(schema));
    return PXR_NS::UsdSchemaRegistry::GetInstance().FindAppliedAPIPrimDefinition(typeName);
}

bool contains(const PXR_NS::TfTokenVector& tokens, const std::string& name) {
    return std::find(tokens.begin(), tokens.end(), PXR_NS::TfToken(name)) != tokens.end();
}

// A plugin's friendly name; one this doesn't know keeps its own.
std::string categoryOf(const std::string& plugin) {
    static const std::map<std::string, std::string> names{
        {"usd", "Collections & Color"},
        {"usdGeom", "Geometry"},
        {"usdHydra", "Hydra"},
        {"usdLod", "Level of Detail"},
        {"usdLux", "Lighting"},
        {"usdMedia", "Media"},
        {"usdMtlx", "Shading"},
        {"usdPhysics", "Physics"},
        {"UsdProfiles", "Profiles"},
        {"usdRi", "Shading"},
        {"usdSemantics", "Semantics"},
        {"usdShade", "Shading"},
        {"usdSkel", "Skeleton"},
        {"usdUI", "UI"},
        {"usdVol", "Volumes & Particles"},
    };
    const auto it = names.find(plugin);
    return it == names.end() ? plugin : it->second;
}

}  // namespace

std::vector<AppliedSchema> appliedSchemas(const PXR_NS::UsdPrim& prim) {
    std::vector<AppliedSchema> schemas;
    if (!prim) return schemas;
    const PXR_NS::UsdSchemaRegistry& registry = PXR_NS::UsdSchemaRegistry::GetInstance();
    const PXR_NS::TfTokenVector authored = prim.GetPrimTypeInfo().GetAppliedAPISchemas();
    const PXR_NS::TfToken typeName = prim.GetTypeName();
    PXR_NS::TfTokenVector fromType;
    if (const PXR_NS::UsdPrimDefinition* type = typeName.IsEmpty() ? nullptr : registry.FindConcretePrimDefinition(typeName)) {
        fromType = type->GetAppliedAPISchemas();
    }
    const PXR_NS::TfTokenVector applied = prim.GetAppliedSchemas();
    for (const PXR_NS::TfToken& token : applied) {
        AppliedSchema schema;
        schema.name = token.GetString();
        if (contains(fromType, schema.name)) {
            // Even if it's authored too: removing it wouldn't change anything.
            schema.state = AppliedSchema::State::BuiltIn;
            schema.cameFrom = typeName.GetString();
        } else if (!contains(authored, schema.name)) {
            schema.state = AppliedSchema::State::BuiltIn;
            for (const PXR_NS::TfToken& other : authored) {
                const PXR_NS::UsdPrimDefinition* definition = definitionOf(other.GetString());
                if (definition && contains(definition->GetAppliedAPISchemas(), schema.name)) {
                    schema.cameFrom = other.GetString();
                    break;
                }
            }
        }
        schemas.push_back(std::move(schema));
    }
    for (const PXR_NS::TfToken& token : authored) {
        if (std::find(applied.begin(), applied.end(), token) != applied.end()) continue;
        AppliedSchema schema;
        schema.name = token.GetString();
        schema.state = AppliedSchema::State::Unregistered;
        schemas.push_back(std::move(schema));
    }
    return schemas;
}

std::vector<SchemaCategory> schemaCatalog(const PXR_NS::UsdPrim& prim) {
    std::set<PXR_NS::TfType> types;
    PXR_NS::TfType::Find<PXR_NS::UsdAPISchemaBase>().GetAllDerivedTypes(&types);
    const PXR_NS::TfTokenVector applied = prim ? prim.GetAppliedSchemas() : PXR_NS::TfTokenVector();
    std::map<std::string, std::vector<SchemaCatalogEntry>> categories;
    for (const PXR_NS::TfType& type : types) {
        if (!PXR_NS::UsdSchemaRegistry::IsAppliedAPISchema(type)) continue;  // not applicable
        const PXR_NS::TfToken name = PXR_NS::UsdSchemaRegistry::GetSchemaTypeName(type);
        if (name.IsEmpty()) continue;
        SchemaCatalogEntry entry;
        entry.name = name.GetString();
        entry.multipleApply = PXR_NS::UsdSchemaRegistry::IsMultipleApplyAPISchema(type);
        entry.documentation = schemaDocumentation(entry.name);
        if (!entry.multipleApply) {
            if (contains(applied, entry.name)) {
                entry.available = false;
                entry.whyNot = "Already on this prim";
            } else if (prim) {
                std::string why;
                if (!prim.CanApplyAPI(type, &why)) {
                    entry.available = false;
                    entry.whyNot = why;
                }
            }
        }
        const PXR_NS::PlugPluginPtr plugin = PXR_NS::PlugRegistry::GetInstance().GetPluginForType(type);
        categories[categoryOf(plugin ? plugin->GetName() : "Other")].push_back(std::move(entry));
    }
    std::vector<SchemaCategory> result;
    for (auto& [name, entries] : categories) {
        std::sort(entries.begin(), entries.end(),
                  [](const SchemaCatalogEntry& a, const SchemaCatalogEntry& b) { return a.name < b.name; });
        result.push_back({name, std::move(entries)});
    }
    return result;
}

bool canApplyInstance(const PXR_NS::UsdPrim& prim, const std::string& schema, const std::string& instance,
                      std::string* why) {
    auto refuse = [why](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (!prim) return refuse("No prim");
    if (!PXR_NS::SdfPath::IsValidNamespacedIdentifier(instance)) return refuse("'" + instance + "' isn't a valid name");
    const PXR_NS::TfToken schemaToken(schema);
    const PXR_NS::TfToken instanceToken(instance);
    if (!PXR_NS::UsdSchemaRegistry::IsAllowedAPISchemaInstanceName(schemaToken, instanceToken)) {
        return refuse(schema + " doesn't allow the name '" + instance + "'");
    }
    if (contains(prim.GetAppliedSchemas(), schema + ":" + instance)) {
        return refuse("This prim has " + schema + ":" + instance + " already");
    }
    std::string reason;
    const PXR_NS::TfType type = PXR_NS::UsdSchemaRegistry::GetAPITypeFromSchemaTypeName(schemaToken);
    if (!prim.CanApplyAPI(type, instanceToken, &reason)) return refuse(reason);
    return true;
}

OrphanedProperties orphanedProperties(const PXR_NS::UsdPrim& prim, const std::string& schema) {
    OrphanedProperties orphans;
    if (!prim) return orphans;
    // The definition without it, built without changing anything. This handles
    // nested schemas and instances, and keeps what another schema still defines.
    PXR_NS::TfTokenVector remaining = prim.GetPrimTypeInfo().GetAppliedAPISchemas();
    remaining.erase(std::remove(remaining.begin(), remaining.end(), PXR_NS::TfToken(schema)), remaining.end());
    PXR_NS::TfTokenVector stillDefined;
    const PXR_NS::UsdSchemaRegistry& registry = PXR_NS::UsdSchemaRegistry::GetInstance();
    if (remaining.empty()) {
        // Its last one: USD won't compose a definition from no schemas, and what
        // stays is the type's (a typeless prim has none).
        if (const PXR_NS::UsdPrimDefinition* type = registry.FindConcretePrimDefinition(prim.GetTypeName())) {
            stillDefined = type->GetPropertyNames();
        }
    } else {
        const std::unique_ptr<PXR_NS::UsdPrimDefinition> without =
            registry.BuildComposedPrimDefinition(prim.GetTypeName(), remaining);
        // Not knowing what stays must never offer to delete anything.
        if (!without) return orphans;
        stillDefined = without->GetPropertyNames();
    }

    std::set<PXR_NS::TfToken> candidates;
    for (const PXR_NS::TfToken& name : prim.GetPrimDefinition().GetPropertyNames()) {
        if (std::find(stillDefined.begin(), stillDefined.end(), name) == stillDefined.end()) candidates.insert(name);
    }
    // MaterialBindingAPI defines no properties: its bindings are per-purpose.
    if (schema == "MaterialBindingAPI") {
        for (const PXR_NS::UsdProperty& property : prim.GetAuthoredProperties()) {
            const std::string name = property.GetName().GetString();
            if (name == "material:binding" || PXR_NS::TfStringStartsWith(name, "material:binding:")) {
                candidates.insert(property.GetName());
            }
        }
    }

    const PXR_NS::UsdEditTarget target = prim.GetStage()->GetEditTarget();
    for (const PXR_NS::TfToken& name : candidates) {
        const PXR_NS::UsdProperty property = prim.GetProperty(name);
        if (!property || property.GetPropertyStack().empty()) continue;  // only a fallback
        (target.GetPropertySpecForScenePath(property.GetPath()) ? orphans.inEditTarget : orphans.inOtherLayers)
            .push_back(name);
    }
    return orphans;
}

std::vector<PXR_NS::TfToken> schemaAttributesToAdd(const PXR_NS::UsdPrim& prim, const std::string& schema) {
    std::vector<PXR_NS::TfToken> names;
    const PXR_NS::UsdPrimDefinition* definition = definitionOf(schema);
    if (!prim || !definition) return names;
    const auto [typeName, instance] = PXR_NS::UsdSchemaRegistry::GetTypeNameAndInstance(PXR_NS::TfToken(schema));
    for (const PXR_NS::TfToken& name : definition->GetPropertyNames()) {
        if (!definition->GetAttributeDefinition(name)) continue;
        // A template ("collection:__INSTANCE_NAME__:mode") as this instance's.
        const PXR_NS::TfToken applied =
            instance.IsEmpty() ? name : PXR_NS::UsdSchemaRegistry::MakeMultipleApplyNameInstance(name.GetString(), instance.GetString());
        const PXR_NS::UsdProperty existing = prim.GetProperty(applied);
        if (!existing || existing.GetPropertyStack().empty()) names.push_back(applied);
    }
    return names;
}

std::string schemaDocumentation(const std::string& schema) {
    const PXR_NS::UsdPrimDefinition* definition = definitionOf(schema);
    return definition ? definition->GetDocumentation() : std::string();
}

std::vector<std::string> schemaPropertyNames(const std::string& schema) {
    std::vector<std::string> names;
    const PXR_NS::UsdPrimDefinition* definition = definitionOf(schema);
    if (!definition) return names;
    const auto [typeName, instance] = PXR_NS::UsdSchemaRegistry::GetTypeNameAndInstance(PXR_NS::TfToken(schema));
    for (const PXR_NS::TfToken& name : definition->GetPropertyNames()) {
        // A template ("collection:__INSTANCE_NAME__:includes") as this instance's.
        names.push_back(instance.IsEmpty() ? name.GetString()
                                           : PXR_NS::UsdSchemaRegistry::MakeMultipleApplyNameInstance(name.GetString(),
                                                                                                     instance.GetString())
                                                 .GetString());
    }
    return names;
}

}  // namespace usdcc::usd
