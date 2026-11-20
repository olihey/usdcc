#include "usdcc/usd/schema_info.h"

#include "usdcc/assets/paths.h"

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/primDefinition.h>
#include <pxr/usd/usd/schemaBase.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/typed.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {

SchemaInfo::Kind kindOf(UsdSchemaKind kind) {
    switch (kind) {
        case UsdSchemaKind::ConcreteTyped: return SchemaInfo::Kind::ConcreteTyped;
        case UsdSchemaKind::AbstractTyped: return SchemaInfo::Kind::AbstractTyped;
        case UsdSchemaKind::SingleApplyAPI: return SchemaInfo::Kind::SingleApplyAPI;
        case UsdSchemaKind::MultipleApplyAPI: return SchemaInfo::Kind::MultipleApplyAPI;
        case UsdSchemaKind::NonAppliedAPI: return SchemaInfo::Kind::NonAppliedAPI;
        default: return SchemaInfo::Kind::AbstractBase;
    }
}

std::vector<std::string> strings(const TfTokenVector& tokens) {
    std::vector<std::string> result;
    result.reserve(tokens.size());
    for (const TfToken& token : tokens) result.push_back(token.GetString());
    return result;
}

const UsdPrimDefinition* definitionOf(const TfToken& name, SchemaInfo::Kind kind) {
    const UsdSchemaRegistry& registry = UsdSchemaRegistry::GetInstance();
    switch (kind) {
        case SchemaInfo::Kind::ConcreteTyped: return registry.FindConcretePrimDefinition(name);
        case SchemaInfo::Kind::AbstractTyped: return registry.FindAbstractPrimDefinition(name);
        case SchemaInfo::Kind::SingleApplyAPI:
        case SchemaInfo::Kind::MultipleApplyAPI: return registry.FindAppliedAPIPrimDefinition(name);
        default: return nullptr;
    }
}

// A typed schema's definition, concrete or abstract.
const UsdPrimDefinition* typedDefinitionOf(const TfToken& name) {
    const UsdSchemaRegistry& registry = UsdSchemaRegistry::GetInstance();
    const UsdPrimDefinition* definition = registry.FindConcretePrimDefinition(name);
    return definition ? definition : registry.FindAbstractPrimDefinition(name);
}

// As USDA writes a value: strings and tokens quoted, assets in @s.
std::string formatValue(const VtValue& value) {
    if (value.IsHolding<std::string>()) return "\"" + value.UncheckedGet<std::string>() + "\"";
    if (value.IsHolding<TfToken>()) return "\"" + value.UncheckedGet<TfToken>().GetString() + "\"";
    if (value.IsHolding<SdfAssetPath>()) return "@" + value.UncheckedGet<SdfAssetPath>().GetAssetPath() + "@";
    if (value.IsHolding<bool>()) return value.UncheckedGet<bool>() ? "true" : "false";
    return TfStringify(value);
}

// An API's definition lists itself first: "LightAPI", or a multiple-apply one
// as "CollectionAPI:__INSTANCE_NAME__".
bool isSelf(const TfToken& api, const TfToken& self) {
    return UsdSchemaRegistry::GetTypeNameAndInstance(api).first == self;
}

// Who declares each of `definition`'s properties (docs/ATTRIBUTE_EDITOR.md §5,
// rules 3 and 4): a built-in API first, multiple-apply instances before the
// rest as they're the most specific; then the most basic typed schema.
std::map<TfToken, std::string> declaringSchemas(const TfToken& self, SchemaInfo::Kind kind,
                                                const UsdPrimDefinition& definition) {
    const UsdSchemaRegistry& registry = UsdSchemaRegistry::GetInstance();
    std::map<TfToken, std::string> owners;
    TfTokenVector apis = definition.GetAppliedAPISchemas();
    apis.erase(std::remove_if(apis.begin(), apis.end(), [&](const TfToken& api) { return isSelf(api, self); }), apis.end());
    std::stable_partition(apis.begin(), apis.end(), [](const TfToken& api) {
        return !UsdSchemaRegistry::GetTypeNameAndInstance(api).second.IsEmpty();
    });
    for (const TfToken& api : apis) {
        const auto [apiName, instance] = UsdSchemaRegistry::GetTypeNameAndInstance(api);
        const UsdPrimDefinition* apiDefinition = registry.FindAppliedAPIPrimDefinition(apiName);
        if (!apiDefinition) continue;
        for (const TfToken& property : apiDefinition->GetPropertyNames()) {
            owners.emplace(instance.IsEmpty() ? property : UsdSchemaRegistry::MakeMultipleApplyNameInstance(property, instance),
                           api.GetString());
        }
    }
    if (kind == SchemaInfo::Kind::ConcreteTyped || kind == SchemaInfo::Kind::AbstractTyped) {
        std::vector<TfType> chain;
        for (TfType type = UsdSchemaRegistry::GetTypeFromSchemaTypeName(self); type && type != TfType::Find<UsdTyped>();) {
            chain.push_back(type);
            const std::vector<TfType> bases = type.GetBaseTypes();
            type = bases.empty() ? TfType() : bases.front();
        }
        std::reverse(chain.begin(), chain.end());  // most basic first
        for (const TfType& type : chain) {
            const TfToken name = UsdSchemaRegistry::GetSchemaTypeName(type);
            if (const UsdPrimDefinition* typed = typedDefinitionOf(name)) {
                for (const TfToken& property : typed->GetPropertyNames()) owners.emplace(property, name.GetString());
            }
        }
    }
    return owners;
}

std::vector<SchemaProperty> propertiesOf(const TfToken& self, SchemaInfo::Kind kind, const UsdPrimDefinition& definition) {
    const std::map<TfToken, std::string> owners = declaringSchemas(self, kind, definition);
    std::vector<SchemaProperty> properties;
    for (const TfToken& name : definition.GetPropertyNames()) {
        const UsdPrimDefinition::Property property = definition.GetPropertyDefinition(name);
        if (!property) continue;
        SchemaProperty p;
        p.name = name.GetString();
        p.relationship = property.IsRelationship();
        p.uniform = property.GetVariability() == SdfVariabilityUniform;
        p.documentation = property.GetDocumentation();
        property.GetMetadata(SdfFieldKeys->DisplayName, &p.displayName);
        property.GetMetadata(SdfFieldKeys->DisplayGroup, &p.displayGroup);
        if (const UsdPrimDefinition::Attribute attribute = definition.GetAttributeDefinition(name)) {
            p.typeName = attribute.GetTypeName().GetAsToken().GetString();
            VtValue fallback;
            if (attribute.GetFallbackValue(&fallback) && !fallback.IsEmpty()) {
                p.hasFallback = true;
                p.fallback = formatValue(fallback);
            }
            VtTokenArray allowed;
            if (property.GetMetadata(SdfFieldKeys->AllowedTokens, &allowed)) {
                for (const TfToken& token : allowed) p.allowedTokens.push_back(token.GetString());
            }
        }
        const auto owner = owners.find(name);
        p.declaredBy = owner == owners.end() ? self.GetString() : owner->second;
        properties.push_back(std::move(p));
    }
    std::sort(properties.begin(), properties.end(),
              [](const SchemaProperty& a, const SchemaProperty& b) { return a.name < b.name; });
    return properties;
}

// The documentation of a schema with no definition (a non-applied API, a
// root), from its plugin's generatedSchema.usda.
std::string documentationFromFile(const std::string& generatedPath, const TfToken& name) {
    if (generatedPath.empty()) return {};
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(generatedPath);
    if (!layer) return {};
    const SdfPrimSpecHandle prim = layer->GetPrimAtPath(SdfPath::AbsoluteRootPath().AppendChild(name));
    if (!prim) return {};
    if (std::string doc = prim->GetDocumentation(); !doc.empty()) return doc;
    // usdGenSchema moves a class's doc here, where prim definitions read it from.
    const VtDictionary customData = prim->GetCustomData();
    const auto brief = customData.find("userDocBrief");
    return brief != customData.end() && brief->second.IsHolding<std::string>() ? brief->second.UncheckedGet<std::string>()
                                                                                : std::string();
}

SchemaInfo infoFor(const TfType& type) {
    SchemaInfo info;
    const UsdSchemaRegistry::SchemaInfo* registered = UsdSchemaRegistry::FindSchemaInfo(type);
    if (!registered || registered->identifier.IsEmpty()) return info;
    const TfToken name = registered->identifier;
    info.name = name.GetString();
    info.typeName = type.GetTypeName();
    info.kind = kindOf(registered->kind);
    info.family = registered->family.GetString();
    info.version = registered->version;

    if (const PlugPluginPtr plugin = PlugRegistry::GetInstance().GetPluginForType(type)) {
        info.plugin = plugin->GetName();
        info.codeless = plugin->IsResource();
        const std::filesystem::path resources = usdcc::assets::fromUtf8(plugin->GetResourcePath());
        std::error_code error;
        const std::filesystem::path source = resources / usdcc::assets::fromUtf8(info.plugin) / "schema.usda";
        if (std::filesystem::is_regular_file(source, error)) info.sourcePath = usdcc::assets::toUtf8(source.lexically_normal());
        const std::filesystem::path generated = resources / "generatedSchema.usda";
        if (std::filesystem::is_regular_file(generated, error)) info.generatedPath = usdcc::assets::toUtf8(generated.lexically_normal());
    }
    // The usd plugin also has the roots and the clip and model APIs, which aren't
    // the collections and color spaces it is in the Applied Schemas menu.
    info.category = info.plugin == "usd" ? std::string("Core")
                                         : schemaCategoryName(info.plugin.empty() ? std::string("Other") : info.plugin);

    const std::vector<TfType> bases = type.GetBaseTypes();
    if (!bases.empty() && bases.front() != TfType::Find<UsdSchemaBase>()) {
        info.base = UsdSchemaRegistry::GetSchemaTypeName(bases.front()).GetString();
    }

    if (const UsdPrimDefinition* definition = definitionOf(name, info.kind)) {
        info.documentation = definition->GetDocumentation();
        info.properties = propertiesOf(name, info.kind, *definition);
        if (isTyped(info.kind)) {
            // What applies itself to this type is listed apart.
            for (const std::string& api : strings(definition->GetAppliedAPISchemas())) {
                const auto& autoApply = UsdSchemaRegistry::GetAutoApplyAPISchemas();
                const auto it = autoApply.find(TfToken(api));
                bool automatic = false;
                if (it != autoApply.end()) {
                    for (const TfToken& target : it->second) {
                        const TfType targetType = UsdSchemaRegistry::GetTypeFromSchemaTypeName(target);
                        if (targetType && type.IsA(targetType)) automatic = true;
                    }
                }
                (automatic ? info.autoAppliedAPIs : info.builtInAPIs).push_back(api);
            }
        } else {
            for (const TfToken& api : definition->GetAppliedAPISchemas()) {
                if (!isSelf(api, name)) info.builtInAPIs.push_back(api.GetString());
            }
        }
    } else {
        info.documentation = documentationFromFile(info.generatedPath, name);
    }

    if (isAPI(info.kind)) {
        const auto& autoApply = UsdSchemaRegistry::GetAutoApplyAPISchemas();
        if (const auto it = autoApply.find(name); it != autoApply.end()) info.autoApplyTo = strings(it->second);
        info.canOnlyApplyTo = strings(UsdSchemaRegistry::GetAPISchemaCanOnlyApplyToTypeNames(name));
    }
    if (info.kind == SchemaInfo::Kind::MultipleApplyAPI) {
        // "collection:__INSTANCE_NAME__:includes" says it: what's before the instance.
        static const std::string kInstance = ":__INSTANCE_NAME__";
        for (const SchemaProperty& property : info.properties) {
            const std::size_t at = property.name.find(kInstance);
            if (at != std::string::npos) {
                info.propertyNamespacePrefix = property.name.substr(0, at);
                break;
            }
        }
    }
    return info;
}

}  // namespace

std::vector<SchemaInfo> allSchemas() {
    // The plugin registry's list, not TfType's: TfType only knows a plugin's
    // types once something has declared them, which a library not loaded yet
    // hasn't (in a test: 124 schemas instead of 140).
    std::set<TfType> types;
    PlugRegistry::GetAllDerivedTypes(TfType::Find<UsdSchemaBase>(), &types);
    std::vector<SchemaInfo> schemas;
    schemas.reserve(types.size());
    for (const TfType& type : types) {
        SchemaInfo info = infoFor(type);
        if (!info.name.empty()) schemas.push_back(std::move(info));
    }
    std::sort(schemas.begin(), schemas.end(), [](const SchemaInfo& a, const SchemaInfo& b) { return a.name < b.name; });
    return schemas;
}

SchemaInfo schemaInfo(const std::string& name) {
    const TfToken schema = UsdSchemaRegistry::GetTypeNameAndInstance(TfToken(name)).first;
    const UsdSchemaRegistry::SchemaInfo* registered = UsdSchemaRegistry::FindSchemaInfo(schema);
    return registered ? infoFor(registered->type) : SchemaInfo();
}

std::vector<std::string> schemaSourceSearchPaths(const std::string& path) {
    std::vector<std::string> paths;
    if (path.find("://") != std::string::npos) return paths;
    const std::filesystem::path file = usdcc::assets::fromUtf8(path);
    if (file.filename() != "schema.usda") return paths;
    const std::filesystem::path folder = file.parent_path();
    const PlugPluginPtr plugin = PlugRegistry::GetInstance().GetPluginWithName(usdcc::assets::toUtf8(folder.filename()));
    std::error_code error;
    if (!plugin || !std::filesystem::equivalent(usdcc::assets::fromUtf8(plugin->GetResourcePath()), folder.parent_path(), error)) {
        return paths;
    }
    paths.push_back(usdcc::assets::toUtf8(folder));
    for (const PlugPluginPtr& other : PlugRegistry::GetInstance().GetAllPlugins()) {
        if (!other->GetResourcePath().empty()) paths.push_back(other->GetResourcePath());
    }
    return paths;
}

std::string schemaKindName(SchemaInfo::Kind kind) {
    switch (kind) {
        case SchemaInfo::Kind::ConcreteTyped: return "Prim type";
        case SchemaInfo::Kind::AbstractTyped: return "Abstract type";
        case SchemaInfo::Kind::SingleApplyAPI: return "API";
        case SchemaInfo::Kind::MultipleApplyAPI: return "Multiple-apply API";
        case SchemaInfo::Kind::NonAppliedAPI: return "Non-applied API";
        case SchemaInfo::Kind::AbstractBase: return "Base";
    }
    return {};
}

bool isTyped(SchemaInfo::Kind kind) {
    return kind == SchemaInfo::Kind::ConcreteTyped || kind == SchemaInfo::Kind::AbstractTyped;
}

bool isAPI(SchemaInfo::Kind kind) {
    return kind == SchemaInfo::Kind::SingleApplyAPI || kind == SchemaInfo::Kind::MultipleApplyAPI ||
           kind == SchemaInfo::Kind::NonAppliedAPI;
}

std::string schemaCategoryName(const std::string& plugin) {
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

}  // namespace usdcc::usd
