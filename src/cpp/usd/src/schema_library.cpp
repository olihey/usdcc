#include "usdcc/usd/schema_library.h"

#include "usdcc/assets/paths.h"

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/primDefinition.h>
#include <pxr/usd/usd/schemaRegistry.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {

const TfToken kApiSchemas("apiSchemas");
const TfToken kApiSchemaType("apiSchemaType");
const TfToken kAutoApplyTo("apiSchemaAutoApplyTo");
const TfToken kCanOnlyApplyTo("apiSchemaCanOnlyApplyTo");
const TfToken kNamespacePrefix("propertyNamespacePrefix");
constexpr auto kGlobal = "GLOBAL";
constexpr auto kApiSchemaBase = "APISchemaBase";

std::filesystem::path pathOf(const std::string& text) { return usdcc::assets::fromUtf8(text); }

VtDictionary customDataOf(const SdfPrimSpecHandle& prim) {
    const VtValue value = prim->GetInfo(SdfFieldKeys->CustomData);
    return value.IsHolding<VtDictionary>() ? value.UncheckedGet<VtDictionary>() : VtDictionary();
}

// The first error USD posted since `mark`, cleared; `fallback` if none.
std::string takeError(TfErrorMark& mark, const std::string& fallback) {
    std::string message;
    for (auto it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
        if (message.empty()) message = it->GetCommentary();
    }
    mark.Clear();
    return message.empty() ? fallback : message;
}

std::vector<std::string> tokenStrings(const VtValue& value) {
    std::vector<std::string> result;
    if (value.IsHolding<VtTokenArray>()) {
        for (const TfToken& token : value.UncheckedGet<VtTokenArray>()) result.push_back(token.GetString());
    } else if (value.IsHolding<VtStringArray>()) {
        for (const std::string& text : value.UncheckedGet<VtStringArray>()) result.push_back(text);
    } else if (value.IsHolding<std::vector<std::string>>()) {
        result = value.UncheckedGet<std::vector<std::string>>();
    }
    return result;
}

std::string textOf(const VtValue& value) {
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
    return {};
}

VtTokenArray tokens(const std::vector<std::string>& names) {
    VtTokenArray result;
    for (const std::string& name : names) result.push_back(TfToken(name));
    return result;
}

std::string quoted(const std::string& text) {
    // As USDA writes a one-line string; a schema's tokens and strings are simple.
    std::string escaped;
    for (const char c : text) {
        if (c == '"' || c == '\\') escaped += '\\';
        escaped += c;
    }
    return "\"" + escaped + "\"";
}

template <class T, class Format>
std::string arrayText(const VtArray<T>& values, Format format) {
    std::string text = "[";
    for (std::size_t i = 0; i < values.size(); ++i) text += (i ? ", " : "") + format(values[i]);
    return text + "]";
}

SchemaInfo::Kind kindOfSpec(const SdfPrimSpecHandle& prim, const std::string& base) {
    if (!prim->GetTypeName().IsEmpty()) return SchemaInfo::Kind::ConcreteTyped;
    const VtDictionary data = customDataOf(prim);
    const auto found = data.find(kApiSchemaType.GetString());
    const std::string type = found == data.end() ? std::string() : textOf(found->second);
    if (type == "multipleApply") return SchemaInfo::Kind::MultipleApplyAPI;
    if (type == "nonApplied") return SchemaInfo::Kind::NonAppliedAPI;
    if (type == "singleApply" || base == kApiSchemaBase) return SchemaInfo::Kind::SingleApplyAPI;
    return SchemaInfo::Kind::AbstractTyped;
}

bool isLibraryAPI(SchemaInfo::Kind kind) {
    return kind == SchemaInfo::Kind::SingleApplyAPI || kind == SchemaInfo::Kind::MultipleApplyAPI ||
           kind == SchemaInfo::Kind::NonAppliedAPI;
}

// Sets a customData entry, or removes it for an empty value.
void setCustomData(const SdfPrimSpecHandle& prim, const TfToken& key, const VtValue& value) {
    if (value.IsEmpty()) {
        if (customDataOf(prim).count(key.GetString())) prim->GetCustomData().erase(key.GetString());
    } else {
        prim->SetCustomData(key.GetString(), value);
    }
}

void setOrClear(const SdfSpecHandle& spec, const TfToken& field, const std::string& text) {
    if (text.empty()) {
        if (spec->HasInfo(field)) spec->ClearInfo(field);
    } else {
        spec->SetInfo(field, VtValue(text));
    }
}

bool writeProperty(const SdfPrimSpecHandle& prim, const SchemaPropertyDef& def, std::string* error) {
    const SdfPath path = prim->GetPath().AppendProperty(TfToken(def.name));
    SdfSpecHandle spec;
    if (def.relationship) {
        SdfRelationshipSpecHandle rel = prim->GetRelationshipAtPath(path);
        if (!rel) {
            if (SdfAttributeSpecHandle attr = prim->GetAttributeAtPath(path)) prim->RemoveProperty(attr);
            rel = SdfRelationshipSpec::New(prim, def.name, /*custom=*/false, SdfVariabilityUniform);
        }
        if (!rel) {
            *error = def.name + ": the relationship can't be made";
            return false;
        }
        spec = rel;
    } else {
        const SdfValueTypeName type = SdfSchema::GetInstance().FindType(def.typeName);
        if (!type) {
            *error = def.name + ": no value type '" + def.typeName + "'";
            return false;
        }
        const SdfVariability variability = def.uniform ? SdfVariabilityUniform : SdfVariabilityVarying;
        SdfAttributeSpecHandle attr = prim->GetAttributeAtPath(path);
        if (attr && (attr->GetTypeName() != type || attr->GetVariability() != variability)) {
            prim->RemoveProperty(attr);
            attr = SdfAttributeSpecHandle();
        }
        if (!attr) {
            if (SdfRelationshipSpecHandle rel = prim->GetRelationshipAtPath(path)) prim->RemoveProperty(rel);
            attr = SdfAttributeSpec::New(prim, def.name, type, variability, /*custom=*/false);
        }
        if (!attr) {
            *error = def.name + ": the attribute can't be made";
            return false;
        }
        if (def.fallback.empty()) {
            if (attr->HasDefaultValue()) attr->ClearDefaultValue();
        } else {
            VtValue value;
            std::string why;
            if (!parseFallback(def.typeName, def.fallback, &value, &why)) {
                *error = def.name + ": " + why;
                return false;
            }
            attr->SetDefaultValue(value);
        }
        if (def.allowedTokens.empty()) {
            if (attr->HasAllowedTokens()) attr->ClearAllowedTokens();
        } else {
            attr->SetAllowedTokens(tokens(def.allowedTokens));
        }
        spec = attr;
    }
    setOrClear(spec, SdfFieldKeys->Documentation, def.documentation);
    setOrClear(spec, SdfFieldKeys->DisplayName, def.displayName);
    setOrClear(spec, SdfFieldKeys->DisplayGroup, def.displayGroup);
    return true;
}

bool writeClass(const SdfLayerHandle& layer, const SchemaClassDef& def, std::string* error) {
    const SdfPath path = SdfPath::AbsoluteRootPath().AppendChild(TfToken(def.name));
    const bool api = isLibraryAPI(def.kind);
    const std::string typeName = def.kind == SchemaInfo::Kind::ConcreteTyped ? def.name : std::string();
    SdfPrimSpecHandle prim = layer->GetPrimAtPath(path);
    if (!prim) prim = SdfPrimSpec::New(layer, def.name, SdfSpecifierClass, typeName);
    if (!prim) {
        *error = def.name + ": the class can't be made";
        return false;
    }
    prim->SetSpecifier(SdfSpecifierClass);
    prim->SetTypeName(typeName);

    const std::string base = api ? std::string(kApiSchemaBase) : (def.base.empty() ? std::string("Typed") : def.base);
    prim->SetInfo(SdfFieldKeys->InheritPaths,
                  VtValue(SdfPathListOp::CreateExplicit({SdfPath::AbsoluteRootPath().AppendChild(TfToken(base))})));
    setOrClear(prim, SdfFieldKeys->Documentation, def.documentation);

    if (def.builtInAPIs.empty()) {
        if (prim->HasInfo(kApiSchemas)) prim->ClearInfo(kApiSchemas);
    } else {
        SdfTokenListOp apis;
        std::vector<TfToken> names;
        for (const std::string& name : def.builtInAPIs) names.push_back(TfToken(name));
        apis.SetPrependedItems(names);
        prim->SetInfo(kApiSchemas, VtValue(apis));
    }
    const char* apiType = def.kind == SchemaInfo::Kind::MultipleApplyAPI ? "multipleApply"
                          : def.kind == SchemaInfo::Kind::NonAppliedAPI  ? "nonApplied"
                          : api                                          ? "singleApply"
                                                                         : nullptr;
    setCustomData(prim, kApiSchemaType, apiType ? VtValue(TfToken(apiType)) : VtValue());
    setCustomData(prim, kAutoApplyTo, api && !def.autoApplyTo.empty() ? VtValue(tokens(def.autoApplyTo)) : VtValue());
    setCustomData(prim, kCanOnlyApplyTo,
                  api && !def.canOnlyApplyTo.empty() ? VtValue(tokens(def.canOnlyApplyTo)) : VtValue());
    setCustomData(prim, kNamespacePrefix,
                  def.kind == SchemaInfo::Kind::MultipleApplyAPI && !def.propertyNamespacePrefix.empty()
                      ? VtValue(TfToken(def.propertyNamespacePrefix))
                      : VtValue());

    std::set<std::string> keep;
    for (const SchemaPropertyDef& property : def.properties) keep.insert(property.name);
    for (const SdfPropertySpecHandle& property : prim->GetProperties()) {
        if (!keep.count(property->GetName())) prim->RemoveProperty(property);
    }
    for (const SchemaPropertyDef& property : def.properties) {
        if (!writeProperty(prim, property, error)) {
            *error = def.name + "." + *error;
            return false;
        }
    }
    return true;
}

// A typed schema's base chain, this library's classes first, then the
// registry's: the type a property `name` has there, and who declares it.
bool inheritedPropertyType(const std::map<std::string, const SchemaClassDef*>& known, std::string base,
                           const std::string& name, std::string* type, std::string* owner) {
    std::set<std::string> seen;
    while (!base.empty() && seen.insert(base).second) {
        if (const auto local = known.find(base); local != known.end()) {
            for (const SchemaPropertyDef& property : local->second->properties) {
                if (property.name == name) {
                    *type = property.relationship ? std::string("relationship") : property.typeName;
                    *owner = local->second->name;
                    return true;
                }
            }
            base = local->second->base;
            continue;
        }
        const UsdSchemaRegistry& registry = UsdSchemaRegistry::GetInstance();
        const UsdPrimDefinition* definition = registry.FindConcretePrimDefinition(TfToken(base));
        if (!definition) definition = registry.FindAbstractPrimDefinition(TfToken(base));
        if (!definition) return false;
        if (const UsdPrimDefinition::Property property = definition->GetPropertyDefinition(TfToken(name))) {
            *type = property.IsRelationship() ? std::string("relationship")
                                              : definition->GetAttributeDefinition(TfToken(name)).GetTypeName().GetAsToken().GetString();
            // A definition has its bases' properties too: the owner is the most basic one with it.
            *owner = base;
            for (TfType t = UsdSchemaRegistry::GetTypeFromSchemaTypeName(TfToken(base)); t;) {
                const std::vector<TfType> parents = t.GetBaseTypes();
                t = parents.empty() ? TfType() : parents.front();
                const TfToken parent = t ? UsdSchemaRegistry::GetSchemaTypeName(t) : TfToken();
                const UsdPrimDefinition* parentDefinition =
                    parent.IsEmpty() ? nullptr : registry.FindAbstractPrimDefinition(parent);
                if (!parentDefinition) parentDefinition = parent.IsEmpty() ? nullptr : registry.FindConcretePrimDefinition(parent);
                if (!parentDefinition || !parentDefinition->GetPropertyDefinition(TfToken(name))) break;
                *owner = parent.GetString();
            }
            return true;
        }
        return false;
    }
    return false;
}

// The plugin a registered schema comes from, or empty.
std::string registeredPlugin(const std::string& name) {
    const UsdSchemaRegistry::SchemaInfo* info = UsdSchemaRegistry::FindSchemaInfo(TfToken(name));
    if (!info) return {};
    const PlugPluginPtr plugin = PlugRegistry::GetInstance().GetPluginForType(info->type);
    return plugin ? plugin->GetName() : std::string("?");
}

}  // namespace

std::string schemaSourcePath(const std::string& usdFolder, const std::string& library) {
    return usdcc::assets::toUtf8(pathOf(usdFolder) / pathOf(library) / "schema.usda");
}

std::string generatedSchemaPath(const std::string& usdFolder) {
    return usdcc::assets::toUtf8(pathOf(usdFolder) / "generatedSchema.usda");
}

std::string plugInfoPath(const std::string& usdFolder) { return usdcc::assets::toUtf8(pathOf(usdFolder) / "plugInfo.json"); }

bool parseFallback(const std::string& typeName, const std::string& text, VtValue* value, std::string* error) {
    const SdfValueTypeName type = SdfSchema::GetInstance().FindType(typeName);
    if (!type) {
        if (error) *error = "no value type '" + typeName + "'";
        return false;
    }
    TfErrorMark mark;
    const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
    const std::string usda = "#usda 1.0\ndef \"P\"\n{\n    " + type.GetAsToken().GetString() + " a = " + text + "\n}\n";
    if (!layer->ImportFromString(usda)) {
        // ":4:16: Error parsing simple value: ..." is about the line made here: keep what follows.
        const std::string why = takeError(mark, "it doesn't parse");
        const std::size_t at = why.find(": ");
        if (error) *error = "'" + text + "' isn't a " + typeName + (at == std::string::npos ? "" : why.substr(at));
        return false;
    }
    mark.Clear();
    const SdfAttributeSpecHandle attr = layer->GetAttributeAtPath(SdfPath("/P.a"));
    if (!attr || !attr->HasDefaultValue()) {
        if (error) *error = "'" + text + "' isn't a " + typeName;
        return false;
    }
    if (value) *value = attr->GetDefaultValue();
    return true;
}

std::string usdaValueText(const VtValue& value) {
    if (value.IsHolding<std::string>()) return quoted(value.UncheckedGet<std::string>());
    if (value.IsHolding<TfToken>()) return quoted(value.UncheckedGet<TfToken>().GetString());
    if (value.IsHolding<SdfAssetPath>()) return "@" + value.UncheckedGet<SdfAssetPath>().GetAssetPath() + "@";
    if (value.IsHolding<bool>()) return value.UncheckedGet<bool>() ? "true" : "false";
    if (value.IsHolding<VtStringArray>()) return arrayText(value.UncheckedGet<VtStringArray>(), quoted);
    if (value.IsHolding<VtTokenArray>()) {
        return arrayText(value.UncheckedGet<VtTokenArray>(), [](const TfToken& t) { return quoted(t.GetString()); });
    }
    if (value.IsHolding<VtArray<SdfAssetPath>>()) {
        return arrayText(value.UncheckedGet<VtArray<SdfAssetPath>>(),
                         [](const SdfAssetPath& a) { return "@" + a.GetAssetPath() + "@"; });
    }
    if (value.IsHolding<VtBoolArray>()) {
        return arrayText(value.UncheckedGet<VtBoolArray>(), [](bool b) { return std::string(b ? "true" : "false"); });
    }
    return TfStringify(value);
}

bool readSchemaLibrary(const std::string& path, SchemaLibraryDef* library, std::string* error) {
    auto fail = [error](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pathOf(path), ec)) return fail("No file " + path);
    TfErrorMark mark;
    // A copy, never the layer a stage may have open.
    const SdfLayerRefPtr layer = SdfLayer::OpenAsAnonymous(path);
    if (!layer) return fail(takeError(mark, "It can't be read: " + path));
    mark.Clear();
    const SdfPrimSpecHandle global = layer->GetPrimAtPath(SdfPath::AbsoluteRootPath().AppendChild(TfToken(kGlobal)));
    const VtDictionary globalData = global ? customDataOf(global) : VtDictionary();
    const auto name = globalData.find("libraryName");
    if (name == globalData.end()) return fail("It isn't a schema library: no GLOBAL prim with a libraryName");

    SchemaLibraryDef result;
    result.name = textOf(name->second);
    if (const auto prefix = globalData.find("libraryPrefix"); prefix != globalData.end()) result.prefix = textOf(prefix->second);
    result.sublayers = layer->GetSubLayerPaths();
    for (const SdfPrimSpecHandle& prim : layer->GetRootPrims()) {
        if (prim->GetName() == kGlobal) continue;
        SchemaClassDef c;
        c.name = prim->GetName();
        const VtValue inheritValue = prim->GetInfo(SdfFieldKeys->InheritPaths);
        const SdfPathListOp inherits =
            inheritValue.IsHolding<SdfPathListOp>() ? inheritValue.UncheckedGet<SdfPathListOp>() : SdfPathListOp();
        const std::vector<SdfPath> items = inherits.IsExplicit() ? inherits.GetExplicitItems() : inherits.GetPrependedItems();
        if (!items.empty()) c.base = items.front().GetName();
        c.kind = kindOfSpec(prim, c.base);
        if (isLibraryAPI(c.kind)) c.base = kApiSchemaBase;
        c.documentation = prim->GetDocumentation();
        if (prim->HasInfo(kApiSchemas)) {
            const VtValue apis = prim->GetInfo(kApiSchemas);
            if (apis.IsHolding<SdfTokenListOp>()) {
                const SdfTokenListOp op = apis.UncheckedGet<SdfTokenListOp>();
                for (const TfToken& api : op.IsExplicit() ? op.GetExplicitItems() : op.GetPrependedItems()) {
                    c.builtInAPIs.push_back(api.GetString());
                }
            }
        }
        const VtDictionary data = customDataOf(prim);
        if (const auto it = data.find(kAutoApplyTo.GetString()); it != data.end()) c.autoApplyTo = tokenStrings(it->second);
        if (const auto it = data.find(kCanOnlyApplyTo.GetString()); it != data.end()) c.canOnlyApplyTo = tokenStrings(it->second);
        if (const auto it = data.find(kNamespacePrefix.GetString()); it != data.end()) c.propertyNamespacePrefix = textOf(it->second);
        for (const SdfPropertySpecHandle& property : prim->GetProperties()) {
            SchemaPropertyDef p;
            p.name = property->GetName();
            p.documentation = property->GetDocumentation();
            p.displayName = property->GetDisplayName();
            p.displayGroup = property->GetDisplayGroup();
            if (const SdfAttributeSpecHandle attr = TfDynamic_cast<SdfAttributeSpecHandle>(property)) {
                p.typeName = attr->GetTypeName().GetAsToken().GetString();
                p.uniform = attr->GetVariability() == SdfVariabilityUniform;
                if (attr->HasDefaultValue()) p.fallback = usdaValueText(attr->GetDefaultValue());
                for (const TfToken& token : attr->GetAllowedTokens()) p.allowedTokens.push_back(token.GetString());
            } else {
                p.relationship = true;
            }
            c.properties.push_back(std::move(p));
        }
        // By name: USD writes them so, whatever order they were in.
        std::sort(c.properties.begin(), c.properties.end(),
                  [](const SchemaPropertyDef& a, const SchemaPropertyDef& b) { return a.name < b.name; });
        result.classes.push_back(std::move(c));
    }
    *library = std::move(result);
    return true;
}

bool writeSchemaLibrary(const SchemaLibraryDef& library, const std::string& path, std::string* error) {
    auto fail = [error](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (library.name.empty()) return fail("The library has no name");
    std::error_code ec;
    const std::filesystem::path file = pathOf(path);
    TfErrorMark mark;
    SdfLayerRefPtr layer =
        std::filesystem::is_regular_file(file, ec) ? SdfLayer::OpenAsAnonymous(path) : SdfLayer::CreateAnonymous(".usda");
    if (!layer) return fail(takeError(mark, "It can't be read: " + path));

    layer->SetSubLayerPaths(library.sublayers);
    const SdfPath globalPath = SdfPath::AbsoluteRootPath().AppendChild(TfToken(kGlobal));
    SdfPrimSpecHandle global = layer->GetPrimAtPath(globalPath);
    if (!global) global = SdfPrimSpec::New(layer, kGlobal, SdfSpecifierOver);
    global->SetCustomData("libraryName", VtValue(library.name));
    global->SetCustomData("libraryPath", VtValue(std::string(".")));
    global->SetCustomData("skipCodeGeneration", VtValue(true));
    setCustomData(global, TfToken("libraryPrefix"), library.prefix.empty() ? VtValue() : VtValue(library.prefix));

    std::set<std::string> keep{kGlobal};
    for (const SchemaClassDef& c : library.classes) keep.insert(c.name);
    for (const SdfPrimSpecHandle& prim : layer->GetRootPrims()) {
        if (!keep.count(prim->GetName())) layer->RemoveRootPrim(prim);
    }
    for (const SchemaClassDef& c : library.classes) {
        if (!writeClass(layer, c, error)) return false;
    }
    std::filesystem::create_directories(file.parent_path(), ec);
    if (!layer->Export(path)) return fail(takeError(mark, "It can't be written: " + path));
    mark.Clear();
    return true;
}

bool findSchemaLibrary(const std::string& usdFolder, SchemaLibraryDef* library, std::string* error) {
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(pathOf(usdFolder), ec)) {
        const std::filesystem::path source = entry.path() / "schema.usda";
        if (entry.is_directory(ec) && std::filesystem::is_regular_file(source, ec)) {
            return readSchemaLibrary(usdcc::assets::toUtf8(source), library, error);
        }
    }
    if (error) *error = "No schema library in " + usdFolder;
    return false;
}

std::string libraryTypePrefix(const SchemaLibraryDef& library) {
    if (!library.prefix.empty()) return library.prefix;
    std::string prefix = library.name;
    if (!prefix.empty()) prefix[0] = char(std::toupper(static_cast<unsigned char>(prefix[0])));
    return prefix;
}

bool hasErrors(const std::vector<SchemaIssue>& issues) {
    return std::any_of(issues.begin(), issues.end(),
                       [](const SchemaIssue& issue) { return issue.severity == SchemaIssue::Severity::Error; });
}

std::string issueText(const SchemaIssue& issue) {
    std::string where = issue.schema;
    if (!issue.property.empty()) where += (where.empty() ? "" : ".") + issue.property;
    return where.empty() ? issue.message : where + ": " + issue.message;
}

std::vector<SchemaIssue> checkSchemaLibrary(const SchemaLibraryDef& library, const std::vector<SchemaLibraryDef>& bases) {
    std::vector<SchemaIssue> issues;
    auto add = [&issues](SchemaIssue::Severity severity, const std::string& schema, const std::string& property,
                         const std::string& message) { issues.push_back({severity, schema, property, message}); };
    constexpr auto Error = SchemaIssue::Severity::Error;
    constexpr auto Warning = SchemaIssue::Severity::Warning;

    if (library.name.empty() || !TfIsValidIdentifier(library.name)) {
        add(Error, "", "", "The library's name '" + library.name + "' isn't a valid identifier");
    }
    const std::string prefix = libraryTypePrefix(library);
    std::map<std::string, const SchemaClassDef*> local;
    for (const SchemaClassDef& c : library.classes) {
        if (!local.emplace(c.name, &c).second) add(Error, c.name, "", "There are two classes of this name");
    }
    // The libraries it builds on count as known, as usdGenSchema will find them.
    std::set<std::string> baseNames;
    for (const SchemaLibraryDef& base : bases) {
        baseNames.insert(base.name);
        for (const SchemaClassDef& c : base.classes) local.emplace(c.name, &c);
    }
    auto knownTyped = [&](const std::string& name) {
        if (const auto it = local.find(name); it != local.end()) return !isLibraryAPI(it->second->kind);
        const UsdSchemaRegistry::SchemaInfo* info = UsdSchemaRegistry::FindSchemaInfo(TfToken(name));
        return name == "Typed" || (info && (info->kind == UsdSchemaKind::ConcreteTyped || info->kind == UsdSchemaKind::AbstractTyped));
    };
    auto knownConcrete = [&](const std::string& name) {
        if (const auto it = local.find(name); it != local.end()) return it->second->kind == SchemaInfo::Kind::ConcreteTyped;
        const UsdSchemaRegistry::SchemaInfo* info = UsdSchemaRegistry::FindSchemaInfo(TfToken(name));
        return info && info->kind == UsdSchemaKind::ConcreteTyped;
    };
    auto knownAPI = [&](const std::string& name) {
        const std::string schema = name.substr(0, name.find(':'));
        if (const auto it = local.find(schema); it != local.end()) return isLibraryAPI(it->second->kind);
        return UsdSchemaRegistry::IsAppliedAPISchema(UsdSchemaRegistry::GetAPITypeFromSchemaTypeName(TfToken(schema)));
    };

    for (const SchemaClassDef& c : library.classes) {
        const bool api = isLibraryAPI(c.kind);
        if (!TfIsValidIdentifier(c.name)) {
            add(Error, c.name, "", "'" + c.name + "' isn't a valid identifier");
            continue;
        }
        // Trap 3: a name another plugin has breaks that schema at the first stage.
        if (const std::string plugin = registeredPlugin(c.name);
            !plugin.empty() && plugin != library.name && !baseNames.count(plugin)) {
            add(Error, c.name, "", "The " + plugin + " plugin has a schema of this name already");
        }
        if (const TfType type = TfType::FindByName(prefix + c.name); !type.IsUnknown()) {
            const PlugPluginPtr plugin = PlugRegistry::GetInstance().GetPluginForType(type);
            if (plugin && plugin->GetName() != library.name) {
                add(Error, c.name, "", "Its type " + prefix + c.name + " is the " + plugin->GetName() + " plugin's already");
            }
        }
        if (api && !TfStringEndsWith(c.name, "API")) add(Error, c.name, "", "An API schema's name ends in API");
        if (!api && TfStringEndsWith(c.name, "API")) add(Warning, c.name, "", "A prim type's name ending in API reads as an API schema");
        if (!api && !c.base.empty() && !knownTyped(c.base)) {
            add(Error, c.name, "", "It inherits " + c.base + ", which isn't a prim type USD or this library knows");
        }
        if (c.kind == SchemaInfo::Kind::MultipleApplyAPI && c.propertyNamespacePrefix.empty() && !c.properties.empty()) {
            add(Error, c.name, "", "A multiple-apply API with properties needs a property namespace prefix");
        }
        for (const std::string& builtIn : c.builtInAPIs) {
            if (!knownAPI(builtIn)) add(Error, c.name, "", "It builds in " + builtIn + ", which isn't an API schema USD or this library knows");
        }
        for (const std::string& type : c.autoApplyTo) {
            if (!knownTyped(type)) add(Warning, c.name, "", "It applies itself to " + type + ", which USD doesn't know");
        }
        for (const std::string& type : c.canOnlyApplyTo) {
            if (!knownTyped(type)) add(Warning, c.name, "", "It can only apply to " + type + ", which USD doesn't know");
        }
        if (!api && (!c.autoApplyTo.empty() || !c.canOnlyApplyTo.empty())) {
            add(Warning, c.name, "", "Only an API schema applies itself, or is limited to some types");
        }

        std::set<std::string> names;
        for (const SchemaPropertyDef& p : c.properties) {
            if (!names.insert(p.name).second) add(Error, c.name, p.name, "There are two properties of this name");
            if (!SdfPath::IsValidNamespacedIdentifier(p.name)) {
                add(Error, c.name, p.name, "'" + p.name + "' isn't a valid property name");
                continue;
            }
            if (!p.relationship && !SdfSchema::GetInstance().FindType(p.typeName)) {
                add(Error, c.name, p.name, "No value type '" + p.typeName + "'");
                continue;
            }
            if (!p.relationship && !p.fallback.empty()) {
                std::string why;
                VtValue value;
                if (!parseFallback(p.typeName, p.fallback, &value, &why)) {
                    add(Error, c.name, p.name, why);
                } else if (!p.allowedTokens.empty() && value.IsHolding<TfToken>() &&
                           std::find(p.allowedTokens.begin(), p.allowedTokens.end(),
                                     value.UncheckedGet<TfToken>().GetString()) == p.allowedTokens.end()) {
                    add(Error, c.name, p.name, "Its fallback " + p.fallback + " isn't one of its allowed tokens");
                }
            }
            if (!p.allowedTokens.empty() && p.typeName != "token" && p.typeName != "token[]") {
                add(Warning, c.name, p.name, "Allowed tokens are for a token attribute");
            }
            // Trap 4: USD accepts a property that changes an inherited one's type, in silence.
            if (!api) {
                std::string type, owner;
                if (inheritedPropertyType(local, c.base, p.name, &type, &owner)) {
                    const std::string mine = p.relationship ? std::string("relationship") : p.typeName;
                    if (type != mine) {
                        add(Warning, c.name, p.name, "It's a " + type + " in " + owner + ", which this replaces with a " + mine);
                    }
                }
            }
            // A namespace keeps two studios' "status" apart; a multiple-apply API has its prefix.
            if (c.kind != SchemaInfo::Kind::MultipleApplyAPI && p.name.find(':') == std::string::npos) {
                add(Warning, c.name, p.name, "It has no namespace (" + library.name + ":" + p.name + "), so another schema's could clash with it");
            }
        }
    }
    return issues;
}

bool fillPlugInfoPlaceholders(const std::string& path, std::string* error) {
    std::ifstream in(pathOf(path), std::ios::binary);
    if (!in) {
        if (error) *error = "It can't be read: " + path;
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    in.close();
    std::string json = text.str();
    json = TfStringReplace(json, "@PLUG_INFO_LIBRARY_PATH@", "");
    json = TfStringReplace(json, "@PLUG_INFO_RESOURCE_PATH@", ".");
    json = TfStringReplace(json, "@PLUG_INFO_ROOT@", ".");
    std::ofstream out(pathOf(path), std::ios::binary | std::ios::trunc);
    out << json;
    if (!out) {
        if (error) *error = "It can't be written: " + path;
        return false;
    }
    return true;
}

}  // namespace usdcc::usd
