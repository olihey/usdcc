#include "usdcc/usd/property_categorizer.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/usd/primDefinition.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/typed.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {

enum Rank {
    kTransform = 0,
    kTypedSchema = 1,
    kApiSchema = 2,
    kDisplayGroup = 3,
    kPrimvars = 4,
    kInputs = 5,
    kOutputs = 6,
    kNamespace = 7,
    kCustom = 8,
};

struct Bucket {
    PropertyCategory category;
    int rank = kCustom;
    int order = 0;  // within a rank
};

bool startsWith(const std::string& s, const char* prefix) { return TfStringStartsWith(s, prefix); }

const char* friendlyTypedName(const std::string& schema) {
    if (schema == "Imageable") return "Visibility";
    if (schema == "Boundable") return "Bounds";
    if (schema == "Gprim") return "Display";
    if (schema == "PointBased") return "Geometry";
    if (schema == "Xformable") return "Transform";
    return nullptr;
}

const char* friendlyApiName(const std::string& schema) {
    if (schema == "NodeDefAPI") return "Node Definition";
    if (schema == "MaterialBindingAPI") return "Material Binding";
    return nullptr;
}

}  // namespace

std::string prettifyIdentifier(const std::string& identifier) {
    std::string out;
    const size_t n = identifier.size();
    for (size_t i = 0; i < n; ++i) {
        const char c = identifier[i];
        if (c == '_' || c == ':') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        const bool upper = std::isupper(static_cast<unsigned char>(c));
        if (i > 0 && upper && !out.empty() && out.back() != ' ') {
            const char prev = identifier[i - 1];
            const bool prevLowerOrDigit =
                std::islower(static_cast<unsigned char>(prev)) || std::isdigit(static_cast<unsigned char>(prev));
            // An acronym ends where an upper-case letter is followed by a lower-case one: "UVSet" -> "UV Set".
            const bool acronymEnd = std::isupper(static_cast<unsigned char>(prev)) && i + 1 < n &&
                                    std::islower(static_cast<unsigned char>(identifier[i + 1]));
            if (prevLowerOrDigit || acronymEnd) out += ' ';
        }
        const bool wordStart = out.empty() || out.back() == ' ';
        out += wordStart ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string schemaLabel(const TfToken& schemaName) {
    const auto [typeName, instance] = UsdSchemaRegistry::GetTypeNameAndInstance(schemaName);
    const std::string name = typeName.GetString();
    if (const char* friendly = friendlyTypedName(name)) return friendly;
    if (const char* friendly = friendlyApiName(name)) return friendly;
    std::string base = name;
    if (TfStringEndsWith(base, "API")) base.resize(base.size() - 3);
    std::string label = prettifyIdentifier(base);
    if (!instance.IsEmpty()) label += " (" + instance.GetString() + ")";
    return label;
}

std::string propertyLabel(const UsdProperty& property) {
    const std::string displayName = property.GetDisplayName();
    if (!displayName.empty()) return displayName;

    std::string name = property.GetName().GetString();
    if (startsWith(name, "xformOp:")) {
        // xformOp:<type>[:<suffix>]
        const std::vector<std::string> parts = TfStringSplit(name.substr(8), ":");
        std::string label = parts.empty() ? name : prettifyIdentifier(parts[0]);
        if (parts.size() > 1) {
            std::vector<std::string> suffix(parts.begin() + 1, parts.end());
            label += " (" + TfStringJoin(suffix, ":") + ")";
        }
        return label;
    }
    for (const char* prefix : {"primvars:", "inputs:", "outputs:"}) {
        if (startsWith(name, prefix)) {
            name = name.substr(std::char_traits<char>::length(prefix));
            break;
        }
    }
    return prettifyIdentifier(name);
}

std::string propertyLabelInCategory(const UsdProperty& property, const PropertyCategory& category) {
    if (!property.GetDisplayName().empty()) return property.GetDisplayName();
    const std::string& name = property.GetName().GetString();
    // The leading name components the section's own title already says.
    std::string covered;
    if (TfStringStartsWith(category.key, "ns:")) {
        covered = category.key.substr(3) + ":";
    } else if (TfStringStartsWith(category.key, "api:")) {
        const auto [schema, instance] = UsdSchemaRegistry::GetTypeNameAndInstance(TfToken(category.key.substr(4)));
        const std::vector<std::string> parts = property.SplitName();
        if (!instance.IsEmpty()) {
            // e.g. collection:lightLink:includeRoot in "Collection (lightLink)".
            const std::string marker = ":" + instance.GetString() + ":";
            const size_t at = name.find(marker);
            if (at != std::string::npos) covered = name.substr(0, at + marker.size());
        } else if (parts.size() > 1) {
            // e.g. physics:velocity in "Physics Rigid Body", light:filters in "Light".
            const std::string first = prettifyIdentifier(parts.front());
            const std::string& label = category.label;
            if (TfStringStartsWith(label, first) && (label.size() == first.size() || label[first.size()] == ' ')) {
                covered = parts.front() + ":";
            }
        }
    }
    if (!covered.empty() && TfStringStartsWith(name, covered) && name.size() > covered.size()) {
        return prettifyIdentifier(name.substr(covered.size()));
    }
    return propertyLabel(property);
}

std::vector<PropertyCategory> categorizeProperties(const UsdPrim& prim, const CategorizeOptions& options) {
    std::vector<PropertyCategory> result;
    if (!prim) return result;

    const UsdSchemaRegistry& registry = UsdSchemaRegistry::GetInstance();

    // Rule 3: which applied API declares each property. An API's definition
    // includes the properties of the APIs it builds in (LightAPI includes its
    // light-link CollectionAPI instance), so multiple-apply instances claim
    // their properties first: they are the most specific owner.
    std::map<TfToken, TfToken> apiOwner;
    std::map<TfToken, int> apiOrder;
    int apiIndex = 0;
    std::vector<TfToken> appliedSchemas = prim.GetAppliedSchemas();
    for (const TfToken& applied : appliedSchemas) apiOrder[applied] = apiIndex++;
    std::stable_partition(appliedSchemas.begin(), appliedSchemas.end(), [](const TfToken& applied) {
        return !UsdSchemaRegistry::GetTypeNameAndInstance(applied).second.IsEmpty();
    });
    for (const TfToken& applied : appliedSchemas) {
        const auto [schemaName, instance] = UsdSchemaRegistry::GetTypeNameAndInstance(applied);
        const UsdPrimDefinition* def = registry.FindAppliedAPIPrimDefinition(schemaName);
        if (!def) continue;
        for (const TfToken& property : def->GetPropertyNames()) {
            const TfToken name =
                instance.IsEmpty() ? property : UsdSchemaRegistry::MakeMultipleApplyNameInstance(property, instance);
            apiOwner.emplace(name, applied);
        }
    }

    // Rule 4: the most basic typed schema declaring each property; depth
    // orders the sections most-derived first.
    std::map<TfToken, TfToken> typedOwner;
    std::map<TfToken, int> typedDepth;
    {
        std::vector<TfType> chain;
        for (TfType t = UsdSchemaRegistry::GetTypeFromSchemaTypeName(prim.GetTypeName());
             t && t != TfType::Find<UsdTyped>();) {
            chain.push_back(t);
            const std::vector<TfType> bases = t.GetBaseTypes();
            t = bases.empty() ? TfType() : bases.front();
        }
        std::reverse(chain.begin(), chain.end());  // most basic first
        int depth = 0;
        for (const TfType& t : chain) {
            const TfToken schemaName = UsdSchemaRegistry::GetSchemaTypeName(t);
            typedDepth[schemaName] = depth++;
            const UsdPrimDefinition* def = registry.FindConcretePrimDefinition(schemaName);
            if (!def) def = registry.FindAbstractPrimDefinition(schemaName);
            if (!def) continue;
            for (const TfToken& property : def->GetPropertyNames()) {
                typedOwner.emplace(property, schemaName);
            }
        }
    }

    std::map<std::string, Bucket> buckets;
    int displayGroupIndex = 0;
    auto bucketFor = [&](const std::string& key, const std::string& label, int rank, int order) -> Bucket& {
        auto [it, inserted] = buckets.try_emplace(key);
        if (inserted) {
            it->second.category.key = key;
            it->second.category.label = label;
            it->second.rank = rank;
            it->second.order = order;
        }
        return it->second;
    };

    for (const UsdProperty& property : prim.GetProperties()) {
        if (!options.includeHidden && property.IsHidden()) continue;
        if (options.authoredOnly && !property.IsAuthored()) continue;

        const TfToken& name = property.GetName();
        const std::string& n = name.GetString();

        if (property.HasAuthoredDisplayGroup()) {
            const std::string group = property.GetDisplayGroup();
            if (!group.empty()) {
                const std::vector<std::string> nested = property.GetNestedDisplayGroups();
                const std::string label = TfStringJoin(nested, " / ");
                bucketFor("group:" + group, label, kDisplayGroup, displayGroupIndex++).category.properties.push_back(property);
                continue;
            }
        }
        if (startsWith(n, "xformOp:") || name == UsdGeomTokens->xformOpOrder) {
            bucketFor("transform", "Transform", kTransform, 0).category.properties.push_back(property);
            continue;
        }
        if (auto it = apiOwner.find(name); it != apiOwner.end()) {
            bucketFor("api:" + it->second.GetString(), schemaLabel(it->second), kApiSchema, apiOrder[it->second])
                .category.properties.push_back(property);
            continue;
        }
        if (auto it = typedOwner.find(name); it != typedOwner.end()) {
            // Most derived first: deeper schemas get a smaller order.
            bucketFor("schema:" + it->second.GetString(), schemaLabel(it->second), kTypedSchema,
                      -typedDepth[it->second])
                .category.properties.push_back(property);
            continue;
        }
        if (startsWith(n, "inputs:")) {
            bucketFor("inputs", "Inputs", kInputs, 0).category.properties.push_back(property);
            continue;
        }
        if (startsWith(n, "outputs:")) {
            bucketFor("outputs", "Outputs", kOutputs, 0).category.properties.push_back(property);
            continue;
        }
        if (startsWith(n, "primvars:")) {
            bucketFor("primvars", "Primvars", kPrimvars, 0).category.properties.push_back(property);
            continue;
        }
        const std::vector<std::string> namespaces = property.SplitName();
        if (namespaces.size() > 1) {
            bucketFor("ns:" + namespaces.front(), prettifyIdentifier(namespaces.front()), kNamespace, 0)
                .category.properties.push_back(property);
            continue;
        }
        bucketFor("custom", "Custom", kCustom, 0).category.properties.push_back(property);
    }

    std::vector<Bucket*> ordered;
    for (auto& [key, bucket] : buckets) ordered.push_back(&bucket);
    std::sort(ordered.begin(), ordered.end(), [](const Bucket* a, const Bucket* b) {
        if (a->rank != b->rank) return a->rank < b->rank;
        if (a->order != b->order) return a->order < b->order;
        return a->category.label < b->category.label;
    });

    // Transform follows the op order (then any op not in it, then the order
    // attribute itself); everything else is alphabetical by label.
    std::map<TfToken, int> opIndex;
    if (UsdGeomXformable xformable{prim}) {
        bool resets = false;
        int i = 0;
        for (const UsdGeomXformOp& op : xformable.GetOrderedXformOps(&resets)) {
            opIndex.emplace(op.GetAttr().GetName(), i++);
        }
    }
    for (Bucket* bucket : ordered) {
        auto& properties = bucket->category.properties;
        if (bucket->category.key == "transform") {
            auto rankOf = [&](const UsdProperty& p) {
                if (p.GetName() == UsdGeomTokens->xformOpOrder) return 1 << 20;
                auto it = opIndex.find(p.GetName());
                return it != opIndex.end() ? it->second : (1 << 19);
            };
            std::stable_sort(properties.begin(), properties.end(), [&](const UsdProperty& a, const UsdProperty& b) {
                const int ra = rankOf(a);
                const int rb = rankOf(b);
                return ra != rb ? ra < rb : a.GetName() < b.GetName();
            });
        } else {
            const PropertyCategory& category = bucket->category;
            std::stable_sort(properties.begin(), properties.end(), [&](const UsdProperty& a, const UsdProperty& b) {
                return propertyLabelInCategory(a, category) < propertyLabelInCategory(b, category);
            });
        }
        result.push_back(std::move(bucket->category));
    }
    return result;
}

}  // namespace usdcc::usd
